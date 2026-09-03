// The one state holder. Compose reads it, the editor view writes to it, and every
// action the UI can take is a method here.
//
// Design rule: the UI never talks to CopeNative, never touches a handle, and never
// decides a colour. It reads state and calls methods.
package dev.cope.ide

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import dev.cope.ide.core.AssetIndex
import dev.cope.ide.core.CopeDocument
import dev.cope.ide.core.CopeEngine
import dev.cope.ide.core.CopeNative
import dev.cope.ide.core.Inspection
import dev.cope.ide.core.ThemeSnapshot
import dev.cope.ide.core.Tier
import dev.cope.ide.editor.CopeEditorView
import dev.cope.ide.settings.Prefs
import dev.cope.ide.storage.Storage
import dev.cope.ide.storage.StorageMode
import dev.cope.ide.theme.CopeColors
import dev.cope.ide.theme.CopeFonts
import dev.cope.ide.theme.Derive
import dev.cope.ide.theme.UiKeys
import java.nio.charset.StandardCharsets

/** How a tab renders its document. */
public enum class ViewMode { EDIT, PREVIEW, SPLIT }

/** Which sheet page is showing. */
public enum class SheetTab { FILES, INSPECTOR, TERMINAL }

/** Sheet height, snapped. */
public enum class SheetSnap { CLOSED, HALF, FULL }

/** One notice at a time, highest severity first. Never "something went wrong". */
public data class Notice(
    public val text: String,
    public val actionLabel: String? = null,
    public val severity: Severity = Severity.INFO,
    public val action: (() -> Unit)? = null,
) {
    public enum class Severity { INFO, WARN, ERROR }
}

/** Modal surfaces. Exactly one can be open. */
public sealed interface Overlay {
    /** The per-file menu. `target` is explicit so opening it can never switch tabs. */
    public data class FileMenu(public val tabIndex: Int) : Overlay
    public data class TreeMenu(public val path: String, public val isDirectory: Boolean) : Overlay
    public data object AppSettings : Overlay
    public data object Palette : Overlay
    public data object ThemePicker : Overlay
    public data object GoToLine : Overlay
    public data class Rename(public val tabIndex: Int) : Overlay
    public data class CloseConfirm(public val tabIndex: Int) : Overlay
    public data class Scopes(public val scopes: List<String>, public val token: String) : Overlay
    public data object OpenFiles : Overlay

    /** Long-press inside the editor. A real menu with real actions, not three. */
    public data object EditorMenu : Overlay

    /** ZArchiver's properties dialog: measured facts, no decoration. */
    public data class Properties(public val path: String) : Overlay
    public data class NewEntry(public val directory: String, public val folder: Boolean) : Overlay
    public data class RenamePath(public val path: String) : Overlay
    public data class DeleteConfirm(public val path: String) : Overlay
}

public class Tab(
    public val document: CopeDocument,
    title: String,
    directory: String,
    /** SAF document this tab saves back to, when it did not come from a real path. */
    uri: Uri?,
) {
    // Title and directory are observable state: rename and "save as" both move a
    // live tab to a new name while its edits and caret stay put.
    public var title: String by mutableStateOf(title)
    public var directory: String by mutableStateOf(directory)
    public var uri: Uri? = uri

    /** Caret, anchor, scrollY, scrollX — restored when the tab comes back. */
    public var editorState: LongArray = longArrayOf(0, 0, 0, 0)
    public var mode: ViewMode by mutableStateOf(ViewMode.EDIT)

    public val isMarkdown: Boolean
        get() = title.endsWith(".md", true) || title.endsWith(".markdown", true)
}

public class AppState(private val context: Context) {

    public val prefs: Prefs = Prefs(context)

    public var engine: CopeEngine? = null
        private set
    public var index: AssetIndex = AssetIndex.EMPTY
        private set
    public var fonts: CopeFonts = CopeFonts.PLATFORM
        private set

    /** Set when the app cannot work at all; the UI shows the real reason. */
    public var fatal: String? by mutableStateOf(null)
        private set

    public var colors: CopeColors by mutableStateOf(CopeColors.FALLBACK)
        private set
    public var palette: IntArray by mutableStateOf(IntArray(3))
        private set
    public var themeName: String by mutableStateOf("")
        private set
    public var themeFile: String? by mutableStateOf(null)
        private set

    public val tabs: MutableList<Tab> = mutableStateListOf()
    public var active: Int by mutableIntStateOf(-1)
        private set

    public var sheetTab: SheetTab by mutableStateOf(SheetTab.FILES)
    public var sheetSnap: SheetSnap by mutableStateOf(SheetSnap.CLOSED)
    public var overlay: Overlay? by mutableStateOf(null)
    public var notice: Notice? by mutableStateOf(null)
    public var findOpen: Boolean by mutableStateOf(false)
    public var keyboardUp: Boolean by mutableStateOf(false)

    /** Bumped whenever the document or caret changes, to force recomposition. */
    public var revision: Int by mutableIntStateOf(0)
        private set

    public var caretLine: Int by mutableIntStateOf(0)
        private set
    public var caretColumn: Int by mutableIntStateOf(0)
        private set
    public var selectionBytes: Long by mutableStateOf(0L)
        private set

    public var editor: CopeEditorView? = null

    // AppState cannot launch an activity result contract, so MainActivity installs
    // these and the whole app asks through requestOpenDocument/requestSaveAs.
    public var onPickDocument: (() -> Unit)? = null
    public var onCreateDocument: ((String) -> Unit)? = null

    /** Tab waiting for a "save as" destination, or -1. */
    private var saveAsTab: Int = -1

    // File tree
    public var treeRoot: String by mutableStateOf(Storage.defaultRoot())
        private set
    public var treePath: String by mutableStateOf(Storage.defaultRoot())
        private set
    /** Expanded directories in the tree. A list, because Compose observes lists. */
    public val treeExpanded: MutableList<String> = mutableStateListOf()

    public var storageMode: StorageMode by mutableStateOf(StorageMode.SAF_ONLY)
        private set

    // Find state
    public var findQuery: String by mutableStateOf("")
    public var findCaseSensitive: Boolean by mutableStateOf(false)
    public var findWholeWord: Boolean by mutableStateOf(false)
    public var findMatches: Long by mutableStateOf(0L)
        private set
    public var replaceQuery: String by mutableStateOf("")

    public val activeTab: Tab? get() = tabs.getOrNull(active)

    // --- startup ------------------------------------------------------------

    public fun start() {
        if (!CopeNative.available) {
            fatal = "libcope_jni.so did not load. This build has no native library for this " +
                "device's ABI — install the arm64-v8a or armeabi-v7a APK."
            return
        }
        val assets = context.assets
        fonts = CopeFonts.load(assets)
        index = AssetIndex.parse(
            grammarsTsv = readAsset("index/grammars.tsv"),
            themesTsv = readAsset("index/themes.tsv"),
            defaultsTsv = readAsset("index/defaults.tsv"),
        )
        if (index.grammarIndexTsv.isEmpty()) {
            fatal = "assets/index/grammars.tsv is missing from the APK. " +
                "Run tools/gen_asset_index.py and rebuild."
            return
        }
        val created = CopeEngine.create(assets, index)
        if (created == null) {
            fatal = "The native engine refused to start. Nothing can be opened."
            return
        }
        engine = created
        storageMode = Storage.mode(context)
        applyTheme(prefs.themeFile ?: index.defaultDark)
        treeRoot = prefs.lastFolder ?: Storage.defaultRoot()
        treePath = treeRoot
        val crash = CopeApp.takeLastCrash(context)
        if (crash != null) {
            // The previous run died. Say what it was, once, and never again.
            notice = Notice(
                "The last session ended in a crash: $crash",
                actionLabel = "Copy",
                severity = Notice.Severity.ERROR,
                action = { copyText("crash", crash) },
            )
        } else if (!fonts.bundled) {
            notice = Notice(
                "JetBrains Mono is not in this build — using the platform monospace. " +
                    "Italic and bold token styles will look flat.",
                severity = Notice.Severity.INFO,
            )
        }
    }

    // --- settings -----------------------------------------------------------
    //
    // Every editor preference is written through here so the persisted value and
    // the live view can never disagree.

    public fun setEditorSp(sp: Int) {
        prefs.editorSp = sp
        editor?.textSizeSp = prefs.editorSp
        bump()
    }

    public fun setTabWidth(width: Int) {
        prefs.tabWidth = width
        editor?.tabWidth = prefs.tabWidth
        editor?.invalidateContent()
        syncCaret()
        bump()
    }

    public fun setIndentGuides(on: Boolean) {
        prefs.indentGuides = on
        editor?.showIndentGuides = on
        bump()
    }

    public fun setShowWhitespace(on: Boolean) {
        prefs.showWhitespace = on
        editor?.showWhitespace = on
        bump()
    }

    public fun setHaptics(on: Boolean) {
        prefs.haptics = on
        editor?.hapticsEnabled = on
        bump()
    }

    private fun readAsset(path: String): String = try {
        context.assets.open(path).use { String(it.readBytes(), StandardCharsets.UTF_8) }
    } catch (error: java.io.IOException) {
        ""
    }

    public fun shutdown() {
        engine?.close()
        engine = null
        tabs.clear()
        active = -1
    }

    // --- theme --------------------------------------------------------------

    public fun applyTheme(file: String?) {
        val engine = this.engine ?: return
        val entry = index.themeByFile(file)
            ?: index.themeByFile(index.defaultDark)
            ?: index.themes.firstOrNull()
            ?: return
        val snapshot: ThemeSnapshot? = engine.applyTheme(entry, UiKeys.ALL)
        if (snapshot == null) {
            notice = Notice(
                "Theme \"${entry.name}\" could not be parsed. Keeping the previous one.",
                severity = Notice.Severity.WARN,
            )
            return
        }
        colors = Derive.from(snapshot)
        palette = snapshot.palette
        themeName = entry.name
        themeFile = entry.file
        prefs.themeFile = entry.file
        editor?.let {
            it.colors = colors
            it.palette = palette
            it.invalidateContent()
        }
        bump()
    }

    /** Refreshes the palette after any theme-affecting change. */
    public fun refreshPalette() {
        val engine = this.engine ?: return
        val snapshot = engine.snapshot(UiKeys.ALL) ?: return
        colors = Derive.from(snapshot)
        palette = snapshot.palette
        editor?.let {
            it.colors = colors
            it.palette = palette
            it.invalidateContent()
        }
        bump()
    }

    // --- opening / closing --------------------------------------------------

    public fun openPath(path: String) {
        val engine = this.engine ?: return
        val existing = tabs.indexOfFirst { it.document.path == path }
        if (existing >= 0) {
            switchTab(existing)
            return
        }
        val stat = engine.stat(path)
        if (stat == null) {
            notice = Notice("$path does not exist or cannot be read.", severity = Notice.Severity.ERROR)
            return
        }
        if (stat.isDirectory) {
            treePath = path
            return
        }
        val document = engine.openPath(path)
        if (document == null) {
            notice = Notice(
                "Could not open $path. It exists but the file could not be mapped — " +
                    "check storage permission.",
                severity = Notice.Severity.ERROR,
            )
            return
        }
        prefs.noteRecent(path)
        addTab(Tab(document, path.substringAfterLast('/'), path.substringBeforeLast('/', "") + "/", null))
    }

    public fun openUri(uri: Uri) {
        val engine = this.engine ?: return
        // Prefer a real path: that is the only way the file gets mmapped instead of
        // copied into the heap.
        val real = Storage.realPathOf(uri)
        if (real != null && storageMode == StorageMode.ALL_FILES) {
            openPath(real)
            return
        }
        val size = Storage.sizeOf(context, uri)
        if (size > Storage.SAF_BYTE_LIMIT) {
            notice = Notice(
                "${Storage.displayName(context, uri)} is ${Storage.humanSize(size)}. " +
                    "Files opened through the system picker are read into memory, and the limit " +
                    "is ${Storage.humanSize(Storage.SAF_BYTE_LIMIT)}. Grant all-files access to " +
                    "open it directly.",
                actionLabel = "Grant",
                severity = Notice.Severity.WARN,
                action = { requestAllFilesAccess() },
            )
            return
        }
        val bytes = Storage.readAll(context, uri)
        if (bytes == null) {
            notice = Notice(
                "Could not read ${Storage.displayName(context, uri)} from the system picker.",
                severity = Notice.Severity.ERROR,
            )
            return
        }
        Storage.persist(context, uri)
        val name = Storage.displayName(context, uri)
        val document = engine.openBytes(name, bytes) ?: return
        addTab(Tab(document, name, "", uri))
    }

    public fun newBuffer() {
        val engine = this.engine ?: return
        val document = engine.openBytes("untitled.txt", ByteArray(0)) ?: return
        addTab(Tab(document, "untitled.txt", "", null))
    }

    /** The system picker. Always available, with or without storage permission. */
    public fun requestOpenDocument() {
        val pick = onPickDocument
        if (pick == null) {
            notice = Notice(
                "The system file picker is not available in this window.",
                severity = Notice.Severity.ERROR,
            )
            return
        }
        overlay = null
        pick()
    }

    /** Asks the system for a destination and remembers which tab wanted it. */
    public fun requestSaveAs(tabIndex: Int = active) {
        val tab = tabs.getOrNull(tabIndex) ?: return
        val create = onCreateDocument
        if (create == null) {
            notice = Notice(
                "The system file picker is not available in this window.",
                severity = Notice.Severity.ERROR,
            )
            return
        }
        saveAsTab = tabIndex
        overlay = null
        create(tab.title)
    }

    /** Result of [requestSaveAs]: writes the bytes and retargets the tab. */
    public fun completeSaveAs(uri: Uri) {
        val tab = tabs.getOrNull(saveAsTab) ?: activeTab ?: return
        saveAsTab = -1
        // A real path is worth taking: it turns the tab back into an mmapped file
        // instead of one that round-trips through the ContentResolver on every save.
        val real = Storage.realPathOf(uri)
        if (real != null && storageMode == StorageMode.ALL_FILES && tab.document.save(real)) {
            tab.uri = null
            tab.title = real.substringAfterLast('/')
            tab.directory = real.substringBeforeLast('/', "") + "/"
            prefs.noteRecent(real)
            notice = null
            bump()
            return
        }
        if (!Storage.writeAll(context, uri, tab.document.bytes())) {
            notice = Notice(
                "Could not write to the location you chose.",
                severity = Notice.Severity.ERROR,
            )
            return
        }
        Storage.persist(context, uri)
        tab.uri = uri
        tab.title = Storage.displayName(context, uri)
        tab.directory = ""
        tab.document.markSaved()
        notice = null
        bump()
    }

    private fun addTab(tab: Tab) {
        tabs.add(tab)
        switchTab(tabs.size - 1)
        noticeForTier(tab)
    }

    /** The honest-limits notice: says what happened and offers the real action. */
    private fun noticeForTier(tab: Tab) {
        val document = tab.document
        when {
            document.tier == Tier.PLAIN && document.byteSize > 0 -> {
                notice = Notice(
                    "Highlighting off — ${tab.title} is ${Storage.humanSize(document.byteSize)}, " +
                        "past the safety limit.",
                    actionLabel = "Highlight anyway",
                    severity = Notice.Severity.WARN,
                    action = { forceHighlight(tab) },
                )
            }
            document.tier == Tier.FALLBACK && !document.hasGrammar -> {
                notice = Notice(
                    "No grammar for ${tab.title} — using the heuristic lexer.",
                    severity = Notice.Severity.INFO,
                )
            }
            document.tier == Tier.FALLBACK -> {
                notice = Notice(
                    "The grammar for ${tab.title} scoped almost nothing, so Cope switched to the " +
                        "heuristic lexer.",
                    actionLabel = "Details",
                    severity = Notice.Severity.INFO,
                    action = { openInspector() },
                )
            }
            else -> notice = null
        }
    }

    private fun forceHighlight(tab: Tab) {
        tab.document.forceTier(1)
        editor?.invalidateContent()
        notice = if (tab.document.tier == Tier.PLAIN) {
            Notice(
                "Still off: ${tab.title} has no usable grammar, so there is nothing to force.",
                severity = Notice.Severity.WARN,
            )
        } else {
            null
        }
        bump()
    }

    public fun switchTab(indexToShow: Int) {
        if (indexToShow < 0 || indexToShow >= tabs.size) return
        val view = editor
        tabs.getOrNull(active)?.let { previous ->
            if (view != null) previous.editorState = view.captureState()
        }
        active = indexToShow
        val tab = tabs[indexToShow]
        if (view != null) {
            view.document = tab.document
            view.restoreState(tab.editorState)
        }
        overlay = null
        syncCaret()
        bump()
    }

    public fun requestClose(indexToClose: Int) {
        val tab = tabs.getOrNull(indexToClose) ?: return
        tab.document.refresh()
        if (tab.document.dirty) {
            overlay = Overlay.CloseConfirm(indexToClose)
            return
        }
        closeTab(indexToClose)
    }

    public fun closeTab(indexToClose: Int) {
        val tab = tabs.getOrNull(indexToClose) ?: return
        // Save the live editor state into whatever tab currently owns it *before*
        // the indices shift, then forget the active index so switchTab cannot write
        // state into the wrong tab.
        val view = editor
        if (view != null) tabs.getOrNull(active)?.editorState = view.captureState()
        val previousActive = active
        active = -1
        tab.document.close()
        tabs.removeAt(indexToClose)
        overlay = null
        if (tabs.isEmpty()) {
            editor?.document = null
            syncCaret()
            bump()
            return
        }
        val next = when {
            previousActive > indexToClose -> previousActive - 1
            previousActive >= tabs.size -> tabs.size - 1
            else -> previousActive
        }
        switchTab(next.coerceIn(0, tabs.size - 1))
        bump()
    }

    // --- saving -------------------------------------------------------------

    public fun save(tab: Tab? = activeTab): Boolean {
        val target = tab ?: return false
        val uri = target.uri
        if (uri != null) {
            val ok = Storage.writeAll(context, uri, target.document.bytes())
            if (ok) {
                target.document.markSaved()
                notice = null
            } else {
                notice = Notice(
                    "Could not write ${target.title} back through the system picker. " +
                        "The app may have lost permission to that document.",
                    severity = Notice.Severity.ERROR,
                )
            }
            bump()
            return ok
        }
        val path = target.document.path
        if (path == null) {
            notice = Notice(
                "${target.title} has never been saved, so there is nowhere to write it yet.",
                actionLabel = "Choose a location",
                severity = Notice.Severity.WARN,
                action = { requestSaveAs(tabs.indexOf(target)) },
            )
            return false
        }
        val ok = target.document.save(path)
        if (!ok) {
            notice = Notice(
                "Could not write $path. Storage permission or a read-only location.",
                severity = Notice.Severity.ERROR,
                actionLabel = if (storageMode != StorageMode.ALL_FILES) "Grant" else null,
                action = if (storageMode != StorageMode.ALL_FILES) {
                    { requestAllFilesAccess() }
                } else {
                    null
                },
            )
        } else {
            notice = null
        }
        bump()
        return ok
    }

    public fun requestAllFilesAccess() {
        val intent = Storage.allFilesAccessIntent(context)
        if (intent == null) {
            notice = Notice(
                "This Android version grants storage access from the system permission prompt, " +
                    "not from settings.",
                severity = Notice.Severity.INFO,
            )
            return
        }
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            context.startActivity(intent)
        } catch (error: android.content.ActivityNotFoundException) {
            notice = Notice(
                "This device has no all-files access screen. Use the system file picker instead.",
                severity = Notice.Severity.INFO,
            )
        }
    }

    public fun refreshStorageMode() {
        storageMode = Storage.mode(context)
    }

    // --- file operations ----------------------------------------------------
    //
    // These use java.io.File through Storage rather than the engine: they only run
    // in ALL_FILES mode (the tree cannot browse otherwise) and the Host interface
    // deliberately has no mutating directory API.

    public fun renameEntry(path: String, newName: String) {
        val result = Storage.rename(path, newName)
        val failure = result.error
        if (failure != null) {
            notice = Notice(failure, severity = Notice.Severity.ERROR)
            return
        }
        val moved = result.path ?: return
        for (tab in tabs) {
            if (tab.document.path == path) {
                tab.document.path = moved
                tab.title = moved.substringAfterLast('/')
                tab.directory = moved.substringBeforeLast('/', "") + "/"
            }
        }
        if (treePath == path) setTreePath(moved)
        treeExpanded.remove(path)
        overlay = null
        notice = null
        bump()
    }

    /** Renames the file behind a tab, keeping the tab, its caret and its edits. */
    public fun renameTab(tabIndex: Int, newName: String) {
        val tab = tabs.getOrNull(tabIndex) ?: return
        val path = tab.document.path
        if (path == null) {
            // An unsaved buffer has no file to rename, so this is just a retitle.
            tab.title = newName
            overlay = null
            bump()
            return
        }
        renameEntry(path, newName)
    }

    public fun createEntry(directory: String, name: String, folder: Boolean) {
        val result = Storage.create(directory, name, folder)
        val failure = result.error
        if (failure != null) {
            notice = Notice(failure, severity = Notice.Severity.ERROR)
            return
        }
        overlay = null
        val created = result.path
        when {
            created == null -> Unit
            folder -> if (!treeExpanded.contains(created)) treeExpanded.add(created)
            else -> openPath(created)
        }
        bump()
    }

    public fun deleteEntry(path: String) {
        val error = Storage.delete(path)
        overlay = null
        if (error != null) {
            notice = Notice(error, severity = Notice.Severity.ERROR)
            return
        }
        val open = tabs.indexOfFirst { it.document.path == path }
        if (open >= 0) closeTab(open)
        treeExpanded.remove(path)
        notice = Notice("Deleted ${path.substringAfterLast('/')}.")
        bump()
    }

    // --- clipboard ----------------------------------------------------------

    private val clipboard: ClipboardManager? =
        context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager

    public fun copyText(label: String, text: String): Boolean {
        val manager = clipboard
        if (manager == null || text.isEmpty()) return false
        return try {
            manager.setPrimaryClip(ClipData.newPlainText(label, text))
            true
        } catch (error: RuntimeException) {
            // Some OEM clipboard services throw on very large payloads.
            notice = Notice(
                "The system clipboard refused ${text.length} characters.",
                severity = Notice.Severity.WARN,
            )
            false
        }
    }

    public fun clipboardText(): String? {
        val manager = clipboard ?: return null
        val clip = manager.primaryClip ?: return null
        if (clip.itemCount == 0) return null
        return clip.getItemAt(0)?.coerceToText(context)?.toString()
    }

    public fun selectedText(): String {
        val view = editor ?: return ""
        val document = activeTab?.document ?: return ""
        if (!view.hasSelection) return ""
        return document.textRangeString(view.selectionStart, view.selectionEnd - view.selectionStart)
    }

    public fun copySelection() {
        val text = selectedText()
        if (text.isEmpty()) return
        if (copyText("cope", text)) notice = null
    }

    public fun cutSelection() {
        val text = selectedText()
        if (text.isEmpty()) return
        if (!copyText("cope", text)) return
        editor?.deleteSelection()
        onEditorChanged()
    }

    public fun paste() {
        val text = clipboardText()
        if (text.isNullOrEmpty()) {
            notice = Notice("The clipboard is empty.")
            return
        }
        editor?.insertText(text)
        onEditorChanged()
    }

    // --- file tree ----------------------------------------------------------

    public fun setTreePath(path: String) {
        treePath = if (path.isEmpty()) "/" else path
        prefs.lastFolder = treePath
    }

    public fun toggleExpanded(path: String) {
        if (!treeExpanded.remove(path)) treeExpanded.add(path)
    }

    // --- editor bridge ------------------------------------------------------

    public fun syncCaret() {
        val view = editor
        val document = activeTab?.document
        if (view == null || document == null) {
            // No live editor: everything the status strip and the selection bar read
            // has to go back to zero, or closing the last tab leaves a stale
            // "27 B selected" on screen.
            caretLine = 0
            caretColumn = 1
            selectionBytes = 0L
            return
        }
        val position = document.lineColumnOf(view.caret)
        caretLine = position[0].toInt()
        caretColumn = document.displayColumnOf(view.caret, prefs.tabWidth) + 1
        selectionBytes = if (view.hasSelection) view.selectionEnd - view.selectionStart else 0L
    }

    public fun bump() {
        revision++
    }

    /**
     * The editor's change hook. Caret and selection always refresh, but `revision`
     * only bumps when the document version actually moved: a fling would otherwise
     * recompose every chrome row on every frame.
     */
    public fun onEditorChanged() {
        syncCaret()
        val version = activeTab?.document?.version ?: 0L
        if (version != lastSeenVersion) {
            lastSeenVersion = version
            bump()
        }
    }

    private var lastSeenVersion: Long = -1L

    public fun applyEditorPrefs() {
        val view = editor ?: return
        view.textSizeSp = prefs.editorSp
        view.tabWidth = prefs.tabWidth
        view.showIndentGuides = prefs.indentGuides
        view.showWhitespace = prefs.showWhitespace
        view.hapticsEnabled = prefs.haptics
        view.colors = colors
        view.palette = palette
        view.fonts = fonts
    }

    /** The persisted size after a pinch-zoom. The view already applied it. */
    public fun noteEditorSp(sp: Int) {
        prefs.editorSp = sp
    }

    /**
     * Back-press precedence, stated once: overlay, find bar, sheet, selection,
     * notice. False means nothing was open and the system should handle it.
     */
    public fun dismissTopmost(): Boolean {
        if (overlay != null) {
            overlay = null
            return true
        }
        if (findOpen) {
            findOpen = false
            return true
        }
        if (sheetSnap != SheetSnap.CLOSED) {
            sheetSnap = SheetSnap.CLOSED
            return true
        }
        val view = editor
        if (view != null && view.hasSelection) {
            view.setCaret(view.caret, extend = false)
            syncCaret()
            return true
        }
        if (notice != null) {
            notice = null
            return true
        }
        return false
    }

    /**
     * Whether [dismissTopmost] would do anything. Kept separate so the back
     * handler can be disabled entirely and let the system close the app, rather
     * than swallowing the gesture and looking broken.
     */
    public fun canDismiss(): Boolean =
        overlay != null ||
            findOpen ||
            sheetSnap != SheetSnap.CLOSED ||
            selectionBytes > 0 ||
            notice != null

    // --- panels -------------------------------------------------------------

    public fun openInspector() {
        sheetTab = SheetTab.INSPECTOR
        if (sheetSnap == SheetSnap.CLOSED) sheetSnap = SheetSnap.HALF
        overlay = null
    }

    /** Raises the sheet to at least half height without changing which page shows. */
    public fun openSheet() {
        if (sheetSnap == SheetSnap.CLOSED) sheetSnap = SheetSnap.HALF
    }

    public fun inspect(): Inspection = activeTab?.document?.inspect() ?: Inspection.NONE

    /** Opens the file tree at a path's folder, raising the sheet if it is closed. */
    public fun revealInFiles(path: String) {
        val directory = path.substringBeforeLast('/', "")
        setTreePath(if (directory.isEmpty()) "/" else directory)
        sheetTab = SheetTab.FILES
        if (sheetSnap == SheetSnap.CLOSED) sheetSnap = SheetSnap.HALF
        overlay = null
        bump()
    }

    public fun showScopesAtCaret() {
        val view = editor ?: return
        val document = activeTab?.document ?: return
        val position = document.lineColumnOf(view.caret)
        val scopes = document.scopesAt(position[0].toInt(), position[1].toInt())
        val token = document.textRangeString(view.caret, minOf(24L, document.byteSize - view.caret))
            .takeWhile { it != '\n' }
        overlay = Overlay.Scopes(scopes, token)
    }

    // --- find ---------------------------------------------------------------

    private fun searchFlags(): Int =
        (if (findCaseSensitive) 1 else 0) or (if (findWholeWord) 2 else 0)

    public fun findNext(backwards: Boolean = false) {
        val view = editor ?: return
        val document = activeTab?.document ?: return
        if (findQuery.isEmpty()) return
        val from = if (backwards) view.selectionStart else view.selectionEnd
        var hit = document.find(findQuery, from, searchFlags(), backwards)
        if (hit == null) {
            // Wrap, and say so: silently doing nothing at the end of a file is the
            // most common "search is broken" complaint.
            hit = document.find(
                findQuery,
                if (backwards) document.byteSize else 0L,
                searchFlags(),
                backwards,
            )
            if (hit == null) {
                notice = Notice("No matches for \"$findQuery\".", severity = Notice.Severity.INFO)
                findMatches = 0
                return
            }
            notice = Notice("Wrapped to the ${if (backwards) "end" else "start"} of the file.")
        }
        view.setSelection(hit[0], hit[0] + hit[1])
        view.ensureCaretVisible()
        countMatches()
        syncCaret()
        bump()
    }

    public fun countMatches() {
        val document = activeTab?.document ?: return
        findMatches = if (findQuery.isEmpty()) {
            0
        } else {
            (document.findAll(findQuery, searchFlags(), MATCH_CAP).size / 2).toLong()
        }
    }

    public fun replaceAll() {
        val document = activeTab?.document ?: return
        if (findQuery.isEmpty()) return
        val count = document.replaceAll(findQuery, replaceQuery, searchFlags())
        editor?.invalidateContent()
        notice = when {
            count < 0 -> Notice(
                "Refused: replacing every match would rewrite more than 8 MB in one edit, " +
                    "which would make undo unusable.",
                severity = Notice.Severity.WARN,
            )
            count == 0L -> Notice("No matches for \"$findQuery\".")
            else -> Notice("Replaced $count occurrence${if (count == 1L) "" else "s"}.")
        }
        countMatches()
        bump()
    }

    public fun goToLine(line: Int) {
        val view = editor ?: return
        val document = activeTab?.document ?: return
        val clamped = (line - 1).coerceIn(0, document.lineCount - 1)
        view.setCaret(document.offsetOf(clamped, 0), extend = false)
        view.scrollToLine(maxOf(0, clamped - 3))
        overlay = null
        syncCaret()
        bump()
    }

    private companion object {
        const val MATCH_CAP = 2000
    }
}

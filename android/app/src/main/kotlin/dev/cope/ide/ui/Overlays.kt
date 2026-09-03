// Every modal surface: menus, dialogs, the command palette, the theme picker.
//
// Two rules from the design brief are enforced structurally here, not by taste:
//
//  1. A MENU TARGET IS ALWAYS A PARAMETER. The mockup's openMenu() set
//     `active = i` before rendering, so opening the ⋮ menu on an inactive file
//     silently switched the editor behind the scrim. Every composable below takes
//     the thing it acts on as an argument and never touches AppState.active.
//  2. NO DEAD END. Every destructive or refusable action offers the way forward:
//     closing a dirty file is Save / Discard / Cancel, not a toast; a failed
//     rename says which name collided; a folder that will not delete says how
//     many items are still in it.
@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package dev.cope.ide.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import dev.cope.ide.AppState
import dev.cope.ide.Notice
import dev.cope.ide.Overlay
import dev.cope.ide.SheetTab
import dev.cope.ide.ViewMode
import dev.cope.ide.storage.Storage
import dev.cope.ide.storage.StorageMode
import dev.cope.ide.theme.CopeDimens
import dev.cope.ide.theme.LocalCopeColors

/** Dispatches the one open overlay. Nothing else in the app renders a modal. */
@Composable
public fun Overlays(state: AppState) {
    val overlay = state.overlay ?: return
    val dismiss = { state.overlay = null }
    when (overlay) {
        is Overlay.FileMenu -> FileMenu(state, overlay.tabIndex, dismiss)
        is Overlay.TreeMenu -> TreeMenu(state, overlay.path, overlay.isDirectory, dismiss)
        Overlay.AppSettings -> AppSettings(state, dismiss)
        Overlay.Palette -> CommandPalette(state, dismiss)
        Overlay.ThemePicker -> ThemePicker(state, dismiss)
        Overlay.GoToLine -> GoToLine(state, dismiss)
        Overlay.OpenFiles -> OpenFiles(state, dismiss)
        Overlay.EditorMenu -> EditorMenu(state, dismiss)
        is Overlay.Rename -> RenameTabDialog(state, overlay.tabIndex, dismiss)
        is Overlay.RenamePath -> RenamePathDialog(state, overlay.path, dismiss)
        is Overlay.CloseConfirm -> CloseConfirm(state, overlay.tabIndex, dismiss)
        is Overlay.Scopes -> ScopesDialog(overlay.scopes, overlay.token, state, dismiss)
        is Overlay.Properties -> PropertiesDialog(overlay.path, state, dismiss)
        is Overlay.NewEntry -> NewEntryDialog(state, overlay.directory, overlay.folder, dismiss)
        is Overlay.DeleteConfirm -> DeleteConfirmDialog(state, overlay.path, dismiss)
    }
}

/**
 * A menu anchored under the top chrome. Not centred: it hangs off the ⋮ that
 * opened it, which is what makes it reachable at any sheet snap.
 */
@Composable
private fun AnchoredMenu(
    onDismiss: () -> Unit,
    content: @Composable () -> Unit,
) {
    val colors = LocalCopeColors.current
    Box(Modifier.fillMaxSize()) {
        Scrim(onDismiss)
        Column(
            Modifier
                .align(Alignment.TopEnd)
                .padding(top = 4.dp, end = 4.dp)
                .width(268.dp)
                .heightIn(max = 460.dp)
                .background(Color(colors.menuBg))
                .border(1.dp, Color(colors.border))
                .verticalScroll(rememberScrollState()),
        ) {
            content()
        }
    }
}

// --- the per-file menu -----------------------------------------------------

@Composable
private fun FileMenu(state: AppState, tabIndex: Int, onDismiss: () -> Unit) {
    val tab = state.tabs.getOrNull(tabIndex)
    if (tab == null) {
        onDismiss()
        return
    }
    val document = tab.document
    AnchoredMenu(onDismiss) {
        MenuHeader(
            title = tab.title,
            subtitle = buildString {
                append(if (tab.directory.isEmpty()) "not on disk" else tab.directory)
                append(" · ")
                append(Storage.humanSize(document.byteSize))
                append(" · ")
                append(document.lineCount)
                append(" lines")
            },
        )
        // Save is a real menu item. In the mockup the only save affordance was a
        // 7px dot in the info bar, which is not a target.
        MenuItem(
            text = "Save",
            onClick = { state.save(tab); onDismiss() },
            value = if (document.dirty) "unsaved" else "up to date",
            enabled = document.dirty,
            icon = Icon.SAVE,
        )
        MenuItem(
            text = "Save as…",
            onClick = { state.requestSaveAs(tabIndex) },
        )
        MenuItem(
            text = "Rename…",
            onClick = { state.overlay = Overlay.Rename(tabIndex) },
        )
        MenuSeparator()
        MenuItem(
            text = "Find in file",
            onClick = { state.findOpen = true; state.countMatches(); onDismiss() },
            icon = Icon.SEARCH,
        )
        MenuItem(
            text = "Go to line…",
            onClick = { state.overlay = Overlay.GoToLine },
            value = "1–${document.lineCount}",
        )
        MenuItem(
            text = "Select all",
            onClick = { state.editor?.selectAll(); state.onEditorChanged(); onDismiss() },
        )
        MenuSeparator()
        if (tab.isMarkdown) {
            MenuItem(
                text = "View mode",
                onClick = {
                    tab.mode = nextMode(tab.mode)
                    state.bump()
                },
                value = tab.mode.name.lowercase(),
                icon = Icon.PREVIEW,
            )
        }
        MenuItem(
            text = "Inspect highlighting",
            onClick = { state.openInspector() },
            value = "tier ${document.tier.label}",
            icon = Icon.INFO,
        )
        MenuItem(
            text = "Show scopes at caret",
            onClick = { state.showScopesAtCaret() },
        )
        if (document.path != null) {
            val filePath = document.path ?: ""
            MenuItem(
                text = "Reveal in Files",
                onClick = { state.revealInFiles(filePath) },
                icon = Icon.FOLDER,
                enabled = state.storageMode == StorageMode.ALL_FILES,
                value = if (state.storageMode == StorageMode.ALL_FILES) null else "needs access",
            )
            MenuItem(
                text = "Copy path",
                onClick = {
                    if (state.copyText("path", filePath)) {
                        state.notice = Notice("Copied $filePath")
                    }
                    onDismiss()
                },
            )
            MenuItem(
                text = "Properties",
                onClick = { state.overlay = Overlay.Properties(filePath) },
                icon = Icon.LIST,
            )
        }
        MenuSeparator()
        MenuItem(
            text = "Close file",
            onClick = { onDismiss(); state.requestClose(tabIndex) },
            icon = Icon.CLOSE,
            destructive = document.dirty,
            value = if (document.dirty) "will ask first" else null,
        )
    }
}

private fun nextMode(mode: ViewMode): ViewMode = when (mode) {
    ViewMode.EDIT -> ViewMode.PREVIEW
    ViewMode.PREVIEW -> ViewMode.SPLIT
    ViewMode.SPLIT -> ViewMode.EDIT
}

// --- the file-tree menu ----------------------------------------------------

@Composable
private fun TreeMenu(
    state: AppState,
    path: String,
    isDirectory: Boolean,
    onDismiss: () -> Unit,
) {
    val openTab = state.tabs.indexOfFirst { it.document.path == path }
    AnchoredMenu(onDismiss) {
        MenuHeader(
            title = path.substringAfterLast('/'),
            subtitle = path.substringBeforeLast('/', "/"),
        )
        if (isDirectory) {
            MenuItem("Open folder", { state.setTreePath(path); onDismiss() }, icon = Icon.FOLDER)
            MenuItem(
                text = if (state.treeExpanded.contains(path)) "Collapse" else "Expand",
                onClick = { state.toggleExpanded(path); onDismiss() },
            )
            MenuSeparator()
            MenuItem("New file here…", { state.overlay = Overlay.NewEntry(path, false) }, icon = Icon.PLUS)
            MenuItem("New folder here…", { state.overlay = Overlay.NewEntry(path, true) })
        } else if (openTab >= 0) {
            MenuItem("Switch to this file", { state.switchTab(openTab); onDismiss() }, icon = Icon.FILE)
            MenuItem(
                text = "Save",
                onClick = { state.save(state.tabs[openTab]); onDismiss() },
                enabled = state.tabs[openTab].document.dirty,
                value = if (state.tabs[openTab].document.dirty) "unsaved" else "up to date",
            )
            MenuItem("Close file", { onDismiss(); state.requestClose(openTab) })
        } else {
            MenuItem("Open", { state.openPath(path); onDismiss() }, icon = Icon.FILE)
        }
        MenuSeparator()
        MenuItem("Rename…", { state.overlay = Overlay.RenamePath(path) }, icon = Icon.EDIT)
        MenuItem(
            text = "Copy path",
            onClick = {
                if (state.copyText("path", path)) state.notice = Notice("Copied $path")
                onDismiss()
            },
        )
        MenuItem("Properties", { state.overlay = Overlay.Properties(path) }, icon = Icon.LIST)
        MenuSeparator()
        MenuItem(
            text = "Delete",
            onClick = { state.overlay = Overlay.DeleteConfirm(path) },
            icon = Icon.TRASH,
            destructive = true,
            value = if (isDirectory) "empty folders only" else null,
        )
    }
}

// --- the editor's long-press menu ------------------------------------------

@Composable
private fun EditorMenu(state: AppState, onDismiss: () -> Unit) {
    val view = state.editor
    val hasSelection = view?.hasSelection == true
    val clipboardHas = remember { state.clipboardText()?.isNotEmpty() == true }
    AnchoredMenu(onDismiss) {
        MenuHeader(
            title = if (hasSelection) "${state.selectionBytes} bytes selected" else "At the caret",
            subtitle = "Ln ${state.caretLine + 1}, Col ${state.caretColumn}",
        )
        MenuItem("Cut", { state.cutSelection(); onDismiss() }, enabled = hasSelection)
        MenuItem("Copy", { state.copySelection(); onDismiss() }, enabled = hasSelection)
        MenuItem(
            text = "Paste",
            onClick = { state.paste(); onDismiss() },
            enabled = clipboardHas,
            value = if (clipboardHas) null else "clipboard empty",
        )
        MenuItem("Select all", { view?.selectAll(); state.onEditorChanged(); onDismiss() })
        MenuSeparator()
        MenuItem(
            text = "Find this text",
            onClick = {
                val selected = state.selectedText().takeWhile { it != '\n' }
                if (selected.isNotEmpty()) {
                    state.findQuery = selected
                    state.findOpen = true
                    state.countMatches()
                }
                onDismiss()
            },
            enabled = hasSelection,
            icon = Icon.SEARCH,
        )
        MenuItem("Go to line…", { state.overlay = Overlay.GoToLine })
        MenuSeparator()
        // This is trace.h surfaced. It is the tool that would have saved the five
        // previous attempts at this project, so it gets a permanent menu slot.
        MenuItem("Show scopes here", { state.showScopesAtCaret() }, icon = Icon.INFO)
        MenuItem("Inspect highlighting", { state.openInspector() })
    }
}

// --- app settings ----------------------------------------------------------

@Composable
private fun AppSettings(state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    val prefs = state.prefs
    AnchoredMenu(onDismiss) {
        MenuHeader("Settings", "Applies to every open file")
        MenuItem(
            text = "Theme",
            onClick = { state.overlay = Overlay.ThemePicker },
            value = state.themeName.ifEmpty { "none" },
            icon = Icon.PALETTE,
        )
        StepperRow(
            text = "Editor text size",
            value = "${prefs.editorSp}sp",
            onDecrease = { state.setEditorSp(prefs.editorSp - 1) },
            onIncrease = { state.setEditorSp(prefs.editorSp + 1) },
            canDecrease = prefs.editorSp > CopeDimens.EDITOR_SP_MIN,
            canIncrease = prefs.editorSp < CopeDimens.EDITOR_SP_MAX,
        )
        StepperRow(
            text = "Indent width",
            value = "${prefs.tabWidth}",
            onDecrease = { state.setTabWidth(prefs.tabWidth - 1) },
            onIncrease = { state.setTabWidth(prefs.tabWidth + 1) },
            canDecrease = prefs.tabWidth > 1,
            canIncrease = prefs.tabWidth < 16,
        )
        ToggleRow(
            text = "Indent guides",
            checked = prefs.indentGuides,
            onToggle = { state.setIndentGuides(it) },
        )
        ToggleRow(
            text = "Show tabs",
            checked = prefs.showWhitespace,
            onToggle = { state.setShowWhitespace(it) },
            detail = "Marks a tab character with a dash so mixed indentation is visible",
        )
        ToggleRow(
            text = "Haptics",
            checked = prefs.haptics,
            onToggle = { state.setHaptics(it) },
            detail = "A tick on long-press and on the sheet's snap points",
        )
        MenuSeparator()
        MenuItem(
            text = "Storage access",
            onClick = {
                if (state.storageMode == StorageMode.ALL_FILES) {
                    state.notice = Notice(
                        "All-files access is granted, so files open by mmap and the tree can " +
                            "browse folders.",
                    )
                    onDismiss()
                } else {
                    state.requestAllFilesAccess()
                }
            },
            value = if (state.storageMode == StorageMode.ALL_FILES) "granted" else "picker only",
        )
        MenuItem(
            text = "Font",
            onClick = {
                state.notice = Notice(
                    if (state.fonts.bundled) {
                        "JetBrains Mono is bundled, with real bold and italic faces."
                    } else {
                        "JetBrains Mono is not in this build, so the platform monospace is in " +
                            "use and italic token styles are synthesised."
                    },
                )
                onDismiss()
            },
            value = if (state.fonts.bundled) "JetBrains Mono" else "platform mono",
        )
        MenuSeparator()
        // Facts, not an about screen. No tagline, no version-number vanity.
        Column(Modifier.padding(horizontal = 14.dp, vertical = 9.dp)) {
            Label(
                "${state.index.themes.size} themes · ${state.index.extensionToScope.size} " +
                    "file extensions mapped to grammars",
                colors.dim,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                maxLines = 3,
            )
        }
    }
}

// --- theme picker ----------------------------------------------------------

@Composable
private fun ThemePicker(state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    var query by remember { mutableStateOf("") }
    var darkOnly by remember { mutableStateOf(state.colors.isDark) }
    val all = state.index.themes
    val shown = remember(query, darkOnly, all) {
        all.filter { it.isDark == darkOnly && it.name.contains(query, ignoreCase = true) }
            .sortedByDescending { it.score }
    }
    Box(Modifier.fillMaxSize()) {
        Scrim(onDismiss)
        Column(
            Modifier
                .align(Alignment.Center)
                .fillMaxWidth(0.94f)
                .heightIn(max = 520.dp)
                .background(Color(colors.menuBg))
                .border(1.dp, Color(colors.border)),
        ) {
            MenuHeader("Theme", "Tap to apply. The whole app follows the theme, not just tokens.")
            Row(
                Modifier.fillMaxWidth().padding(10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CopeField(
                    value = query,
                    onValueChange = { query = it },
                    placeholder = "filter",
                    modifier = Modifier.weight(1f),
                )
                PillButton(if (darkOnly) "dark" else "light", { darkOnly = !darkOnly })
            }
            HDivider(colors.border)
            if (shown.isEmpty()) {
                EmptyState("No ${if (darkOnly) "dark" else "light"} theme matches \"$query\".")
            }
            LazyColumn(Modifier.fillMaxWidth().weight(1f, fill = false)) {
                items(shown, key = { it.file }) { entry ->
                    val selected = entry.file == state.themeFile
                    DenseRow(
                        selected = selected,
                        onClick = { state.applyTheme(entry.file) },
                    ) {
                        Swatch(
                            background = entry.backgroundArgb,
                            foreground = entry.foregroundArgb,
                            accent = entry.accentArgb,
                            borderColor = colors.border,
                        )
                        Label(
                            entry.name,
                            if (selected) colors.listActiveFg else colors.surfaceFg,
                            bold = selected,
                            modifier = Modifier.weight(1f),
                        )
                        // uiCovered is the honest quality signal: how many of the 40
                        // chrome keys the theme actually defines. Below ~20 the
                        // derivation layer is inventing most of the app's colours.
                        Label(
                            "${entry.uiCovered}/40",
                            if (entry.uiCovered >= 24) colors.dim else colors.warning,
                            sizeSp = CopeDimens.TEXT_TINY_SP,
                        )
                        if (selected) CopeIconGlyph(Icon.CHECK, colors.accent, sizeDp = 14)
                        HSpace(6)
                    }
                }
            }
            HDivider(colors.border)
            DialogActions {
                Label(
                    "${shown.size} shown",
                    colors.dim,
                    sizeSp = CopeDimens.TEXT_TINY_SP,
                    modifier = Modifier.weight(1f),
                )
                PillButton("Done", onDismiss, emphasised = true)
            }
        }
    }
}

// --- command palette -------------------------------------------------------

private class Command(
    val label: String,
    val hint: String,
    val perform: () -> Unit,
)

/**
 * One fuzzy list over commands, open tabs, recent files and the current folder.
 * It replaces menu depth rather than adding to it, which is the whole reason it
 * is in v1 (decision 4).
 *
 * It deliberately does NOT index the whole device: no workspace file index exists
 * yet, and a recursive scan of /sdcard on a phone would take seconds. The header
 * says which sources it searched, so an absent file is explained rather than
 * mysterious.
 */
@Composable
private fun CommandPalette(state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    var query by remember { mutableStateOf("") }

    val commands = remember(state.tabs.size, state.active, state.revision) {
        buildCommands(state, onDismiss)
    }
    val files = remember(state.treePath, state.revision) { paletteFiles(state) }

    val goToLine = query.startsWith(":") && query.length > 1 && query.drop(1).all { it.isDigit() }
    val matchedCommands = remember(query, commands) {
        if (goToLine) {
            emptyList()
        } else {
            commands.map { it to fuzzyScore(it.label, query) }
                .filter { it.second > 0 }
                .sortedByDescending { it.second }
                .take(12)
                .map { it.first }
        }
    }
    val matchedFiles = remember(query, files) {
        if (goToLine) {
            emptyList()
        } else {
            files.map { it to fuzzyScore(it.second, query) }
                .filter { it.second > 0 }
                .sortedByDescending { it.second }
                .take(20)
                .map { it.first }
        }
    }

    Box(Modifier.fillMaxSize()) {
        Scrim(onDismiss)
        Column(
            Modifier
                .align(Alignment.TopCenter)
                .padding(top = 24.dp)
                .fillMaxWidth(0.96f)
                .heightIn(max = 520.dp)
                .background(Color(colors.menuBg))
                .border(1.dp, Color(colors.border)),
        ) {
            Box(Modifier.padding(10.dp)) {
                CopeField(
                    value = query,
                    onValueChange = { query = it },
                    placeholder = "command, file, or :line",
                    autoFocus = true,
                    modifier = Modifier.fillMaxWidth(),
                    onSubmit = {
                        when {
                            goToLine -> state.goToLine(query.drop(1).toIntOrNull() ?: 1)
                            matchedCommands.isNotEmpty() -> matchedCommands[0].perform()
                            matchedFiles.isNotEmpty() -> {
                                state.openPath(matchedFiles[0].first)
                                onDismiss()
                            }
                        }
                    },
                )
            }
            HDivider(colors.border)
            if (goToLine) {
                val line = query.drop(1).toIntOrNull() ?: 1
                val total = state.activeTab?.document?.lineCount ?: 0
                DenseRow(onClick = { state.goToLine(line) }) {
                    CopeIconGlyph(Icon.ARROW_RIGHT, colors.accent, sizeDp = 14)
                    Label("Go to line $line", colors.surfaceFg, modifier = Modifier.weight(1f))
                    Label("of $total", colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
                    HSpace(8)
                }
                return@Column
            }
            if (matchedCommands.isEmpty() && matchedFiles.isEmpty()) {
                EmptyState(
                    "Nothing matches \"$query\". The palette searches commands, open files, " +
                        "recent files and the folder shown in Files — not the whole device.",
                )
                return@Column
            }
            LazyColumn(Modifier.fillMaxWidth().weight(1f, fill = false)) {
                items(matchedCommands.size) { index ->
                    val command = matchedCommands[index]
                    DenseRow(onClick = command.perform) {
                        CopeIconGlyph(Icon.CHEVRON_RIGHT, colors.dim, sizeDp = 12)
                        Label(command.label, colors.surfaceFg, modifier = Modifier.weight(1f))
                        Label(command.hint, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
                        HSpace(8)
                    }
                }
                items(matchedFiles.size) { index ->
                    val (path, name) = matchedFiles[index]
                    DenseRow(onClick = { state.openPath(path); onDismiss() }) {
                        CopeIconGlyph(Icon.FILE, colors.dim, sizeDp = 13)
                        Label(name, colors.surfaceFg, modifier = Modifier.weight(1f))
                        Label(
                            shortenTailOf(path.substringBeforeLast('/', "")),
                            colors.dim,
                            sizeSp = CopeDimens.TEXT_TINY_SP,
                        )
                        HSpace(8)
                    }
                }
            }
        }
    }
}

private fun buildCommands(state: AppState, onDismiss: () -> Unit): List<Command> {
    val commands = ArrayList<Command>(20)
    commands += Command("New file", "buffer") { state.newBuffer(); onDismiss() }
    commands += Command("Open file…", "system picker") { state.requestOpenDocument() }
    commands += Command("Save", "current file") { state.save(); onDismiss() }
    commands += Command("Save as…", "choose a location") { state.requestSaveAs() }
    commands += Command("Find in file", "top bar") {
        state.findOpen = true
        state.countMatches()
        onDismiss()
    }
    commands += Command("Go to line…", "or type :123") { state.overlay = Overlay.GoToLine }
    commands += Command("Theme…", "${state.index.themes.size} bundled") {
        state.overlay = Overlay.ThemePicker
    }
    commands += Command("Settings", "whole app") { state.overlay = Overlay.AppSettings }
    commands += Command("Files panel", "sheet") {
        state.sheetTab = SheetTab.FILES
        state.openSheet()
        onDismiss()
    }
    commands += Command("Inspector", "tier, coverage, refused rules") { state.openInspector() }
    commands += Command("Show scopes at caret", "grammar debugger") { state.showScopesAtCaret() }
    commands += Command("Undo", "editor") {
        state.editor?.undo()
        state.onEditorChanged()
        onDismiss()
    }
    commands += Command("Redo", "editor") {
        state.editor?.redo()
        state.onEditorChanged()
        onDismiss()
    }
    commands += Command("Select all", "editor") {
        state.editor?.selectAll()
        state.onEditorChanged()
        onDismiss()
    }
    commands += Command("Close file", "current tab") {
        onDismiss()
        state.requestClose(state.active)
    }
    for ((index, tab) in state.tabs.withIndex()) {
        if (index == state.active) continue
        commands += Command("Switch to ${tab.title}", "open tab") {
            state.switchTab(index)
            onDismiss()
        }
    }
    return commands
}

/** Open tabs, recent files, then the current tree folder. Deduplicated by path. */
private fun paletteFiles(state: AppState): List<Pair<String, String>> {
    val seen = LinkedHashMap<String, String>()
    for (path in state.prefs.recentFiles) {
        seen.putIfAbsent(path, path.substringAfterLast('/'))
    }
    if (state.storageMode == StorageMode.ALL_FILES) {
        val engine = state.engine
        if (engine != null) {
            for (entry in engine.listDir(state.treePath)) {
                if (entry.isDirectory) continue
                val path = if (state.treePath.endsWith('/')) {
                    "${state.treePath}${entry.name}"
                } else {
                    "${state.treePath}/${entry.name}"
                }
                seen.putIfAbsent(path, entry.name)
            }
        }
    }
    return seen.entries.map { it.key to it.value }
}

/**
 * Subsequence match, scored — see ui/Fuzzy.kt. Kept out of this file so the
 * scoring is unit-testable without loading a Compose-compiled class.
 */
private fun shortenTailOf(path: String): String = shortenTail(path)

// --- open files ------------------------------------------------------------

@Composable
private fun OpenFiles(state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    Box(Modifier.fillMaxSize()) {
        Scrim(onDismiss)
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .heightIn(max = 440.dp)
                .background(Color(colors.menuBg))
                .border(1.dp, Color(colors.border)),
        ) {
            MenuHeader("Open files", "${state.tabs.size} open")
            LazyColumn(Modifier.fillMaxWidth().weight(1f, fill = false)) {
                itemsIndexed(state.tabs) { index, tab ->
                    DenseRow(
                        selected = index == state.active,
                        onClick = { state.switchTab(index); onDismiss() },
                        onLongClick = { state.overlay = Overlay.FileMenu(index) },
                    ) {
                        CopeIconGlyph(Icon.FILE, colors.dim, sizeDp = 13)
                        if (tab.document.dirty) DirtyDot(colors.accent, sizeDp = 6)
                        Label(
                            tab.title,
                            if (index == state.active) colors.listActiveFg else colors.surfaceFg,
                            bold = index == state.active,
                            modifier = Modifier.weight(1f),
                        )
                        Label(
                            Storage.humanSize(tab.document.byteSize),
                            colors.dim,
                            sizeSp = CopeDimens.TEXT_TINY_SP,
                        )
                        IconButton(
                            icon = Icon.CLOSE,
                            description = "Close ${tab.title}",
                            onClick = { state.requestClose(index) },
                            tint = colors.dim,
                            sizeDp = 12,
                            touchDp = 40,
                        )
                    }
                }
            }
            HDivider(colors.border)
            DialogActions {
                PillButton("New file", { state.newBuffer(); onDismiss() })
                PillButton("Open…", { state.requestOpenDocument() }, emphasised = true)
            }
        }
    }
}

// --- dialogs ---------------------------------------------------------------

@Composable
private fun GoToLine(state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    val total = state.activeTab?.document?.lineCount ?: 1
    var text by remember { mutableStateOf("") }
    val line = text.toIntOrNull()
    DialogFrame(onDismiss) {
        MenuHeader("Go to line", "1 to $total")
        Box(Modifier.padding(12.dp)) {
            CopeField(
                value = text,
                onValueChange = { new -> text = new.filter { it.isDigit() }.take(9) },
                placeholder = "line number",
                autoFocus = true,
                numeric = true,
                modifier = Modifier.fillMaxWidth(),
                onSubmit = { if (line != null) state.goToLine(line) },
            )
        }
        if (line != null && line > total) {
            Label(
                "This file has $total lines, so $line clamps to the last one.",
                colors.warning,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                maxLines = 2,
                modifier = Modifier.padding(horizontal = 12.dp),
            )
        }
        DialogActions {
            PillButton("Cancel", onDismiss)
            PillButton(
                text = "Go",
                onClick = { if (line != null) state.goToLine(line) },
                emphasised = line != null,
            )
        }
    }
}

@Composable
private fun RenameTabDialog(state: AppState, tabIndex: Int, onDismiss: () -> Unit) {
    val tab = state.tabs.getOrNull(tabIndex)
    if (tab == null) {
        onDismiss()
        return
    }
    NameDialog(
        title = "Rename",
        subtitle = tab.document.path ?: "not saved yet — this only changes the tab name",
        initial = tab.title,
        confirmLabel = "Rename",
        onDismiss = onDismiss,
        onConfirm = { state.renameTab(tabIndex, it) },
    )
}

@Composable
private fun RenamePathDialog(state: AppState, path: String, onDismiss: () -> Unit) {
    NameDialog(
        title = "Rename",
        subtitle = path.substringBeforeLast('/', "/"),
        initial = path.substringAfterLast('/'),
        confirmLabel = "Rename",
        onDismiss = onDismiss,
        onConfirm = { state.renameEntry(path, it) },
    )
}

@Composable
private fun NewEntryDialog(
    state: AppState,
    directory: String,
    folder: Boolean,
    onDismiss: () -> Unit,
) {
    NameDialog(
        title = if (folder) "New folder" else "New file",
        subtitle = directory,
        initial = "",
        confirmLabel = "Create",
        onDismiss = onDismiss,
        onConfirm = { state.createEntry(directory, it, folder) },
    )
}

/** The one name-entry dialog. Keeps the base name selected-ish and validates live. */
@Composable
private fun NameDialog(
    title: String,
    subtitle: String,
    initial: String,
    confirmLabel: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    val colors = LocalCopeColors.current
    var text by remember { mutableStateOf(initial) }
    val trimmed = text.trim()
    val problem = when {
        trimmed.isEmpty() -> null
        trimmed.any { it in "/\\:*?\"<>|" } -> "A name cannot contain / \\ : * ? \" < > |"
        trimmed == "." || trimmed == ".." -> "That is a directory reference, not a name."
        else -> null
    }
    DialogFrame(onDismiss) {
        MenuHeader(title, subtitle)
        Box(Modifier.padding(12.dp)) {
            CopeField(
                value = text,
                onValueChange = { text = it },
                placeholder = "name",
                autoFocus = true,
                modifier = Modifier.fillMaxWidth(),
                onSubmit = { if (trimmed.isNotEmpty() && problem == null) onConfirm(trimmed) },
            )
        }
        if (problem != null) {
            Label(
                problem,
                colors.error,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                maxLines = 2,
                modifier = Modifier.padding(horizontal = 12.dp),
            )
        }
        DialogActions {
            PillButton("Cancel", onDismiss)
            PillButton(
                text = confirmLabel,
                onClick = { if (trimmed.isNotEmpty() && problem == null) onConfirm(trimmed) },
                emphasised = trimmed.isNotEmpty() && problem == null,
            )
        }
    }
}

/**
 * Three buttons, because there are three things a person might mean. The mockup
 * refused this with a toast and no way forward, which was bug 3 of the port.
 */
@Composable
private fun CloseConfirm(state: AppState, tabIndex: Int, onDismiss: () -> Unit) {
    val tab = state.tabs.getOrNull(tabIndex)
    if (tab == null) {
        onDismiss()
        return
    }
    DialogFrame(onDismiss) {
        MenuHeader(
            "${tab.title} has unsaved changes",
            if (tab.document.path != null || tab.uri != null) {
                "Closing without saving discards them."
            } else {
                "This buffer was never saved anywhere, so discarding loses it entirely."
            },
        )
        DialogActions {
            PillButton("Cancel", onDismiss)
            PillButton("Discard", { state.closeTab(tabIndex) })
            PillButton(
                text = "Save",
                onClick = { if (state.save(tab)) state.closeTab(tabIndex) },
                emphasised = true,
            )
        }
    }
}

@Composable
private fun DeleteConfirmDialog(state: AppState, path: String, onDismiss: () -> Unit) {
    val facts = remember(path) { Storage.properties(path) }
    DialogFrame(onDismiss) {
        MenuHeader(
            "Delete ${path.substringAfterLast('/')}?",
            "This cannot be undone. Android has no trash for app-deleted files.",
        )
        Column(Modifier.padding(horizontal = 14.dp, vertical = 6.dp)) {
            for ((key, value) in facts) FactRow(key, value)
        }
        DialogActions {
            PillButton("Cancel", onDismiss)
            PillButton("Delete", { state.deleteEntry(path) }, emphasised = true)
        }
    }
}

/** ZArchiver's properties dialog: real facts, and nothing that is not a fact. */
@Composable
private fun PropertiesDialog(path: String, state: AppState, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    val facts = remember(path) { Storage.properties(path) }
    val tab = state.tabs.firstOrNull { it.document.path == path }
    DialogFrame(onDismiss) {
        MenuHeader("Properties", path)
        Column(
            Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState()).padding(14.dp),
        ) {
            for ((key, value) in facts) FactRow(key, value)
            if (tab != null) {
                val document = tab.document
                HDivider(colors.border, Modifier.padding(vertical = 6.dp))
                FactRow("open", "yes")
                FactRow("lines", document.lineCount.toString())
                FactRow("unsaved edits", if (document.dirty) "yes" else "no")
                FactRow("highlight tier", document.tier.label)
                FactRow("language", state.index.languageOf(tab.title))
                FactRow("mapped", if (document.inMemory) "no — read into memory" else "yes")
            }
        }
        DialogActions {
            PillButton("Copy path", { state.copyText("path", path) })
            PillButton("Close", onDismiss, emphasised = true)
        }
    }
}

@Composable
private fun FactRow(key: String, value: String) {
    val colors = LocalCopeColors.current
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Label(key, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP, modifier = Modifier.width(96.dp))
        Label(value, colors.menuFg, sizeSp = CopeDimens.TEXT_SMALL_SP, maxLines = 3)
    }
}

/**
 * The scope stack under the caret, outermost first. This is the diagnostic that
 * explains why a token has the colour it has — and when it is empty, that is the
 * answer: nothing scoped this byte.
 */
@Composable
private fun ScopesDialog(
    scopes: List<String>,
    token: String,
    state: AppState,
    onDismiss: () -> Unit,
) {
    val colors = LocalCopeColors.current
    DialogFrame(onDismiss) {
        MenuHeader(
            "Scopes at Ln ${state.caretLine + 1}, Col ${state.caretColumn}",
            if (token.isEmpty()) "end of file" else "at \"${token.take(24)}\"",
        )
        if (scopes.isEmpty()) {
            EmptyState(
                "No scope covers this byte. Under tier ${state.activeTab?.document?.tier?.label} " +
                    "that means the grammar left it unclaimed and the fallback lexer did not " +
                    "repair it either — it renders in the default colour.",
                actionLabel = "Inspect the file",
                onAction = { state.openInspector() },
            )
        } else {
            Column(
                Modifier.heightIn(max = 340.dp).verticalScroll(rememberScrollState()).padding(12.dp),
            ) {
                for ((depth, scope) in scopes.withIndex()) {
                    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                        Box(Modifier.width((depth * 10).dp))
                        Label(
                            scope,
                            if (depth == scopes.size - 1) colors.accent else colors.menuFg,
                            sizeSp = CopeDimens.TEXT_SMALL_SP,
                            bold = depth == scopes.size - 1,
                            maxLines = 2,
                        )
                    }
                }
                Label(
                    "The innermost scope wins, per attribute. A theme rule matching a shorter " +
                        "prefix can still supply the colour if the innermost one sets only " +
                        "fontStyle.",
                    colors.dim,
                    sizeSp = CopeDimens.TEXT_TINY_SP,
                    maxLines = 4,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        }
        DialogActions {
            PillButton("Copy", { state.copyText("scopes", scopes.joinToString("\n")) })
            PillButton("Done", onDismiss, emphasised = true)
        }
    }
}

// --- find bar --------------------------------------------------------------

/**
 * Top-anchored, never sheet content (decision 2): the sheet collapses under the
 * keyboard, so a find field living there could never be on screen at the same
 * time as the IME typing into it.
 */
@Composable
public fun FindBar(state: AppState) {
    val colors = LocalCopeColors.current
    var replaceOpen by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().background(Color(colors.surface))) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(CopeDimens.FIND_BAR_HEIGHT.dp)
                .padding(horizontal = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            CopeField(
                value = state.findQuery,
                onValueChange = {
                    state.findQuery = it
                    state.countMatches()
                },
                placeholder = "find",
                autoFocus = true,
                modifier = Modifier.weight(1f),
                onSubmit = { state.findNext() },
            )
            Box(Modifier.width(58.dp), contentAlignment = Alignment.Center) {
                Label(
                    text = when {
                        state.findQuery.isEmpty() -> ""
                        state.findMatches == 0L -> "none"
                        state.findMatches >= MATCH_DISPLAY_CAP -> "${MATCH_DISPLAY_CAP}+"
                        else -> "${state.findMatches}"
                    },
                    color = if (state.findMatches == 0L && state.findQuery.isNotEmpty()) {
                        colors.warning
                    } else {
                        colors.dim
                    },
                    sizeSp = CopeDimens.TEXT_TINY_SP,
                )
            }
            IconButton(
                icon = Icon.ARROW_UP,
                description = "Previous match",
                onClick = { state.findNext(backwards = true) },
                enabled = state.findQuery.isNotEmpty(),
                sizeDp = 14,
                touchDp = 40,
            )
            IconButton(
                icon = Icon.ARROW_DOWN,
                description = "Next match",
                onClick = { state.findNext() },
                enabled = state.findQuery.isNotEmpty(),
                sizeDp = 14,
                touchDp = 40,
            )
            IconButton(
                icon = Icon.CLOSE,
                description = "Close find",
                onClick = { state.findOpen = false },
                tint = colors.dim,
                sizeDp = 13,
                touchDp = 40,
            )
        }
        Row(
            Modifier.fillMaxWidth().padding(start = 6.dp, end = 6.dp, bottom = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            ToggleChip("Aa", state.findCaseSensitive) {
                state.findCaseSensitive = !state.findCaseSensitive
                state.countMatches()
            }
            ToggleChip("word", state.findWholeWord) {
                state.findWholeWord = !state.findWholeWord
                state.countMatches()
            }
            ToggleChip("replace", replaceOpen) { replaceOpen = !replaceOpen }
        }
        if (replaceOpen) {
            Row(
                Modifier.fillMaxWidth().padding(start = 6.dp, end = 6.dp, bottom = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                CopeField(
                    value = state.replaceQuery,
                    onValueChange = { state.replaceQuery = it },
                    placeholder = "replace with",
                    modifier = Modifier.weight(1f),
                )
                PillButton(
                    text = "All",
                    onClick = { state.replaceAll() },
                    emphasised = state.findQuery.isNotEmpty(),
                )
            }
        }
        HDivider(colors.border)
    }
}

private const val MATCH_DISPLAY_CAP = 2000

@Composable
private fun ToggleChip(text: String, on: Boolean, onClick: () -> Unit) {
    val colors = LocalCopeColors.current
    Box(
        Modifier
            .height(26.dp)
            .background(Color(if (on) colors.accent else colors.keyBg))
            .border(1.dp, Color(colors.border))
            .clickable(onClick = onClick)
            .padding(horizontal = 9.dp),
        contentAlignment = Alignment.Center,
    ) {
        Label(
            text,
            if (on) contrastOn(colors.accent) else colors.keyFg,
            sizeSp = CopeDimens.TEXT_TINY_SP,
        )
    }
}

// --- selection action bar --------------------------------------------------

/**
 * Floats above the key row while a selection exists. Cut/copy/paste need to be
 * one tap, not a long-press away, because the platform's own action bar cannot
 * attach to a custom View's selection.
 */
@Composable
public fun SelectionBar(state: AppState) {
    val colors = LocalCopeColors.current
    Row(
        Modifier
            .fillMaxWidth()
            .height(SELECTION_BAR_HEIGHT)
            .background(Color(colors.menuBg))
            .padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Label(
            "${state.selectionBytes} B",
            colors.dim,
            sizeSp = CopeDimens.TEXT_TINY_SP,
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        PillButton("Cut", { state.cutSelection() })
        PillButton("Copy", { state.copySelection() })
        PillButton("Paste", { state.paste() })
        Box(Modifier.weight(1f))
        IconButton(
            icon = Icon.MORE_VERTICAL,
            description = "More actions",
            onClick = { state.overlay = Overlay.EditorMenu },
            tint = colors.menuFg,
            sizeDp = 14,
            touchDp = 36,
        )
        IconButton(
            icon = Icon.CLOSE,
            description = "Clear selection",
            onClick = {
                val view = state.editor
                if (view != null) view.setCaret(view.caret, extend = false)
                state.onEditorChanged()
            },
            tint = colors.dim,
            sizeDp = 12,
            touchDp = 36,
        )
    }
}

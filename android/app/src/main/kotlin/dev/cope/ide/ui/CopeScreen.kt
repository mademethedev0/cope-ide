// The root layout: the one place the anchoring rules are enforced.
//
// ORDER, top to bottom, and why each one is where it is
// ----------------------------------------------------
//   notice strip     0 or 1, precedence ordered, dismissable
//   info bar         breadcrumb, dirty/save, undo/redo, mode, ⋮, ⚙
//   find bar         only when open; TOP anchored, because the sheet collapses
//                    under the keyboard and a find field there could never be on
//                    screen at the same time as the IME typing into it
//   editor           the only element that gives up space
//   status strip     hidden while the IME is up (it is information, not action)
//   selection bar    only while a selection exists
//   grip + tab strip thumb zone; the grip drags the sheet
//   sheet            Files / Inspector / Terminal, measured snap heights
//   key row          only while the IME is up
//
// INSETS, and the deliberate deviation from the phase-4 notes
// ----------------------------------------------------------
// The notes said "enableEdgeToEdge + imePadding". That is right on API 30+ and
// wrong on 27-29: WindowCompat.setDecorFitsSystemWindows(false) sets
// SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN there, and that flag is exactly what breaks
// windowSoftInputMode=adjustResize on those versions — the window stops resizing
// and the keyboard covers the bottom chrome, which is where every touch target in
// this design lives. So: edge-to-edge is enabled on API 30+ only (MainActivity),
// and this file pads with WindowInsets.safeDrawing, which reads 0 on the older
// path because the decor view already consumed the insets there. One layout, both
// worlds, and the IME never covers the key row.
//
// Keyboard visibility is likewise two signals OR'd together (see imeVisible):
// the ime inset on the new path, a visible-frame measurement on the old one.
@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package dev.cope.ide.ui

import android.graphics.Rect
import android.os.Build
import android.view.ViewTreeObserver
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import dev.cope.ide.AppState
import dev.cope.ide.Notice
import dev.cope.ide.Overlay
import dev.cope.ide.SheetSnap
import dev.cope.ide.ViewMode
import dev.cope.ide.editor.CopeEditorView
import dev.cope.ide.editor.EditorCallbacks
import dev.cope.ide.markdown.MarkdownStream
import dev.cope.ide.markdown.MarkdownView
import dev.cope.ide.storage.StorageMode
import dev.cope.ide.theme.CopeDimens
import dev.cope.ide.theme.LocalCopeColors
import kotlin.math.abs

@Composable
public fun CopeScreen(state: AppState) {
    val colors = LocalCopeColors.current
    val fatal = state.fatal
    if (fatal != null) {
        FatalScreen(fatal)
        return
    }

    val imeUp = imeVisible()
    // keyboardUp is read by the key row and by KeyRow's escape action; keeping it
    // on AppState means no composable has to thread it through.
    LaunchedEffect(imeUp) { state.keyboardUp = imeUp }

    var keyPage by remember { mutableIntStateOf(0) }
    // The snap to come back to when the keyboard closes. Dragging the sheet while
    // the keyboard is up replaces it, per the anchoring rule.
    var restoreSnap by remember { mutableStateOf(SheetSnap.CLOSED) }
    LaunchedEffect(imeUp) {
        if (imeUp) {
            if (state.sheetSnap != SheetSnap.CLOSED) {
                restoreSnap = state.sheetSnap
                state.sheetSnap = SheetSnap.CLOSED
            }
        } else if (restoreSnap != SheetSnap.CLOSED && state.sheetSnap == SheetSnap.CLOSED) {
            state.sheetSnap = restoreSnap
            restoreSnap = SheetSnap.CLOSED
        }
    }

    BackHandler(enabled = state.canDismiss()) { state.dismissTopmost() }

    Box(Modifier.fillMaxSize().background(Color(colors.editorBg))) {
        Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
            val notice = state.notice
            if (notice != null) {
                NoticeStrip(notice) { state.notice = null }
            }
            InfoBar(state)
            if (state.findOpen) {
                FindBar(state)
            }

            // Everything below is measured together, so the sheet's snap heights
            // re-derive whenever the notice strip, the find bar or the key row
            // appears. Nothing here is a hardcoded height.
            BoxWithConstraints(Modifier.fillMaxWidth().weight(1f)) {
                val available = maxHeight
                val fixed = fixedChromeHeight(state, imeUp)
                val flexible = (available - fixed).coerceAtLeast(0.dp)
                val snapFull = flexible.value
                val snapHalf = (flexible.value * 0.45f)
                // The drag gesture reports pixels; every height here is in dp.
                val density = LocalDensity.current.density

                var dragging by remember { mutableStateOf(false) }
                var dragHeight by remember { mutableFloatStateOf(0f) }
                val target = when (state.sheetSnap) {
                    SheetSnap.CLOSED -> 0f
                    SheetSnap.HALF -> snapHalf
                    SheetSnap.FULL -> snapFull
                }
                val animated by animateFloatAsState(
                    targetValue = target,
                    animationSpec = spring(dampingRatio = 0.85f, stiffness = 900f),
                    label = "sheet",
                )
                val sheetHeight = if (dragging) dragHeight else animated

                Column(Modifier.fillMaxSize()) {
                    Box(Modifier.fillMaxWidth().weight(1f, fill = true)) {
                        EditorArea(state)
                    }
                    if (!imeUp) {
                        StatusStrip(state)
                    }
                    if (state.selectionBytes > 0) {
                        SelectionBar(state)
                    }
                    TabStrip(
                        state = state,
                        onHandleDrag = { deltaPx ->
                            if (!dragging) {
                                dragging = true
                                dragHeight = target
                            }
                            // Up is negative in Android's coordinate system, and up
                            // means "more sheet".
                            dragHeight = (dragHeight - deltaPx / density)
                                .coerceIn(0f, snapFull)
                        },
                        onHandleRelease = {
                            if (dragging) {
                                state.sheetSnap = nearestSnap(dragHeight, snapHalf, snapFull)
                                // A deliberate drag replaces whatever the keyboard
                                // would have restored.
                                restoreSnap = SheetSnap.CLOSED
                                dragging = false
                            }
                        },
                    )
                    if (sheetHeight > 0.5f) {
                        Box(
                            Modifier
                                .fillMaxWidth()
                                .height(sheetHeight.dp)
                                .background(Color(colors.surface)),
                        ) {
                            SheetContent(state)
                        }
                    }
                }
            }

            if (imeUp) {
                KeyRow(state, keyPage) { keyPage = it }
            }
        }
        Overlays(state)
    }
}

/**
 * Height of everything inside the flexible region that is not the sheet or the
 * editor. Derived from the same constants the composables use, so the two cannot
 * drift.
 */
private fun fixedChromeHeight(state: AppState, imeUp: Boolean): Dp {
    var total = SHEET_GRIP_HEIGHT + CopeDimens.TAB_HEIGHT.dp
    if (!imeUp && state.activeTab != null) total += CopeDimens.STATUS_HEIGHT.dp + 1.dp
    if (state.selectionBytes > 0) total += SELECTION_BAR_HEIGHT
    return total
}

internal val SHEET_GRIP_HEIGHT: Dp = 10.dp
internal val SELECTION_BAR_HEIGHT: Dp = 38.dp

/** Snaps to whichever of the three heights the release was closest to. */
private fun nearestSnap(height: Float, half: Float, full: Float): SheetSnap {
    val toClosed = abs(height)
    val toHalf = abs(height - half)
    val toFull = abs(height - full)
    return when {
        toClosed <= toHalf && toClosed <= toFull -> SheetSnap.CLOSED
        toHalf <= toFull -> SheetSnap.HALF
        else -> SheetSnap.FULL
    }
}

// --- the editor / preview area ---------------------------------------------

@Composable
private fun EditorArea(state: AppState) {
    val colors = LocalCopeColors.current
    val tab = state.activeTab
    if (tab == null) {
        NoFileOpen(state)
        return
    }
    when (tab.mode) {
        ViewMode.EDIT -> EditorHost(state, Modifier.fillMaxSize())
        ViewMode.PREVIEW -> Preview(state, Modifier.fillMaxSize())
        ViewMode.SPLIT -> Column(Modifier.fillMaxSize()) {
            EditorHost(state, Modifier.fillMaxWidth().weight(1f))
            HDivider(colors.border)
            Preview(state, Modifier.fillMaxWidth().weight(1f))
        }
    }
}

@Composable
private fun EditorHost(state: AppState, modifier: Modifier) {
    AndroidView(
        modifier = modifier,
        factory = { context ->
            CopeEditorView(context).also { view ->
                state.editor = view
                view.callbacks = object : EditorCallbacks {
                    override fun onEditorStateChanged() {
                        state.onEditorChanged()
                    }

                    override fun onSelectionChanged(hasSelection: Boolean) {
                        state.onEditorChanged()
                    }

                    override fun onContextRequested(x: Float, y: Float) {
                        state.overlay = Overlay.EditorMenu
                    }

                    override fun onTextSizeChanged(sp: Int) {
                        state.noteEditorSp(sp)
                    }
                }
                state.applyEditorPrefs()
                val tab = state.activeTab
                if (tab != null) {
                    view.document = tab.document
                    view.restoreState(tab.editorState)
                }
                state.syncCaret()
            }
        },
        update = { view ->
            // Assigning `document` resets the caret and the viewport cache, so it
            // must only happen when the document actually changed. switchTab()
            // already does the swap plus restoreState; this is the attach path and
            // the safety net.
            val tab = state.activeTab
            val document = tab?.document
            if (view.document !== document) {
                view.document = document
                if (tab != null) view.restoreState(tab.editorState)
            }
            if (view.colors !== state.colors) view.colors = state.colors
            if (view.palette !== state.palette) view.palette = state.palette
        },
        onRelease = { view ->
            if (state.editor === view) state.editor = null
        },
    )
}

@Composable
private fun Preview(state: AppState, modifier: Modifier) {
    val tab = state.activeTab ?: return
    val document = tab.document
    // Reparsing on every version bump is one full parse per keystroke in split
    // mode. Acceptable for a README-sized file, and the honest fix is a debounce
    // plus Document::version staleness checking in the background-work phase.
    val blocks = remember(tab, document.version) {
        MarkdownStream.parse(document.markdownStream())
    }
    MarkdownView(
        blocks = blocks,
        modifier = modifier,
        onLink = { href -> openLink(state, href) },
    )
}

/**
 * A relative link is a file next to this one; anything with a scheme is not ours
 * to open. Refusing to launch an intent for http keeps the app off the network.
 */
private fun openLink(state: AppState, href: String) {
    if (href.isEmpty()) return
    if (href.startsWith('#')) {
        state.notice = Notice(
            "In-document anchors are not linked yet — the preview has no heading index.",
        )
        return
    }
    val scheme = href.substringBefore(':', "")
    if (scheme.isNotEmpty() && !href.startsWith("./") && !href.startsWith("../")) {
        state.notice = Notice(
            "$href points outside this file. Cope does not open external links.",
            actionLabel = "Copy link",
            action = { state.copyText("link", href) },
        )
        return
    }
    val base = state.activeTab?.document?.path?.substringBeforeLast('/', "") ?: ""
    if (base.isEmpty()) {
        state.notice = Notice(
            "This file has no folder on disk, so \"$href\" cannot be resolved.",
        )
        return
    }
    state.openPath(normalisePath("$base/${href.substringBefore('#')}"))
}

// --- empty and fatal states ------------------------------------------------

@Composable
private fun NoFileOpen(state: AppState) {
    val colors = LocalCopeColors.current
    Column(
        Modifier.fillMaxSize().padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterVertically),
    ) {
        Label("No file open", colors.editorFg, sizeSp = 15, bold = true)
        Label(
            text = if (state.storageMode == StorageMode.ALL_FILES) {
                "Open one from the Files panel, the system picker, or start an empty buffer."
            } else {
                "Cope can open one file at a time through the system picker. Granting " +
                    "all-files access turns on folder browsing and opens files by mmap instead " +
                    "of reading them into memory."
            },
            color = colors.lineNumber,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            maxLines = 6,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PillButton("Open…", { state.requestOpenDocument() }, emphasised = true)
            PillButton("New file", { state.newBuffer() })
            if (state.storageMode != StorageMode.ALL_FILES) {
                PillButton("Grant access", { state.requestAllFilesAccess() })
            }
        }
        if (state.prefs.recentFiles.isNotEmpty()) {
            Label("Recent", colors.lineNumber, sizeSp = CopeDimens.TEXT_TINY_SP)
            for (path in state.prefs.recentFiles.take(5)) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(32.dp)
                        .clickable { state.openPath(path) },
                    contentAlignment = Alignment.CenterStart,
                ) {
                    Label(path, colors.caret, sizeSp = CopeDimens.TEXT_SMALL_SP)
                }
            }
        }
    }
}

/**
 * The app cannot work. It says exactly why, because "Something went wrong" is
 * explicitly banned by the design brief.
 */
@Composable
private fun FatalScreen(reason: String) {
    val colors = LocalCopeColors.current
    Column(
        Modifier
            .fillMaxSize()
            .background(Color(colors.editorBg))
            .windowInsetsPadding(WindowInsets.safeDrawing)
            .padding(22.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp, Alignment.CenterVertically),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            CopeIconGlyph(Icon.ERROR, colors.error, sizeDp = 20)
            Label("Cope cannot start", colors.editorFg, sizeSp = 16, bold = true)
        }
        Label(reason, colors.editorFg, sizeSp = CopeDimens.TEXT_SP, maxLines = 8)
        Label(
            "Nothing was opened and nothing was changed on disk.",
            colors.lineNumber,
            sizeSp = CopeDimens.TEXT_TINY_SP,
            maxLines = 2,
        )
    }
}

// --- platform glue ---------------------------------------------------------

/**
 * Whether the soft keyboard is up. Two independent signals, OR'd:
 *
 *  * the IME window inset, which is the correct answer on API 30+ and on any
 *    version where the inset plumbing reports it;
 *  * the visible-frame measurement, which is the only answer on API 27-29 where
 *    the window itself is resized by adjustResize and the inset reads 0.
 *
 * The 18% threshold sits above the tallest system-bar-only delta (~13% on a
 * 16:9 phone) and well below any keyboard (~35-45%).
 */
@Composable
private fun imeVisible(): Boolean {
    val view = LocalView.current
    val density = LocalDensity.current
    val insetBottom = WindowInsets.ime.getBottom(density)
    var measured by remember { mutableStateOf(false) }
    DisposableEffect(view) {
        val frame = Rect()
        val listener = ViewTreeObserver.OnGlobalLayoutListener {
            view.getWindowVisibleDisplayFrame(frame)
            val screen = view.rootView.height
            measured = screen > 0 && (screen - frame.height()) > screen * 0.18f
        }
        val observer = view.viewTreeObserver
        observer.addOnGlobalLayoutListener(listener)
        onDispose { observer.removeOnGlobalLayoutListener(listener) }
    }
    return insetBottom > 0 || measured
}

/**
 * Keeps the system back gesture from eating horizontal flings on a strip that
 * lives at the screen edge. Applied imperatively rather than through
 * Modifier.systemGestureExclusion() so it works on the pinned Compose version and
 * degrades to nothing below API 29.
 */
@Composable
internal fun Modifier.excludeFromSystemGestures(): Modifier {
    val view = LocalView.current
    if (Build.VERSION.SDK_INT < 29) return this
    return this.onGloballyPositioned { coordinates ->
        val bounds = coordinates.boundsInWindow()
        val rect = Rect(
            bounds.left.toInt(),
            bounds.top.toInt(),
            bounds.right.toInt(),
            bounds.bottom.toInt(),
        )
        val current = view.systemGestureExclusionRects
        if (current.size != 1 || current[0] != rect) {
            view.systemGestureExclusionRects = listOf(rect)
        }
    }
}

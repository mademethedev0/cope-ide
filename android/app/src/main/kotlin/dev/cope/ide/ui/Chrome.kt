// The persistent chrome: notice strip, info bar, status strip, tab strip, key row.
//
// Vertical budget is the hard constraint. On a 360x720dp phone with the keyboard
// up, everything here plus the IME leaves the editor about eleven lines, so:
//   * the status strip hides while the IME is up (it is information, not action);
//   * the sheet handle is part of the tab strip's top edge, not a separate 16dp;
//   * nothing here is taller than it has to be, and nothing is decorative.
//
// Horizontal budget matters in the info bar for the same reason. Six 36dp targets
// leave ~140dp for the path, so the breadcrumb ellipsises from the left and the
// file name never loses room. Tapping it opens the command palette — quick open is
// the one action worth putting on the largest target in the bar.
@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package dev.cope.ide.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import dev.cope.ide.AppState
import dev.cope.ide.Notice
import dev.cope.ide.Overlay
import dev.cope.ide.Tab
import dev.cope.ide.ViewMode
import dev.cope.ide.core.Tier
import dev.cope.ide.storage.Storage
import dev.cope.ide.theme.CopeDimens
import dev.cope.ide.theme.Derive
import dev.cope.ide.theme.LocalCopeColors

// --- notice strip ----------------------------------------------------------

/**
 * One notice at a time. Every message states the real reason and, where there is
 * one, offers the real action — the app never says "something went wrong".
 */
@Composable
public fun NoticeStrip(notice: Notice, onDismiss: () -> Unit) {
    val colors = LocalCopeColors.current
    val accent = when (notice.severity) {
        Notice.Severity.ERROR -> colors.error
        Notice.Severity.WARN -> colors.warning
        Notice.Severity.INFO -> colors.accent
    }
    val icon = when (notice.severity) {
        Notice.Severity.ERROR -> Icon.ERROR
        Notice.Severity.WARN -> Icon.WARNING
        Notice.Severity.INFO -> Icon.INFO
    }
    Row(
        Modifier
            .fillMaxWidth()
            .background(Color(Derive.mix(colors.surface, accent, 0.18f)))
            .padding(start = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        CopeIconGlyph(icon, accent, sizeDp = 14)
        Label(
            text = notice.text,
            color = colors.surfaceFg,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            maxLines = 3,
            modifier = Modifier.weight(1f).padding(vertical = 7.dp),
        )
        val action = notice.action
        val actionLabel = notice.actionLabel
        if (actionLabel != null && action != null) {
            PillButton(actionLabel, action)
            HSpace(6)
        }
        IconButton(Icon.CLOSE, "Dismiss", onDismiss, tint = colors.dim, sizeDp = 12, touchDp = 40)
    }
}

// --- info bar --------------------------------------------------------------

@Composable
public fun InfoBar(state: AppState) {
    val colors = LocalCopeColors.current
    val tab = state.activeTab
    val document = tab?.document
    // Reading `revision` here is what makes the bar recompose after an edit: the
    // document's own fields are plain vars on purpose (they are read from the draw
    // path too, and making them Compose state would allocate per keystroke).
    @Suppress("UNUSED_EXPRESSION") state.revision

    Row(
        Modifier
            .fillMaxWidth()
            .height(CopeDimens.INFO_BAR_HEIGHT.dp)
            .background(Color(colors.surface)),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (document?.dirty == true) {
            IconButton(
                icon = Icon.SAVE,
                description = "Unsaved changes — tap to save",
                onClick = { state.save() },
                tint = colors.accent,
                sizeDp = 14,
                touchDp = 34,
            )
        } else {
            HSpace(10)
        }

        // The breadcrumb is the quick-open target. It is the widest thing in the
        // bar, and "which file am I in / take me to another one" is one thought.
        Row(
            Modifier
                .weight(1f)
                .fillMaxHeight()
                .clickable { state.overlay = Overlay.Palette },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (tab == null) {
                Label("No file open", colors.dim, sizeSp = CopeDimens.TEXT_SMALL_SP)
            } else {
                if (tab.directory.isNotEmpty()) {
                    Label(
                        text = shortenPath(tab.directory),
                        color = colors.dim,
                        sizeSp = CopeDimens.TEXT_SMALL_SP,
                    )
                }
                Label(
                    text = tab.title,
                    color = colors.surfaceFg,
                    sizeSp = CopeDimens.TEXT_SP,
                    modifier = Modifier.weight(1f, fill = false),
                )
            }
            HSpace(2)
            CopeIconGlyph(Icon.SEARCH, colors.dim, sizeDp = 11)
        }

        if (tab != null) {
            IconButton(
                icon = Icon.UNDO,
                description = "Undo",
                onClick = {
                    state.editor?.undo()
                    state.onEditorChanged()
                },
                enabled = document?.canUndo == true,
                sizeDp = 15,
                touchDp = 36,
            )
            IconButton(
                icon = Icon.REDO,
                description = "Redo",
                onClick = {
                    state.editor?.redo()
                    state.onEditorChanged()
                },
                enabled = document?.canRedo == true,
                sizeDp = 15,
                touchDp = 36,
            )
            if (tab.isMarkdown) {
                IconButton(
                    // The icon shows what the next tap DOES, not the current state:
                    // a button labelled with the state you are already in is the
                    // most common toggle-icon mistake.
                    icon = when (tab.mode) {
                        ViewMode.EDIT -> Icon.PREVIEW
                        ViewMode.PREVIEW -> Icon.SPLIT
                        ViewMode.SPLIT -> Icon.EDIT
                    },
                    description = "Switch to ${nextModeName(tab.mode)} view",
                    onClick = {
                        tab.mode = when (tab.mode) {
                            ViewMode.EDIT -> ViewMode.PREVIEW
                            ViewMode.PREVIEW -> ViewMode.SPLIT
                            ViewMode.SPLIT -> ViewMode.EDIT
                        }
                        state.bump()
                    },
                    sizeDp = 15,
                    touchDp = 36,
                )
            }
            IconButton(
                icon = Icon.MORE_VERTICAL,
                description = "This file: save, rename, find, inspect, close",
                onClick = { state.overlay = Overlay.FileMenu(state.active) },
                sizeDp = 15,
                touchDp = 36,
            )
        }
        IconButton(
            icon = Icon.GEAR,
            description = "Settings for the whole app",
            onClick = { state.overlay = Overlay.AppSettings },
            sizeDp = 15,
            touchDp = 36,
        )
    }
    HDivider(colors.border)
}

private fun nextModeName(mode: ViewMode): String = when (mode) {
    ViewMode.EDIT -> "preview"
    ViewMode.PREVIEW -> "split"
    ViewMode.SPLIT -> "edit"
}

/** "a/b/c/d/" -> "…/c/d/" so the file name never loses room to the path. */
private fun shortenPath(path: String): String {
    val parts = path.trim('/').split('/').filter { it.isNotEmpty() }
    if (parts.size <= 2) return if (path.endsWith('/')) path else "$path/"
    return "…/" + parts.takeLast(2).joinToString("/") + "/"
}

// --- status strip ----------------------------------------------------------

/**
 * A genuine information surface, every segment tappable. The tier segment is here
 * because it is the number this engine can honestly report and no other Android
 * editor shows: whether the real grammar is running or the repair lexer is.
 */
@Composable
public fun StatusStrip(state: AppState) {
    val colors = LocalCopeColors.current
    val tab = state.activeTab ?: return
    val document = tab.document
    @Suppress("UNUSED_EXPRESSION") state.revision

    Row(
        Modifier
            .fillMaxWidth()
            .height(CopeDimens.STATUS_HEIGHT.dp)
            .background(Color(colors.statusBg))
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        StatusItem(
            text = if (state.selectionBytes > 0) {
                "Ln ${state.caretLine + 1}, Col ${state.caretColumn} (${state.selectionBytes} B)"
            } else {
                "Ln ${state.caretLine + 1}, Col ${state.caretColumn}"
            },
            color = colors.statusFg,
        ) { state.overlay = Overlay.GoToLine }

        StatusDot(colors.dim)
        StatusItem(state.index.languageOf(tab.title), colors.statusFg) {
            state.notice = Notice(
                if (document.hasGrammar) {
                    "Highlighted by a TextMate grammar at the ${document.tier.label} tier."
                } else {
                    "No grammar claims .${tab.title.substringAfterLast('.', "")} — " +
                        "the heuristic lexer is colouring this file."
                },
                actionLabel = "Inspect",
                action = { state.openInspector() },
            )
        }

        StatusDot(colors.dim)
        StatusItem(
            text = "tier ${document.tier.label}",
            color = when (document.tier) {
                Tier.GRAMMAR -> colors.statusFg
                Tier.FALLBACK -> colors.warning
                Tier.PLAIN -> colors.dim
            },
        ) { state.openInspector() }

        StatusDot(colors.dim)
        StatusItem("Spaces: ${state.prefs.tabWidth}", colors.statusFg) {
            state.overlay = Overlay.AppSettings
        }

        StatusDot(colors.dim)
        // UTF-8 is a statement of fact, not a picker: the engine stores bytes and
        // does not transcode. Saying so out loud beats a fake encoding menu.
        StatusItem("UTF-8", colors.dim) {
            state.notice = Notice(
                "Cope reads and writes bytes as they are. It does not transcode, so a file in " +
                    "another encoding shows its bytes rather than guessing.",
            )
        }

        StatusDot(colors.dim)
        StatusItem(
            text = Storage.humanSize(document.byteSize),
            color = if (document.tier == Tier.PLAIN) colors.warning else colors.dim,
        ) {
            state.notice = Notice(
                if (document.tier == Tier.PLAIN) {
                    "${Storage.humanSize(document.byteSize)} — past the highlight safety limit, " +
                        "so this file renders as plain text."
                } else {
                    "${Storage.humanSize(document.byteSize)} on disk, ${document.lineCount} lines."
                },
            )
        }

        if (document.inMemory) {
            StatusDot(colors.dim)
            StatusItem("in memory", colors.warning) {
                state.notice = Notice(
                    "Opened through the system picker, so it was read into memory instead of " +
                        "mapped. Grant all-files access to open large files directly.",
                    actionLabel = "Grant",
                    action = { state.requestAllFilesAccess() },
                )
            }
        }
    }
    HDivider(colors.border)
}

@Composable
private fun StatusItem(text: String, color: Int, onClick: () -> Unit) {
    Box(
        Modifier.fillMaxHeight().clickable { onClick() }.padding(horizontal = 2.dp),
        contentAlignment = Alignment.Center,
    ) {
        Label(text, color, sizeSp = CopeDimens.TEXT_TINY_SP)
    }
}

@Composable
private fun StatusDot(color: Int) {
    Box(Modifier.size(3.dp).background(Color(color)))
}

// --- tab strip -------------------------------------------------------------

/**
 * Tabs live at the BOTTOM, in the thumb zone. How many fit is derived from the
 * screen width rather than fixed at three: a 320dp phone gets two, a tablet gets
 * five, and a 96dp tab is the narrowest that still shows a useful name. The rest
 * live behind ⋯ with a count.
 *
 * The strip is also the sheet's drag handle: one gesture surface instead of a
 * separate 16dp grip.
 */
@Composable
public fun TabStrip(
    state: AppState,
    onHandleDrag: (Float) -> Unit,
    onHandleRelease: () -> Unit,
) {
    val colors = LocalCopeColors.current
    val widthDp = LocalConfiguration.current.screenWidthDp
    @Suppress("UNUSED_EXPRESSION") state.revision

    Column(Modifier.excludeFromSystemGestures()) {
        // The grab strip: 10dp above the tab row, drawn as a grip. One gesture
        // surface for the sheet instead of a separate 16dp handle row.
        Box(
            Modifier
                .fillMaxWidth()
                .height(10.dp)
                .background(Color(colors.tabsBg))
                .verticalDragHandle(onHandleDrag, onHandleRelease),
            contentAlignment = Alignment.Center,
        ) {
            Box(
                Modifier
                    .width(34.dp)
                    .height(3.dp)
                    .background(Color(colors.border)),
            )
        }
        Row(
            Modifier
                .fillMaxWidth()
                .height(CopeDimens.TAB_HEIGHT.dp)
                .background(Color(colors.tabsBg)),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (state.tabs.isEmpty()) {
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    Label("No open files", colors.dim, sizeSp = CopeDimens.TEXT_SMALL_SP)
                }
                IconButton(
                    icon = Icon.FOLDER,
                    description = "Open a file",
                    onClick = { state.requestOpenDocument() },
                    tint = colors.tabInactiveFg,
                    sizeDp = 15,
                    touchDp = 42,
                )
                IconButton(
                    icon = Icon.PLUS,
                    description = "New file",
                    onClick = { state.newBuffer() },
                    tint = colors.tabInactiveFg,
                    sizeDp = 14,
                    touchDp = 42,
                )
                return@Row
            }

            val visible = ((widthDp - 46) / 96).coerceIn(1, 5)
            val fits = state.tabs.size <= visible
            // The active tab is always in the window, and the window is as far left
            // as it can be while containing it: switching tabs must not make the
            // strip jump around more than it has to.
            val from = if (fits) {
                0
            } else {
                (state.active - visible + 1).coerceIn(0, state.tabs.size - visible)
            }
            val to = if (fits) state.tabs.size else from + visible
            for (i in from until to) {
                TabChip(state, state.tabs[i], i, Modifier.weight(1f))
            }
            if (!fits) {
                val hidden = state.tabs.size - (to - from)
                Box(
                    Modifier
                        .width(46.dp)
                        .fillMaxHeight()
                        .clickable { state.overlay = Overlay.OpenFiles }
                        .semanticsLabel("$hidden more open files"),
                    contentAlignment = Alignment.Center,
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CopeIconGlyph(Icon.MORE_HORIZONTAL, colors.tabInactiveFg, sizeDp = 14)
                        Label("$hidden", colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
                    }
                }
            }
        }
    }
}

@Composable
private fun TabChip(state: AppState, tab: Tab, index: Int, modifier: Modifier) {
    val colors = LocalCopeColors.current
    val selected = index == state.active
    Row(
        modifier
            .fillMaxHeight()
            .background(Color(if (selected) colors.tabActiveBg else colors.tabInactiveBg))
            .combinedClickable(
                onClick = {
                    // Tapping the active tab scrolls back to the caret. It is the
                    // cheapest "where was I" affordance there is.
                    if (selected) state.editor?.ensureCaretVisible() else state.switchTab(index)
                },
                onLongClick = { state.overlay = Overlay.FileMenu(index) },
            )
            .padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        if (selected) {
            Box(
                Modifier
                    .width(2.dp)
                    .fillMaxHeight()
                    .background(Color(colors.accent)),
            )
        }
        if (tab.document.dirty) DirtyDot(colors.accent, sizeDp = 6)
        Label(
            text = tab.title,
            color = if (selected) colors.tabActiveFg else colors.tabInactiveFg,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            bold = selected,
            modifier = Modifier.weight(1f, fill = false),
        )
        if (selected) {
            IconButton(
                icon = Icon.CLOSE,
                description = "Close ${tab.title}",
                onClick = { state.requestClose(index) },
                tint = colors.dim,
                sizeDp = 11,
                touchDp = 28,
            )
        }
    }
}

// --- key row ---------------------------------------------------------------

/**
 * The smart key row, only present while the IME is up. Two pages: navigation and
 * pairs, then a symbol page whose contents depend on the language — `->` and `::`
 * are useless in Python and essential in C++.
 */
@Composable
public fun KeyRow(state: AppState, page: Int, onPageChange: (Int) -> Unit) {
    val colors = LocalCopeColors.current
    val view = state.editor
    val keys = if (page == 0) {
        navigationKeys()
    } else {
        symbolKeys(state.index.languageOf(state.activeTab?.title ?: ""))
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(CopeDimens.KEY_ROW_HEIGHT.dp)
            .background(Color(colors.surface)),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (key in keys) {
            Box(
                Modifier
                    .weight(if (key.wide) 1.25f else 1f)
                    .fillMaxHeight()
                    .background(Color(colors.keyBg))
                    .clickable {
                        when (key.action) {
                            KeyAction.PAGE -> onPageChange(1 - page)
                            KeyAction.UNDO -> view?.undo()
                            KeyAction.REDO -> view?.redo()
                            KeyAction.LEFT -> view?.moveCaret(-1, false)
                            KeyAction.RIGHT -> view?.moveCaret(1, false)
                            KeyAction.UP -> view?.moveLine(-1, false)
                            KeyAction.DOWN -> view?.moveLine(1, false)
                            KeyAction.TAB -> view?.insertText(" ".repeat(state.prefs.tabWidth))
                            KeyAction.INSERT -> view?.insertText(key.insert)
                            KeyAction.PAIR -> {
                                view?.insertText(key.insert)
                                // Leave the caret between the pair, which is the
                                // only reason a pair key beats two taps.
                                view?.moveCaret(-1, false)
                            }
                            KeyAction.ESCAPE -> {
                                // One key that undoes whatever is in the way, in the
                                // same precedence order as the back gesture.
                                if (!state.dismissTopmost()) view?.hideKeyboard()
                            }
                        }
                        state.onEditorChanged()
                    }
                    .padding(1.dp)
                    .semanticsLabel(key.description),
                contentAlignment = Alignment.Center,
            ) {
                Label(key.label, colors.keyFg, sizeSp = CopeDimens.TEXT_SMALL_SP)
            }
        }
    }
}

private enum class KeyAction { PAGE, UNDO, REDO, LEFT, RIGHT, UP, DOWN, TAB, INSERT, PAIR, ESCAPE }

private class KeyDef(
    val label: String,
    val action: KeyAction,
    val insert: String = "",
    val wide: Boolean = false,
    description: String = "",
) {
    /** Screen readers get words; the key face gets a glyph. */
    val description: String = description.ifEmpty { label }
}

private fun navigationKeys(): List<KeyDef> = listOf(
    KeyDef("esc", KeyAction.ESCAPE, wide = true, description = "Escape"),
    KeyDef("tab", KeyAction.TAB, wide = true, description = "Indent"),
    KeyDef("↶", KeyAction.UNDO, description = "Undo"),
    KeyDef("↷", KeyAction.REDO, description = "Redo"),
    KeyDef("←", KeyAction.LEFT, description = "Left"),
    KeyDef("↑", KeyAction.UP, description = "Up"),
    KeyDef("↓", KeyAction.DOWN, description = "Down"),
    KeyDef("→", KeyAction.RIGHT, description = "Right"),
    KeyDef("()", KeyAction.PAIR, "()", description = "Parentheses"),
    KeyDef("{}", KeyAction.PAIR, "{}", description = "Braces"),
    KeyDef("\"", KeyAction.PAIR, "\"\"", description = "Quotes"),
    KeyDef("#+=", KeyAction.PAGE, wide = true, description = "Symbols page"),
)

/** The symbol page is language-shaped: this is the whole point of a smart row. */
private fun symbolKeys(language: String): List<KeyDef> {
    val symbols = when (language) {
        "C", "C++", "C#", "Rust", "Go", "Java", "Kotlin", "Swift" ->
            listOf("->", "::", "<", ">", "=", "+", "-", "*", "&", "|", "!")
        "Python", "Ruby" ->
            listOf(":", "=", "_", "(", ")", "[", "]", "%", "*", "#", "!")
        "JavaScript", "TypeScript" ->
            listOf("=>", "===", "`", "?", ".", "$", "{", "}", "[", "]", "!")
        "HTML", "XML", "Markdown" ->
            listOf("<", ">", "/", "=", "\"", "#", "*", "[", "]", "(", ")")
        "JSON", "YAML" ->
            listOf(":", "\"", ",", "{", "}", "[", "]", "-", "#", ".", "!")
        "Shell" ->
            listOf("$", "|", ">", "<", "&", "-", "\"", "'", "*", ";", "!")
        else ->
            listOf("=", ":", ";", "-", "_", "+", "*", "/", "<", ">", "!")
    }
    return listOf(KeyDef("abc", KeyAction.PAGE, wide = true, description = "Letters page")) +
        symbols.map { KeyDef(it, KeyAction.INSERT, it) }
}

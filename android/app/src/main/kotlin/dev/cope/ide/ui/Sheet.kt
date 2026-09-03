// The bottom sheet: Files, Inspector, Terminal.
//
// Only these three are tabs. Problems, Search and Replace are NOT here — they
// belong to a file, and the sheet collapses under the keyboard, so a search panel
// in the sheet could never be on screen at the same time as the keyboard typing
// into it. Find lives in a top bar instead (see FindBar).
//
// The Inspector replaces the mockup's Problems panel. There is no diagnostics
// producer in the engine (that needs LSP, a much later phase), and shipping an
// empty Problems list dressed as a feature is exactly the kind of thing this
// project refuses to do. What the engine *can* report honestly — which tier is
// running, how much of the file the grammar scoped, how many patterns the regex
// engine refused, and the scope stack under the caret — is far more useful on a
// phone anyway.
@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package dev.cope.ide.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import dev.cope.ide.AppState
import dev.cope.ide.Overlay
import dev.cope.ide.SheetSnap
import dev.cope.ide.SheetTab
import dev.cope.ide.core.Inspection
import dev.cope.ide.storage.Storage
import dev.cope.ide.storage.StorageMode
import dev.cope.ide.theme.CopeDimens
import dev.cope.ide.theme.LocalCopeColors

@Composable
public fun SheetContent(state: AppState) {
    val colors = LocalCopeColors.current
    Column(Modifier.fillMaxSize().background(Color(colors.surface))) {
        Row(Modifier.fillMaxWidth().height(32.dp).background(Color(colors.surfaceHeaderBg))) {
            SheetTabChip(state, SheetTab.FILES, "Files")
            SheetTabChip(state, SheetTab.INSPECTOR, "Inspector")
            SheetTabChip(state, SheetTab.TERMINAL, "Terminal")
            Box(Modifier.weight(1f).fillMaxHeight())
            IconButton(
                icon = Icon.CLOSE,
                description = "Close the panel",
                onClick = { state.sheetSnap = SheetSnap.CLOSED },
                tint = colors.dim,
                sizeDp = 12,
                touchDp = 32,
            )
        }
        HDivider(colors.border)
        when (state.sheetTab) {
            SheetTab.FILES -> FilesPanel(state)
            SheetTab.INSPECTOR -> InspectorPanel(state)
            SheetTab.TERMINAL -> TerminalPanel()
        }
    }
}

@Composable
private fun SheetTabChip(state: AppState, tab: SheetTab, label: String) {
    val colors = LocalCopeColors.current
    val selected = state.sheetTab == tab
    Box(
        Modifier
            .width(86.dp)
            .fillMaxHeight()
            .background(Color(if (selected) colors.surface else colors.surfaceHeaderBg))
            .clickable { state.sheetTab = tab },
        contentAlignment = Alignment.Center,
    ) {
        Label(
            label,
            if (selected) colors.surfaceFg else colors.dim,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            bold = selected,
        )
        if (selected) {
            Box(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(2.dp)
                    .background(Color(colors.accent)),
            )
        }
    }
}

// --- files -----------------------------------------------------------------

private class TreeNode(
    val path: String,
    val name: String,
    val isDirectory: Boolean,
    val depth: Int,
    val size: Long,
)

@Composable
private fun FilesPanel(state: AppState) {
    val colors = LocalCopeColors.current
    if (state.storageMode != StorageMode.ALL_FILES) {
        // Honest empty state: the real reason, and both real actions.
        Column(
            Modifier.fillMaxWidth().padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Label(
                "Cope cannot browse folders without all-files access. Android only lets an app " +
                    "list directories with that permission; without it, files can still be opened " +
                    "one at a time through the system picker.",
                colors.surfaceFg,
                sizeSp = CopeDimens.TEXT_SMALL_SP,
                maxLines = 6,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton("Grant access", { state.requestAllFilesAccess() }, emphasised = true)
                PillButton("Open one file", { state.requestOpenDocument() })
                PillButton("Recheck", { state.refreshStorageMode() })
            }
            if (state.prefs.recentFiles.isNotEmpty()) {
                Label("Recent", colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
                for (path in state.prefs.recentFiles.take(6)) {
                    Label(
                        path,
                        colors.accent,
                        sizeSp = CopeDimens.TEXT_SMALL_SP,
                        modifier = Modifier.clickable { state.openPath(path) },
                    )
                }
            }
        }
        return
    }

    val nodes = remember(state.treePath, state.treeExpanded.size, state.revision) {
        buildTree(state)
    }
    Column(Modifier.fillMaxSize()) {
        Breadcrumb(state)
        HDivider(colors.border)
        if (nodes.isEmpty()) {
            EmptyState(
                text = "This folder is empty.",
                actionLabel = "New file here",
                onAction = { state.overlay = Overlay.NewEntry(state.treePath, false) },
            )
            return@Column
        }
        LazyColumn(Modifier.fillMaxWidth().weight(1f)) {
            items(nodes, key = { it.path }) { node ->
                TreeRow(state, node)
            }
        }
        HDivider(colors.border)
        Row(
            Modifier.fillMaxWidth().height(38.dp).background(Color(colors.surfaceHeaderBg)),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            HSpace(8)
            Label(
                text = "${nodes.count { !it.isDirectory }} files, " +
                    "${nodes.count { it.isDirectory }} folders",
                color = colors.dim,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                modifier = Modifier.weight(1f),
            )
            IconButton(
                icon = Icon.PLUS,
                description = "New file in this folder",
                onClick = { state.overlay = Overlay.NewEntry(state.treePath, false) },
                tint = colors.surfaceFg,
                sizeDp = 14,
                touchDp = 38,
            )
            IconButton(
                icon = Icon.FOLDER,
                description = "New folder here",
                onClick = { state.overlay = Overlay.NewEntry(state.treePath, true) },
                tint = colors.surfaceFg,
                sizeDp = 14,
                touchDp = 38,
            )
        }
    }
}

@Composable
private fun Breadcrumb(state: AppState) {
    val colors = LocalCopeColors.current
    val segments = remember(state.treePath) {
        val parts = state.treePath.trim('/').split('/').filter { it.isNotEmpty() }
        var accumulated = ""
        parts.map { part ->
            accumulated += "/$part"
            part to accumulated
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(34.dp)
            .background(Color(colors.surfaceHeaderBg)),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Up is a real button. A breadcrumb alone means going up one level requires
        // hitting an 8dp-wide parent segment, which on a phone is a coin toss.
        IconButton(
            icon = Icon.ARROW_UP,
            description = "Up one folder",
            onClick = {
                val parent = state.treePath.trimEnd('/').substringBeforeLast('/', "")
                state.setTreePath(if (parent.isEmpty()) "/" else parent)
            },
            enabled = state.treePath.trim('/').isNotEmpty(),
            tint = colors.surfaceFg,
            sizeDp = 13,
            touchDp = 34,
        )
        Row(
            Modifier
                .weight(1f)
                .fillMaxHeight()
                .horizontalScroll(rememberScrollState()),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Label(
                "/",
                colors.dim,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                modifier = Modifier.clickable { state.setTreePath("/") }.padding(horizontal = 3.dp),
            )
            for ((name, path) in segments) {
                Label(
                    name,
                    colors.surfaceFg,
                    sizeSp = CopeDimens.TEXT_TINY_SP,
                    modifier = Modifier
                        .clickable { state.setTreePath(path) }
                        .padding(horizontal = 3.dp),
                )
                Label("/", colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
            }
        }
    }
}

@Composable
private fun TreeRow(state: AppState, node: TreeNode) {
    val colors = LocalCopeColors.current
    val openTab = state.tabs.firstOrNull { it.document.path == node.path }
    val isActive = state.activeTab?.document?.path == node.path
    DenseRow(
        selected = isActive,
        onClick = {
            if (node.isDirectory) state.toggleExpanded(node.path) else state.openPath(node.path)
        },
        onLongClick = { state.overlay = Overlay.TreeMenu(node.path, node.isDirectory) },
    ) {
        Box(Modifier.width((node.depth * 13).dp))
        if (node.isDirectory) {
            CopeIconGlyph(
                if (state.treeExpanded.contains(node.path)) Icon.CHEVRON_DOWN else Icon.CHEVRON_RIGHT,
                colors.dim,
                sizeDp = 12,
            )
        } else {
            Box(Modifier.size(12.dp))
        }
        CopeIconGlyph(
            if (node.isDirectory) Icon.FOLDER else Icon.FILE,
            if (node.isDirectory) colors.accent else colors.dim,
            sizeDp = 14,
        )
        if (openTab?.document?.dirty == true) {
            DirtyDot(colors.accent, sizeDp = 6)
        }
        Label(
            node.name,
            when {
                isActive -> colors.listActiveFg
                openTab != null -> colors.accent
                else -> colors.surfaceFg
            },
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            bold = isActive,
            modifier = Modifier.weight(1f),
        )
        if (!node.isDirectory) {
            Label(Storage.humanSize(node.size), colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP)
        }
        IconButton(
            icon = Icon.MORE_VERTICAL,
            description = "Menu for ${node.name}",
            onClick = { state.overlay = Overlay.TreeMenu(node.path, node.isDirectory) },
            tint = colors.dim,
            sizeDp = 13,
            touchDp = 40,
        )
    }
}

private fun buildTree(state: AppState): List<TreeNode> {
    val engine = state.engine ?: return emptyList()
    val out = ArrayList<TreeNode>()

    fun walk(path: String, depth: Int) {
        // Depth is bounded so a symlink loop or a pathological tree cannot hang the
        // UI thread; 12 levels is deeper than any real project on a phone.
        if (depth > 12) return
        for (entry in engine.listDir(path)) {
            val child = if (path.endsWith('/')) "$path${entry.name}" else "$path/${entry.name}"
            out += TreeNode(child, entry.name, entry.isDirectory, depth, entry.size)
            if (entry.isDirectory && state.treeExpanded.contains(child)) {
                walk(child, depth + 1)
            }
        }
    }
    walk(state.treePath, 0)
    return out
}

// --- inspector -------------------------------------------------------------

@Composable
private fun InspectorPanel(state: AppState) {
    val colors = LocalCopeColors.current
    val tab = state.activeTab
    if (tab == null) {
        EmptyState(
            "No file is open, so there is nothing to inspect.",
            actionLabel = "Open a file",
            onAction = { state.requestOpenDocument() },
        )
        return
    }
    // Measured on demand, not per frame: analysing a file walks up to 4000 lines.
    val inspection = remember(tab, state.revision) { state.inspect() }
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        // The headline, in one sentence, before the table of numbers. "cov=100% but
        // SUSPECT" is the confusing case the CLI documented; say what it means here.
        Label(
            text = verdictOf(inspection, tab.document.hasGrammar),
            color = when {
                !tab.document.hasGrammar -> colors.warning
                inspection.repairRatio > 0.3f || inspection.refusedPatterns > 0 -> colors.warning
                else -> colors.ok
            },
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            maxLines = 4,
        )
        HDivider(colors.border)

        Fact("tier", inspection.tier.ifEmpty { tab.document.tier.label }, colors.surfaceFg)
        Fact(
            "grammar",
            inspection.grammarScope.ifEmpty { "none \u2014 heuristic lexer only" },
            if (tab.document.hasGrammar) colors.surfaceFg else colors.warning,
        )
        Fact("scope coverage", "${(inspection.coverage * 100).toInt()}%", colors.surfaceFg)
        Fact(
            "repaired by fallback",
            "${(inspection.repairRatio * 100).toInt()}%",
            if (inspection.repairRatio > 0.3f) colors.warning else colors.surfaceFg,
        )
        Fact(
            "patterns the regex engine refused",
            inspection.refusedPatterns.toString(),
            if (inspection.refusedPatterns > 0) colors.warning else colors.surfaceFg,
        )
        Fact(
            "rules disabled (unusable end pattern)",
            inspection.unusableRules.toString(),
            if (inspection.unusableRules > 0) colors.warning else colors.surfaceFg,
        )
        Fact("lines", tab.document.lineCount.toString(), colors.surfaceFg)
        Fact("bytes", Storage.humanSize(tab.document.byteSize), colors.surfaceFg)
        Fact("mapped", if (tab.document.inMemory) "no \u2014 read into memory" else "yes", colors.dim)

        if (inspection.refusedPatterns > 0) {
            Label(
                "Refused patterns are grammar rules this regex backend cannot compile. Their text " +
                    "is coloured by the fallback lexer instead, which is why coverage can be 100% " +
                    "while the grammar is only partly working.",
                colors.dim,
                sizeSp = CopeDimens.TEXT_TINY_SP,
                maxLines = 5,
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PillButton("Show scopes at caret", { state.showScopesAtCaret() }, emphasised = true)
        }
        Label(
            "Force a tier (for debugging a file that looks wrong)",
            colors.dim,
            sizeSp = CopeDimens.TEXT_TINY_SP,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for ((code, label) in TIERS) {
                PillButton(label, {
                    tab.document.forceTier(code)
                    state.editor?.invalidateContent()
                    state.bump()
                })
            }
        }
        if (inspection.report.isNotEmpty()) {
            Label(inspection.report, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP, maxLines = 6)
        }
    }
}

private val TIERS: List<Pair<Int, String>> =
    listOf(0 to "auto", 1 to "grammar", 2 to "fallback", 3 to "plain")

/**
 * One sentence naming what is actually wrong, or that nothing is. The distinction
 * that matters and that a raw number hides: high coverage with high repair means
 * the fallback lexer is doing the grammar's job.
 */
private fun verdictOf(
    inspection: Inspection,
    hasGrammar: Boolean,
): String = when {
    !hasGrammar ->
        "No grammar claims this file type, so every colour here comes from the heuristic lexer. " +
            "That is why keywords are not distinguished from identifiers."
    inspection.refusedPatterns > 0 ->
        "${inspection.refusedPatterns} grammar rules would not compile in this build's regex " +
            "engine, so their text falls back to the heuristic lexer."
    inspection.repairRatio > 0.3f ->
        "The grammar left ${(inspection.repairRatio * 100).toInt()}% of this file unscoped and the " +
            "fallback lexer filled it in. High coverage here does not mean the grammar is working."
    inspection.coverage < 0.9f ->
        "${(inspection.coverage * 100).toInt()}% of the bytes have a scope. The rest render in the " +
            "default colour."
    else ->
        "The grammar is doing the work: ${(inspection.coverage * 100).toInt()}% scoped, nothing " +
            "refused, no rules disabled."
}

@Composable
private fun Fact(label: String, value: String, valueColor: Int) {
    val colors = LocalCopeColors.current
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Label(label, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP, modifier = Modifier.weight(1f))
        Label(value, valueColor, sizeSp = CopeDimens.TEXT_SMALL_SP)
    }
}

// --- terminal --------------------------------------------------------------

@Composable
private fun TerminalPanel() {
    val colors = LocalCopeColors.current
    Column(
        Modifier.fillMaxSize().padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Label("Not built yet.", colors.surfaceFg, sizeSp = CopeDimens.TEXT_SP, bold = true)
        Label(
            "The terminal is a real embedded pty with libvterm and bundled binaries, not a proot " +
                "image and not a shell-out. It is a later phase, and this tab exists so the " +
                "layout it will live in is already settled.",
            colors.dim,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
            maxLines = 6,
        )
    }
}

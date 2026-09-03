// The markdown preview.
//
// It renders the block tree from MarkdownStream with Compose text, not a WebView:
// a WebView is ~20 MB of resident memory, needs its own theme plumbing and cannot
// be styled from the same CopeColors the editor uses. Everything here is drawn
// with the app's own font and the current theme's colours, so preview and edit
// look like the same program.
//
// Scope, stated honestly:
//   * Fenced code renders monospace on a tinted surface. It is NOT syntax
//     highlighted yet — the highlighter works on a Document, and the preview has
//     a string. That is a later change, and the code block says so via nothing at
//     all: it simply looks like a code block, which is not a lie.
//   * Math and Mermaid render as their source with a one-line notice, because the
//     renderers for both are the WebView island in the markdown design (M3), not
//     this phase.
//   * Images render as their alt text plus the target, since fetching remote
//     images from a preview is a network call this app does not make.
package dev.cope.ide.markdown

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.ClickableText
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.cope.ide.theme.CopeColors
import dev.cope.ide.theme.CopeFonts
import dev.cope.ide.theme.Derive
import dev.cope.ide.theme.LocalCopeColors
import dev.cope.ide.theme.LocalCopeFonts

/** Base body size. Headings scale from here; code uses the same size. */
private const val BODY_SP = 14

private val HEADING_SP = intArrayOf(0, 22, 19, 17, 15, 14, 13)

/**
 * Renders a parsed document. `onLink` receives the raw href; the host decides
 * whether that means "open a file" or "hand it to the system", because a relative
 * link inside a repo is a file and an http one is not.
 */
@Composable
public fun MarkdownView(
    blocks: List<MdBlock>,
    modifier: Modifier = Modifier,
    onLink: (String) -> Unit = {},
) {
    val colors = LocalCopeColors.current
    val fonts = LocalCopeFonts.current
    Column(
        modifier
            .fillMaxWidth()
            .background(Color(colors.editorBg))
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(9.dp),
    ) {
        if (blocks.isEmpty()) {
            BasicText(
                text = "This file has no markdown content to preview.",
                style = TextStyle(
                    color = Color(colors.lineNumber),
                    fontSize = BODY_SP.sp,
                    fontFamily = fonts.family,
                ),
            )
            return@Column
        }
        for (block in blocks) {
            Block(block, colors, fonts, onLink)
        }
    }
}

@Composable
private fun Block(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
) {
    when (block.kind) {
        "h" -> Heading(block, colors, fonts, onLink)
        "p" -> Paragraph(block, colors, fonts, onLink, BODY_SP)
        "hr" -> Box(
            Modifier
                .fillMaxWidth()
                .padding(vertical = 4.dp)
                .height(1.dp)
                .background(Color(colors.border)),
        )
        "code" -> CodeBlock(block.text(), block.arg, colors, fonts)
        "math" -> SourceBlock(
            source = block.text(),
            notice = "Math is shown as its source. A renderer for it is a later phase.",
            colors = colors,
            fonts = fonts,
        )
        "mermaid" -> SourceBlock(
            source = block.text(),
            notice = "Mermaid diagrams are shown as their source. Rendering them is a later phase.",
            colors = colors,
            fonts = fonts,
        )
        "quote" -> Quote(block, colors, fonts, onLink)
        "ul" -> ListBlock(block, colors, fonts, onLink, ordered = false)
        "ol" -> ListBlock(block, colors, fonts, onLink, ordered = true)
        "table" -> TableBlock(block, colors, fonts, onLink)
        // "li", "cell", "thead" and "trow" are always reached through their parent.
        else -> {
            if (block.runs.isNotEmpty()) Paragraph(block, colors, fonts, onLink, BODY_SP)
            for (child in block.children) Block(child, colors, fonts, onLink)
        }
    }
}

@Composable
private fun Heading(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
) {
    val level = block.level
    Column(Modifier.fillMaxWidth().padding(top = if (level <= 2) 8.dp else 4.dp)) {
        InlineText(
            runs = block.runs,
            colors = colors,
            fonts = fonts,
            sizeSp = HEADING_SP[level.coerceIn(1, 6)],
            bold = true,
            onLink = onLink,
        )
        // Only h1/h2 get a rule, the same convention every markdown renderer uses
        // and the reason a long document is scannable.
        if (level <= 2) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .padding(top = 5.dp)
                    .height(1.dp)
                    .background(Color(colors.border)),
            )
        }
    }
}

@Composable
private fun Paragraph(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
    sizeSp: Int,
) {
    InlineText(block.runs, colors, fonts, sizeSp, bold = false, onLink = onLink)
}

@Composable
private fun Quote(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(
            Modifier
                .width(3.dp)
                .height(quoteHeightGuess(block).dp)
                .background(Color(colors.accent)),
        )
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (block.runs.isNotEmpty()) Paragraph(block, colors, fonts, onLink, BODY_SP)
            for (child in block.children) Block(child, colors, fonts, onLink)
        }
    }
}

/**
 * The quote bar cannot use fillMaxHeight inside a scrolling Column (the row has no
 * bounded height there), so its length is estimated from the text. Wrong by a few
 * dp on a long quote; a Layout that measures the content first would be the exact
 * fix and is not worth the code in v1.
 */
private fun quoteHeightGuess(block: MdBlock): Int {
    var characters = block.text().length
    for (child in block.children) characters += child.text().length
    val lines = 1 + characters / 46
    return (lines * 20).coerceIn(20, 400)
}

@Composable
private fun ListBlock(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
    ordered: Boolean,
) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        var number = 1
        for (item in block.children) {
            if (item.kind != "li") {
                Block(item, colors, fonts, onLink)
                continue
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                BasicText(
                    text = if (ordered) "$number." else "•",
                    modifier = Modifier.width(if (ordered) 26.dp else 12.dp),
                    style = TextStyle(
                        color = Color(colors.accent),
                        fontSize = BODY_SP.sp,
                        fontFamily = fonts.family,
                    ),
                )
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (item.runs.isNotEmpty()) Paragraph(item, colors, fonts, onLink, BODY_SP)
                    for (child in item.children) Block(child, colors, fonts, onLink)
                }
            }
            number++
        }
    }
}

@Composable
private fun CodeBlock(source: String, language: String, colors: CopeColors, fonts: CopeFonts) {
    val tint = Derive.mix(colors.editorBg, colors.editorFg, if (colors.isDark) 0.07f else 0.05f)
    Column(
        Modifier
            .fillMaxWidth()
            .background(Color(tint))
            .border(1.dp, Color(colors.border)),
    ) {
        if (language.isNotEmpty()) {
            BasicText(
                text = language,
                modifier = Modifier.padding(start = 8.dp, top = 4.dp),
                style = TextStyle(
                    color = Color(colors.lineNumber),
                    fontSize = 10.sp,
                    fontFamily = fonts.family,
                ),
            )
        }
        BasicText(
            text = source.trimEnd('\n'),
            modifier = Modifier
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 8.dp, vertical = 6.dp),
            style = TextStyle(
                color = Color(colors.editorFg),
                fontSize = (BODY_SP - 1).sp,
                fontFamily = fonts.family,
                lineHeight = ((BODY_SP - 1) * 1.4f).sp,
            ),
            softWrap = false,
        )
    }
}

@Composable
private fun SourceBlock(source: String, notice: String, colors: CopeColors, fonts: CopeFonts) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(3.dp)) {
        CodeBlock(source, "", colors, fonts)
        BasicText(
            text = notice,
            style = TextStyle(
                color = Color(colors.lineNumber),
                fontSize = 10.sp,
                fontFamily = fonts.family,
            ),
        )
    }
}

@Composable
private fun TableBlock(
    block: MdBlock,
    colors: CopeColors,
    fonts: CopeFonts,
    onLink: (String) -> Unit,
) {
    // A phone is narrower than any real table, so the whole table scrolls sideways
    // as one unit with fixed-width columns. Proportional widths would make the
    // widest cell decide the layout and squeeze everything else to one character.
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .border(1.dp, Color(colors.border)),
    ) {
        Column {
            for (row in block.children) {
                val header = row.kind == "thead"
                val background = if (header) colors.surfaceHeaderBg else colors.editorBg
                Row(Modifier.background(Color(background))) {
                    for (cell in row.children) {
                        Box(
                            Modifier
                                .width(140.dp)
                                .padding(horizontal = 7.dp, vertical = 5.dp),
                            contentAlignment = when (cell.align) {
                                'c' -> Alignment.Center
                                'r' -> Alignment.CenterEnd
                                else -> Alignment.CenterStart
                            },
                        ) {
                            InlineText(
                                runs = cell.runs,
                                colors = colors,
                                fonts = fonts,
                                sizeSp = BODY_SP - 1,
                                bold = header,
                                onLink = onLink,
                                align = when (cell.align) {
                                    'c' -> TextAlign.Center
                                    'r' -> TextAlign.End
                                    else -> TextAlign.Start
                                },
                            )
                        }
                    }
                }
                Box(Modifier.fillMaxWidth().height(1.dp).background(Color(colors.border)))
            }
        }
    }
}

/** One paragraph's worth of runs, as a single laid-out AnnotatedString. */
@Composable
private fun InlineText(
    runs: List<MdRun>,
    colors: CopeColors,
    fonts: CopeFonts,
    sizeSp: Int,
    bold: Boolean,
    onLink: (String) -> Unit,
    align: TextAlign = TextAlign.Start,
) {
    if (runs.isEmpty()) return
    val codeTint = Derive.mix(colors.editorBg, colors.editorFg, if (colors.isDark) 0.12f else 0.08f)
    val annotated = remember(runs, colors, bold) {
        buildInline(runs, colors, codeTint, bold, fonts.family)
    }
    val style = TextStyle(
        color = Color(colors.editorFg),
        fontSize = sizeSp.sp,
        fontFamily = fonts.family,
        lineHeight = (sizeSp * 1.5f).sp,
        textAlign = align,
    )
    val hasLink = runs.any { it.link }
    if (!hasLink) {
        BasicText(annotated, style = style)
        return
    }
    ClickableText(
        text = annotated,
        style = style,
        onClick = { offset ->
            annotated.getStringAnnotations(TAG_HREF, offset, offset).firstOrNull()?.let {
                onLink(it.item)
            }
        },
    )
}

private const val TAG_HREF = "href"

private fun buildInline(
    runs: List<MdRun>,
    colors: CopeColors,
    codeTint: Int,
    forceBold: Boolean,
    family: FontFamily,
): AnnotatedString = buildAnnotatedString {
    for (run in runs) {
        val text = when {
            run.image -> if (run.text.isEmpty()) "[image] ${run.href}" else "[image: ${run.text}]"
            else -> run.text
        }
        if (text.isEmpty()) continue
        val start = length
        if (run.link && run.href.isNotEmpty()) {
            pushStringAnnotation(TAG_HREF, run.href)
        }
        append(text)
        if (run.link && run.href.isNotEmpty()) {
            pop()
        }
        addStyle(
            SpanStyle(
                color = when {
                    run.link -> Color(colors.accent)
                    run.code -> Color(colors.editorFg)
                    run.math -> Color(colors.warning)
                    run.image -> Color(colors.lineNumber)
                    else -> Color(colors.editorFg)
                },
                background = if (run.code || run.math) Color(codeTint) else Color.Unspecified,
                fontWeight = if (run.bold || forceBold) FontWeight.Bold else null,
                fontStyle = if (run.italic) FontStyle.Italic else null,
                textDecoration = when {
                    run.strike -> TextDecoration.LineThrough
                    run.link -> TextDecoration.Underline
                    else -> null
                },
                fontFamily = family,
            ),
            start,
            length,
        )
    }
}

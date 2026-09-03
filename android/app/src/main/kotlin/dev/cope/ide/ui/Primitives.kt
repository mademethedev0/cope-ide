// Chrome primitives: icons, rows, labels, buttons, dividers, fields, menus.
//
// Icons are drawn as geometry, not glyphs and not Material Icons. Glyph icons
// (the mockup's ⋮ ⚙ ▾ ⚠) render differently on every vendor font and some devices
// substitute an emoji; Material Icons is an extra artifact for shapes this app
// uses twelve of. Drawing them keeps them crisp, themable and free.
@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package dev.cope.ide.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.cope.ide.theme.CopeDimens
import dev.cope.ide.theme.Derive
import dev.cope.ide.theme.LocalCopeColors
import dev.cope.ide.theme.LocalCopeFonts

public enum class Icon {
    MORE_VERTICAL,
    MORE_HORIZONTAL,
    GEAR,
    CHEVRON_DOWN,
    CHEVRON_RIGHT,
    CLOSE,
    SEARCH,
    WARNING,
    ERROR,
    INFO,
    FILE,
    FOLDER,
    UNDO,
    REDO,
    SAVE,
    PLUS,
    CHECK,
    ARROW_LEFT,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PREVIEW,
    SPLIT,
    EDIT,
    GRIP,
    TRASH,
    PALETTE,
    LIST,
}

@Composable
public fun CopeIconGlyph(
    icon: Icon,
    tint: Int,
    modifier: Modifier = Modifier,
    sizeDp: Int = 16,
) {
    Canvas(modifier.size(sizeDp.dp)) {
        val color = Color(tint)
        val w = size.width
        val h = size.height
        val cx = w / 2f
        val cy = h / 2f
        val unit = w / 16f
        val stroke = Stroke(width = maxOf(1.4f, unit * 1.4f))

        fun line(x1: Float, y1: Float, x2: Float, y2: Float) {
            drawLine(color, Offset(x1, y1), Offset(x2, y2), strokeWidth = stroke.width)
        }

        when (icon) {
            Icon.MORE_VERTICAL -> {
                val r = unit * 1.5f
                drawCircle(color, r, Offset(cx, cy - unit * 4.5f))
                drawCircle(color, r, Offset(cx, cy))
                drawCircle(color, r, Offset(cx, cy + unit * 4.5f))
            }
            Icon.MORE_HORIZONTAL -> {
                val r = unit * 1.5f
                drawCircle(color, r, Offset(cx - unit * 4.5f, cy))
                drawCircle(color, r, Offset(cx, cy))
                drawCircle(color, r, Offset(cx + unit * 4.5f, cy))
            }
            Icon.GEAR -> {
                // A ring plus six teeth reads as a gear at 16dp; a real involute
                // profile does not.
                drawCircle(color, w * 0.28f, Offset(cx, cy), style = stroke)
                for (i in 0 until 6) {
                    val angle = Math.PI * i / 3.0
                    val sx = cx + (w * 0.30f * Math.cos(angle)).toFloat()
                    val sy = cy + (h * 0.30f * Math.sin(angle)).toFloat()
                    val ex = cx + (w * 0.46f * Math.cos(angle)).toFloat()
                    val ey = cy + (h * 0.46f * Math.sin(angle)).toFloat()
                    line(sx, sy, ex, ey)
                }
            }
            Icon.CHEVRON_DOWN -> {
                line(cx - unit * 3.5f, cy - unit * 1.6f, cx, cy + unit * 2f)
                line(cx, cy + unit * 2f, cx + unit * 3.5f, cy - unit * 1.6f)
            }
            Icon.CHEVRON_RIGHT -> {
                line(cx - unit * 1.6f, cy - unit * 3.5f, cx + unit * 2f, cy)
                line(cx + unit * 2f, cy, cx - unit * 1.6f, cy + unit * 3.5f)
            }
            Icon.CLOSE -> {
                line(cx - unit * 3.4f, cy - unit * 3.4f, cx + unit * 3.4f, cy + unit * 3.4f)
                line(cx + unit * 3.4f, cy - unit * 3.4f, cx - unit * 3.4f, cy + unit * 3.4f)
            }
            Icon.SEARCH -> {
                drawCircle(color, w * 0.26f, Offset(cx - unit, cy - unit), style = stroke)
                line(cx + unit * 1.6f, cy + unit * 1.6f, cx + unit * 5f, cy + unit * 5f)
            }
            Icon.WARNING -> {
                val path = Path().apply {
                    moveTo(cx, unit * 2f)
                    lineTo(w - unit * 1.5f, h - unit * 2f)
                    lineTo(unit * 1.5f, h - unit * 2f)
                    close()
                }
                drawPath(path, color, style = stroke)
                line(cx, cy - unit * 2f, cx, cy + unit * 2f)
            }
            Icon.ERROR -> {
                drawCircle(color, w * 0.40f, Offset(cx, cy), style = stroke)
                line(cx - unit * 2.4f, cy - unit * 2.4f, cx + unit * 2.4f, cy + unit * 2.4f)
                line(cx + unit * 2.4f, cy - unit * 2.4f, cx - unit * 2.4f, cy + unit * 2.4f)
            }
            Icon.INFO -> {
                drawCircle(color, w * 0.40f, Offset(cx, cy), style = stroke)
                line(cx, cy - unit, cx, cy + unit * 3f)
                drawCircle(color, unit * 0.9f, Offset(cx, cy - unit * 3f))
            }
            Icon.FILE -> {
                val path = Path().apply {
                    moveTo(unit * 3.5f, unit * 1.5f)
                    lineTo(w - unit * 5f, unit * 1.5f)
                    lineTo(w - unit * 3f, unit * 4.5f)
                    lineTo(w - unit * 3f, h - unit * 1.5f)
                    lineTo(unit * 3.5f, h - unit * 1.5f)
                    close()
                }
                drawPath(path, color, style = stroke)
            }
            Icon.FOLDER -> {
                val path = Path().apply {
                    moveTo(unit * 2f, unit * 3.5f)
                    lineTo(unit * 6.5f, unit * 3.5f)
                    lineTo(unit * 8f, unit * 5.5f)
                    lineTo(w - unit * 2f, unit * 5.5f)
                    lineTo(w - unit * 2f, h - unit * 3f)
                    lineTo(unit * 2f, h - unit * 3f)
                    close()
                }
                drawPath(path, color, style = stroke)
            }
            Icon.UNDO, Icon.REDO -> {
                val flip = if (icon == Icon.REDO) -1f else 1f
                val x0 = cx + flip * unit * 4f
                line(cx - flip * unit * 4f, cy + unit * 2.5f, x0, cy + unit * 2.5f)
                line(x0, cy + unit * 2.5f, x0 - flip * unit * 2f, cy - unit * 0.5f)
                line(cx - flip * unit * 4f, cy + unit * 2.5f, cx - flip * unit * 1.5f, cy - unit * 2.5f)
            }
            Icon.SAVE -> {
                // A filled dot: the same shape as the dirty indicator, so "unsaved"
                // and "save" are visibly the same idea.
                drawCircle(color, w * 0.26f, Offset(cx, cy))
            }
            Icon.PLUS -> {
                line(cx - unit * 4f, cy, cx + unit * 4f, cy)
                line(cx, cy - unit * 4f, cx, cy + unit * 4f)
            }
            Icon.CHECK -> {
                line(cx - unit * 4f, cy, cx - unit * 1f, cy + unit * 3f)
                line(cx - unit * 1f, cy + unit * 3f, cx + unit * 4.5f, cy - unit * 3.5f)
            }
            Icon.ARROW_LEFT -> {
                line(cx + unit * 3.5f, cy, cx - unit * 3.5f, cy)
                line(cx - unit * 3.5f, cy, cx, cy - unit * 3.5f)
                line(cx - unit * 3.5f, cy, cx, cy + unit * 3.5f)
            }
            Icon.ARROW_RIGHT -> {
                line(cx - unit * 3.5f, cy, cx + unit * 3.5f, cy)
                line(cx + unit * 3.5f, cy, cx, cy - unit * 3.5f)
                line(cx + unit * 3.5f, cy, cx, cy + unit * 3.5f)
            }
            Icon.ARROW_UP -> {
                line(cx, cy + unit * 3.5f, cx, cy - unit * 3.5f)
                line(cx, cy - unit * 3.5f, cx - unit * 3.5f, cy)
                line(cx, cy - unit * 3.5f, cx + unit * 3.5f, cy)
            }
            Icon.ARROW_DOWN -> {
                line(cx, cy - unit * 3.5f, cx, cy + unit * 3.5f)
                line(cx, cy + unit * 3.5f, cx - unit * 3.5f, cy)
                line(cx, cy + unit * 3.5f, cx + unit * 3.5f, cy)
            }
            Icon.PREVIEW -> {
                drawCircle(color, w * 0.14f, Offset(cx, cy))
                val path = Path().apply {
                    moveTo(unit * 1.5f, cy)
                    quadraticBezierTo(cx, unit * 2f, w - unit * 1.5f, cy)
                    quadraticBezierTo(cx, h - unit * 2f, unit * 1.5f, cy)
                }
                drawPath(path, color, style = stroke)
            }
            Icon.SPLIT -> {
                drawRect(
                    color = color,
                    topLeft = Offset(unit * 2f, unit * 2f),
                    size = Size(w - unit * 4f, h - unit * 4f),
                    style = stroke,
                )
                line(cx, unit * 2f, cx, h - unit * 2f)
            }
            Icon.EDIT -> {
                line(unit * 3f, h - unit * 3f, w - unit * 4f, unit * 3.5f)
                line(w - unit * 4f, unit * 3.5f, w - unit * 2f, unit * 5.5f)
                line(w - unit * 2f, unit * 5.5f, unit * 5f, h - unit * 2f)
            }
            Icon.GRIP -> {
                drawRect(
                    color = color,
                    topLeft = Offset(w * 0.2f, cy - unit),
                    size = Size(w * 0.6f, unit * 2f),
                )
            }
            Icon.TRASH -> {
                line(unit * 2.5f, unit * 4f, w - unit * 2.5f, unit * 4f)
                line(cx - unit * 2f, unit * 4f, cx - unit * 2f, unit * 2f)
                line(cx + unit * 2f, unit * 2f, cx - unit * 2f, unit * 2f)
                line(cx + unit * 2f, unit * 2f, cx + unit * 2f, unit * 4f)
                val body = Path().apply {
                    moveTo(unit * 4f, unit * 4f)
                    lineTo(unit * 5f, h - unit * 2f)
                    lineTo(w - unit * 5f, h - unit * 2f)
                    lineTo(w - unit * 4f, unit * 4f)
                }
                drawPath(body, color, style = stroke)
            }
            Icon.PALETTE -> {
                drawCircle(color, w * 0.42f, Offset(cx, cy), style = stroke)
                drawCircle(color, unit * 1.3f, Offset(cx - unit * 3f, cy - unit * 2f))
                drawCircle(color, unit * 1.3f, Offset(cx + unit * 2.5f, cy - unit * 2.5f))
                drawCircle(color, unit * 1.3f, Offset(cx - unit * 1f, cy + unit * 3f))
            }
            Icon.LIST -> {
                for (i in 0 until 3) {
                    val y = cy + (i - 1) * unit * 4f
                    drawCircle(color, unit * 0.9f, Offset(unit * 3f, y))
                    line(unit * 5.5f, y, w - unit * 2.5f, y)
                }
            }
        }
    }
}

/** A 44dp square tap target wrapping an icon. Never smaller: 26dp is a mis-tap factory. */
@Composable
public fun IconButton(
    icon: Icon,
    description: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    tint: Int = LocalCopeColors.current.surfaceFg,
    sizeDp: Int = 16,
    touchDp: Int = CopeDimens.TOUCH_MIN,
    enabled: Boolean = true,
) {
    val colors = LocalCopeColors.current
    Box(
        modifier
            .size(touchDp.dp)
            .clickable(enabled = enabled) { onClick() }
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) {
        CopeIconGlyph(icon, if (enabled) tint else colors.dim, sizeDp = sizeDp)
    }
}

/** Chrome label. Single line, ellipsised, monospace. */
@Composable
public fun Label(
    text: String,
    color: Int,
    modifier: Modifier = Modifier,
    sizeSp: Int = CopeDimens.TEXT_SP,
    bold: Boolean = false,
    maxLines: Int = 1,
) {
    BasicText(
        text = text,
        modifier = modifier,
        style = TextStyle(
            color = Color(color),
            fontSize = sizeSp.sp,
            fontFamily = LocalCopeFonts.current.family,
            fontWeight = if (bold) FontWeight.Bold else null,
        ),
        maxLines = maxLines,
        overflow = TextOverflow.Ellipsis,
    )
}

/** A small bordered button, the only button shape in the app. */
@Composable
public fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    emphasised: Boolean = false,
) {
    val colors = LocalCopeColors.current
    Box(
        modifier
            .height(32.dp)
            .background(if (emphasised) colors.accent else colors.keyBg)
            .clickable { onClick() }
            .padding(horizontal = 12.dp),
        contentAlignment = Alignment.Center,
    ) {
        Label(
            text = text,
            color = if (emphasised) contrastOn(colors.accent) else colors.keyFg,
            sizeSp = CopeDimens.TEXT_SMALL_SP,
        )
    }
}

/** Black or white, whichever is readable on `background`. */
public fun contrastOn(background: Int): Int =
    if (Derive.luma(background) > 0.45f) 0xFF101010.toInt() else 0xFFFFFFFF.toInt()

@Composable
public fun HDivider(color: Int, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().height(1.dp).background(Color(color)))
}

@Composable
public fun VDivider(color: Int, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxHeight().width(1.dp).background(Color(color)))
}

/** A dense 44dp list row — the ZArchiver shape: no card, no shadow, no gradient. */
@Composable
public fun DenseRow(
    modifier: Modifier = Modifier,
    selected: Boolean = false,
    onClick: (() -> Unit)? = null,
    onLongClick: (() -> Unit)? = null,
    content: @Composable RowScope.() -> Unit,
) {
    val colors = LocalCopeColors.current
    Row(
        modifier
            .fillMaxWidth()
            .height(CopeDimens.ROW_HEIGHT.dp)
            .background(Color(if (selected) colors.listActiveBg else colors.surface))
            .then(
                if (onClick != null || onLongClick != null) {
                    Modifier.combinedClickableCompat(onClick, onLongClick)
                } else {
                    Modifier
                },
            )
            .padding(start = 10.dp, end = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        content = content,
    )
}

@Composable
private fun Modifier.combinedClickableCompat(
    onClick: (() -> Unit)?,
    onLongClick: (() -> Unit)?,
): Modifier = this.combinedClickable(
    onLongClick = onLongClick,
    onClick = { onClick?.invoke() },
)

/** The dirty indicator. One shape, used in the info bar, tabs and the tree. */
@Composable
public fun DirtyDot(color: Int, modifier: Modifier = Modifier, sizeDp: Int = 7) {
    Box(modifier.size(sizeDp.dp).clip(CircleShape).background(Color(color)))
}

@Composable
public fun HSpace(dp: Int) {
    Spacer(Modifier.width(dp.dp))
}

/** A content description on a Box that is not an IconButton. */
public fun Modifier.semanticsLabel(description: String): Modifier =
    this.semantics { contentDescription = description }

/**
 * Vertical drag reporting deltas in pixels (up = negative), then a release.
 * Used by the sheet handle. Deliberately not a Compose draggable state: the sheet
 * height is measured chrome arithmetic, not an animation target.
 */
public fun Modifier.verticalDragHandle(
    onDrag: (Float) -> Unit,
    onRelease: () -> Unit,
): Modifier = this.pointerInput(Unit) {
    detectVerticalDragGestures(
        onDragEnd = { onRelease() },
        onDragCancel = { onRelease() },
        onVerticalDrag = { _, delta -> onDrag(delta) },
    )
}

@Composable
public fun VSpace(dp: Int) {
    Spacer(Modifier.height(dp.dp))
}

// --- text input ------------------------------------------------------------

/**
 * The single-line field used by find, go-to-line, rename and the palette.
 *
 * BasicTextField is correct *here* even though the editor refuses it: these are
 * one-line fields of at most a few hundred characters, and reimplementing IME
 * handling for them would be gratuitous. The editor's objection to
 * BasicTextField is that it lays out a whole document, which does not apply.
 */
@Composable
public fun CopeField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
    autoFocus: Boolean = false,
    numeric: Boolean = false,
    onSubmit: (() -> Unit)? = null,
) {
    val colors = LocalCopeColors.current
    val fonts = LocalCopeFonts.current
    val focusRequester = remember { FocusRequester() }
    if (autoFocus) {
        LaunchedEffect(focusRequester) {
            // One frame of slack: requestFocus() throws if the node is not attached
            // yet, and LaunchedEffect can run before the first layout pass.
            withFrameNanos { }
            try {
                focusRequester.requestFocus()
            } catch (error: IllegalStateException) {
                // The field was removed in the same frame. Nothing to focus.
            }
        }
    }
    Box(
        modifier
            .height(36.dp)
            .background(Color(colors.inputBg))
            .border(1.dp, Color(colors.border))
            .padding(horizontal = 8.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        if (value.isEmpty()) {
            Label(placeholder, colors.dim, sizeSp = CopeDimens.TEXT_SMALL_SP)
        }
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier.fillMaxWidth().focusRequester(focusRequester),
            singleLine = true,
            textStyle = TextStyle(
                color = Color(colors.inputFg),
                fontSize = CopeDimens.TEXT_SP.sp,
                fontFamily = fonts.family,
            ),
            cursorBrush = SolidColor(Color(colors.accent)),
            keyboardOptions = KeyboardOptions(
                keyboardType = if (numeric) KeyboardType.Number else KeyboardType.Ascii,
                autoCorrect = false,
                imeAction = if (onSubmit != null) ImeAction.Done else ImeAction.None,
            ),
            keyboardActions = KeyboardActions(onDone = { onSubmit?.invoke() }),
        )
    }
}

// --- menus and dialogs -----------------------------------------------------

/** The dimmer behind every overlay. Tapping it is always "cancel". */
@Composable
public fun Scrim(onDismiss: () -> Unit) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0x99000000))
            .clickable(onClick = onDismiss)
            .semantics { contentDescription = "Dismiss" },
    )
}

/**
 * One menu row. 46dp, an optional trailing value, an optional destructive tint.
 * ZArchiver's shape: a real list of real actions, no icons-only mystery meat.
 */
@Composable
public fun MenuItem(
    text: String,
    onClick: () -> Unit,
    value: String? = null,
    enabled: Boolean = true,
    destructive: Boolean = false,
    icon: Icon? = null,
) {
    val colors = LocalCopeColors.current
    val tint = when {
        !enabled -> colors.dim
        destructive -> colors.error
        else -> colors.menuFg
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(CopeDimens.MENU_ITEM_HEIGHT.dp)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (icon != null) CopeIconGlyph(icon, tint, sizeDp = 15)
        Label(text, tint, modifier = Modifier.weight(1f))
        if (value != null) Label(value, colors.dim, sizeSp = CopeDimens.TEXT_SMALL_SP)
    }
}

@Composable
public fun MenuSeparator() {
    val colors = LocalCopeColors.current
    HDivider(colors.border, Modifier.padding(vertical = 3.dp))
}

/** The header of a menu or dialog: what this thing is about, stated plainly. */
@Composable
public fun MenuHeader(title: String, subtitle: String? = null) {
    val colors = LocalCopeColors.current
    Column(
        Modifier
            .fillMaxWidth()
            .background(Color(colors.surfaceHeaderBg))
            .padding(horizontal = 14.dp, vertical = 9.dp),
    ) {
        Label(title, colors.surfaceFg, bold = true)
        if (subtitle != null) {
            Label(subtitle, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP, maxLines = 2)
        }
    }
    HDivider(colors.border)
}

/**
 * A centred dialog. Not a Material Dialog: those bring a second window, elevation
 * and rounded corners this app does not use anywhere else.
 */
@Composable
public fun DialogFrame(
    onDismiss: () -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = LocalCopeColors.current
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Scrim(onDismiss)
        Column(
            Modifier
                .fillMaxWidth(0.92f)
                .background(Color(colors.menuBg))
                .border(1.dp, Color(colors.border)),
            content = content,
        )
    }
}

/** The row of actions at the bottom of a dialog. Never a single "OK". */
@Composable
public fun DialogActions(content: @Composable RowScope.() -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

// --- settings controls -----------------------------------------------------

/** A labelled on/off row. The whole row is the target, not a 20dp switch. */
@Composable
public fun ToggleRow(
    text: String,
    checked: Boolean,
    onToggle: (Boolean) -> Unit,
    detail: String? = null,
) {
    val colors = LocalCopeColors.current
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = CopeDimens.MENU_ITEM_HEIGHT.dp)
            .clickable { onToggle(!checked) }
            .padding(horizontal = 14.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Label(text, colors.menuFg)
            if (detail != null) {
                Label(detail, colors.dim, sizeSp = CopeDimens.TEXT_TINY_SP, maxLines = 2)
            }
        }
        Box(
            Modifier
                .width(38.dp)
                .height(20.dp)
                .background(Color(if (checked) colors.accent else colors.keyBg))
                .border(1.dp, Color(colors.border)),
            contentAlignment = if (checked) Alignment.CenterEnd else Alignment.CenterStart,
        ) {
            Box(
                Modifier
                    .padding(horizontal = 3.dp)
                    .size(14.dp)
                    .background(
                        Color(if (checked) contrastOn(colors.accent) else colors.menuFg),
                    ),
            )
        }
    }
}

/** minus / value / plus. The only numeric control in the app. */
@Composable
public fun StepperRow(
    text: String,
    value: String,
    onDecrease: () -> Unit,
    onIncrease: () -> Unit,
    canDecrease: Boolean = true,
    canIncrease: Boolean = true,
) {
    val colors = LocalCopeColors.current
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = CopeDimens.MENU_ITEM_HEIGHT.dp)
            .padding(start = 14.dp, end = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Label(text, colors.menuFg, modifier = Modifier.weight(1f))
        IconButton(
            icon = Icon.ARROW_LEFT,
            description = "Decrease $text",
            onClick = onDecrease,
            enabled = canDecrease,
            sizeDp = 14,
            touchDp = 40,
            tint = colors.menuFg,
        )
        Box(Modifier.width(46.dp), contentAlignment = Alignment.Center) {
            Label(value, colors.accent, bold = true)
        }
        IconButton(
            icon = Icon.ARROW_RIGHT,
            description = "Increase $text",
            onClick = onIncrease,
            enabled = canIncrease,
            sizeDp = 14,
            touchDp = 40,
            tint = colors.menuFg,
        )
    }
}

/** A theme swatch: three bars of the actual colours, so the list is scannable. */
@Composable
public fun Swatch(background: Int, foreground: Int, accent: Int, borderColor: Int) {
    Row(
        Modifier
            .size(width = 34.dp, height = 20.dp)
            .background(Color(background or (0xFF shl 24)))
            .border(1.dp, Color(borderColor))
            .padding(3.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Box(Modifier.weight(2f).height(3.dp).background(Color(foreground or (0xFF shl 24))))
        Box(Modifier.weight(1f).height(3.dp).background(Color(accent or (0xFF shl 24))))
    }
}

/**
 * An empty state that states a fact and offers an action. Never a mascot, never
 * "nothing here yet!".
 */
@Composable
public fun EmptyState(
    text: String,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
) {
    val colors = LocalCopeColors.current
    Column(
        Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Label(text, colors.dim, sizeSp = CopeDimens.TEXT_SMALL_SP, maxLines = 5)
        if (actionLabel != null && onAction != null) {
            PillButton(actionLabel, onAction, emphasised = true)
        }
    }
}

// One theme drives the entire application: the editor, the file tree, the status
// strip, the tabs, the sheet, the menus and the dialogs. This file is the single
// place any chrome colour is decided.
//
// Two guards make that survivable across 60 real VS Code themes:
//
//  * CONTRAST GUARD. Several themes set sideBar.background equal to
//    editor.background, which makes every panel invisible. When two surfaces are
//    within kMinSurfaceDelta luma of each other, the derived one is nudged away
//    and a divider is forced on.
//  * READABILITY GUARD. A derived foreground whose contrast against its own
//    surface is below kMinTextContrast is mixed toward the surface's opposite
//    until it clears the bar. Themes routinely ship a line-number colour that is
//    unreadable on a background they never intended it to sit on.
//
// Alpha matters too: themes give selection and indent-guide colours with alpha
// (e.g. #ca9ee6b3). Backgrounds are flattened over their parent before use, so a
// translucent value never reveals whatever the canvas happened to hold.
package dev.cope.ide.theme

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ProvidableCompositionLocal
import androidx.compose.runtime.compositionLocalOf
import dev.cope.ide.core.ThemeSnapshot
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/** Every colour the UI is allowed to use, as ARGB ints (opaque unless noted). */
@Immutable
public class CopeColors(
    public val isDark: Boolean,
    public val editorBg: Int,
    public val editorFg: Int,
    public val lineHighlight: Int,
    public val selection: Int,
    public val occurrence: Int,
    public val caret: Int,
    public val lineNumber: Int,
    public val lineNumberActive: Int,
    public val indentGuide: Int,
    public val indentGuideActive: Int,
    public val bracketMatchBg: Int,
    public val bracketMatchBorder: Int,
    public val gutterModified: Int,
    public val gutterAdded: Int,
    public val gutterDeleted: Int,
    public val surface: Int,
    public val surfaceFg: Int,
    public val surfaceHeaderBg: Int,
    public val statusBg: Int,
    public val statusFg: Int,
    public val tabActiveBg: Int,
    public val tabActiveFg: Int,
    public val tabInactiveBg: Int,
    public val tabInactiveFg: Int,
    public val tabsBg: Int,
    public val border: Int,
    public val hover: Int,
    public val listActiveBg: Int,
    public val listActiveFg: Int,
    public val inputBg: Int,
    public val inputFg: Int,
    public val accent: Int,
    public val menuBg: Int,
    public val menuFg: Int,
    public val warning: Int,
    public val error: Int,
    public val ok: Int,
    public val scrollbar: Int,
    public val dim: Int,
    public val keyBg: Int,
    public val keyFg: Int,
    /** True when the theme's own surfaces were too close and had to be nudged. */
    public val surfaceWasSynthesized: Boolean,
) {
    public companion object {
        /** A neutral dark scheme, used before a theme loads and if one fails. */
        public val FALLBACK: CopeColors = Derive.from(null)
    }
}

public val LocalCopeColors: ProvidableCompositionLocal<CopeColors> =
    compositionLocalOf { CopeColors.FALLBACK }

public object Derive {

    /** Below this luma difference two surfaces are indistinguishable in practice. */
    private const val MIN_SURFACE_DELTA = 0.02f

    /** WCAG-style contrast ratio floor for secondary text. */
    private const val MIN_TEXT_CONTRAST = 2.6f

    /** Floor for primary text (editor foreground on editor background). */
    private const val MIN_PRIMARY_CONTRAST = 3.4f

    public fun from(snapshot: ThemeSnapshot?): CopeColors {
        val isDark = snapshot?.isDark ?: true
        val ui = snapshot?.uiColors
        fun raw(index: Int): Int =
            if (ui != null && index < ui.size) ui[index] else 0

        // The two colours every other one is derived from. editor.background is
        // present in all 60 bundled themes; editor.foreground in 57.
        val editorBg = opaque(raw(UiKeys.EDITOR_BG), if (isDark) 0xFF1E1E1E.toInt() else 0xFFFFFFFF.toInt())
        val paletteFg = snapshot?.let {
            if (it.styleCount > 0 && (it.flags(0) and ThemeSnapshot.FLAG_HAS_FG) != 0) {
                it.foreground(0)
            } else {
                0
            }
        } ?: 0
        val editorFg = opaque(
            firstNonZero(raw(UiKeys.EDITOR_FG), paletteFg),
            if (isDark) 0xFFD4D4D4.toInt() else 0xFF2B2B2B.toInt(),
        )
        val away = if (isDark) 0xFFFFFFFF.toInt() else 0xFF000000.toInt()
        val toward = if (isDark) 0xFF000000.toInt() else 0xFFFFFFFF.toInt()

        // --- editor surfaces ---
        val lineHighlight = flattenOr(raw(UiKeys.LINE_HIGHLIGHT), editorBg) {
            mix(editorBg, away, if (isDark) 0.05f else 0.04f)
        }
        val selection = flattenOr(raw(UiKeys.SELECTION), editorBg) {
            mix(editorBg, accentOf(snapshot, editorFg), 0.32f)
        }
        val occurrence = flattenOr(raw(UiKeys.SELECTION_HIGHLIGHT), editorBg) {
            mix(editorBg, selection, 0.55f)
        }
        val caret = opaque(firstNonZero(raw(UiKeys.CARET), editorFg), editorFg)

        val lineNumber = readable(
            opaque(raw(UiKeys.LINE_NUMBER), mix(editorBg, editorFg, 0.42f)),
            editorBg, MIN_TEXT_CONTRAST, away,
        )
        val lineNumberActive = readable(
            opaque(firstNonZero(raw(UiKeys.LINE_NUMBER_ACTIVE), editorFg), editorFg),
            lineHighlight, MIN_TEXT_CONTRAST, away,
        )
        // 21 of 60 themes have no indent guide colour at all.
        val indentGuide = flattenOr(raw(UiKeys.INDENT_GUIDE), editorBg) {
            mix(editorBg, editorFg, if (isDark) 0.14f else 0.16f)
        }
        val indentGuideActive = flattenOr(raw(UiKeys.INDENT_GUIDE_ACTIVE), editorBg) {
            mix(editorBg, editorFg, if (isDark) 0.30f else 0.34f)
        }
        // 17 of 60 have no bracket-match colour.
        val bracketMatchBg = flattenOr(raw(UiKeys.BRACKET_MATCH_BG), editorBg) {
            mix(editorBg, away, if (isDark) 0.12f else 0.10f)
        }
        val bracketMatchBorder = flattenOr(raw(UiKeys.BRACKET_MATCH_BORDER), editorBg) {
            mix(editorBg, editorFg, 0.45f)
        }

        val warning = opaque(raw(UiKeys.WARNING), if (isDark) 0xFFCCA700.toInt() else 0xFFBF8803.toInt())
        val error = opaque(raw(UiKeys.ERROR), if (isDark) 0xFFF14C4C.toInt() else 0xFFE51400.toInt())
        val gutterModified = opaque(raw(UiKeys.GUTTER_MODIFIED), if (isDark) 0xFF1B6B93.toInt() else 0xFF2090D3.toInt())
        val gutterAdded = opaque(raw(UiKeys.GUTTER_ADDED), if (isDark) 0xFF487E02.toInt() else 0xFF48985D.toInt())
        val gutterDeleted = opaque(raw(UiKeys.GUTTER_DELETED), error)

        // --- chrome surfaces, with the contrast guard ---
        var surface = flattenOr(
            firstNonZero(raw(UiKeys.SIDEBAR_BG), raw(UiKeys.WIDGET_BG)), editorBg,
        ) {
            mix(editorBg, toward, if (isDark) 0.35f else 0.06f)
        }
        var synthesized = false
        if (abs(luma(surface) - luma(editorBg)) < MIN_SURFACE_DELTA) {
            surface = mix(editorBg, toward, if (isDark) 0.40f else 0.08f)
            if (abs(luma(surface) - luma(editorBg)) < MIN_SURFACE_DELTA) {
                surface = mix(editorBg, away, 0.08f)
            }
            synthesized = true
        }
        val surfaceFg = readable(
            opaque(firstNonZero(raw(UiKeys.SIDEBAR_FG), editorFg), editorFg),
            surface, MIN_TEXT_CONTRAST, away,
        )
        val surfaceHeaderBg = flattenOr(raw(UiKeys.SIDEBAR_HEADER_BG), surface) {
            mix(surface, away, if (isDark) 0.05f else 0.04f)
        }

        val statusBg = flattenOr(firstNonZero(raw(UiKeys.STATUS_BG), raw(UiKeys.SIDEBAR_BG)), surface) {
            surface
        }
        val statusFg = readable(
            opaque(firstNonZero(raw(UiKeys.STATUS_FG), surfaceFg), surfaceFg),
            statusBg, MIN_TEXT_CONTRAST, away,
        )

        val tabsBg = flattenOr(
            firstNonZero(raw(UiKeys.TABS_BG), raw(UiKeys.TAB_INACTIVE_BG), raw(UiKeys.SIDEBAR_BG)),
            surface,
        ) { surface }
        val tabActiveBg = flattenOr(raw(UiKeys.TAB_ACTIVE_BG), tabsBg) { editorBg }
        val tabInactiveBg = flattenOr(raw(UiKeys.TAB_INACTIVE_BG), tabsBg) { tabsBg }
        val tabActiveFg = readable(
            opaque(firstNonZero(raw(UiKeys.TAB_ACTIVE_FG), editorFg), editorFg),
            tabActiveBg, MIN_TEXT_CONTRAST, away,
        )
        val tabInactiveFg = readable(
            opaque(firstNonZero(raw(UiKeys.TAB_INACTIVE_FG), mix(tabInactiveBg, editorFg, 0.6f)),
                mix(tabInactiveBg, editorFg, 0.6f)),
            tabInactiveBg, 2.0f, away,
        )

        val border = flattenOr(
            firstNonZero(raw(UiKeys.PANEL_BORDER), raw(UiKeys.TAB_BORDER), raw(UiKeys.WIDGET_BORDER)),
            surface,
        ) {
            mix(surface, away, if (isDark) 0.10f else 0.14f)
        }
        val hover = flattenOr(raw(UiKeys.LIST_HOVER), surface) {
            mix(surface, away, if (isDark) 0.07f else 0.06f)
        }
        val accent = opaque(accentOf(snapshot, editorFg), editorFg)
        val listActiveBg = flattenOr(raw(UiKeys.LIST_ACTIVE_BG), surface) {
            mix(surface, accent, 0.30f)
        }
        val listActiveFg = readable(
            opaque(firstNonZero(raw(UiKeys.LIST_ACTIVE_FG), surfaceFg), surfaceFg),
            listActiveBg, MIN_TEXT_CONTRAST, away,
        )
        val inputBg = flattenOr(raw(UiKeys.INPUT_BG), surface) {
            mix(surface, toward, if (isDark) 0.25f else 0.03f)
        }
        val inputFg = readable(
            opaque(firstNonZero(raw(UiKeys.INPUT_FG), surfaceFg), surfaceFg),
            inputBg, MIN_PRIMARY_CONTRAST, away,
        )
        val menuBg = flattenOr(firstNonZero(raw(UiKeys.MENU_BG), raw(UiKeys.WIDGET_BG)), surface) {
            mix(surface, away, if (isDark) 0.06f else 0.03f)
        }
        val menuFg = readable(
            opaque(firstNonZero(raw(UiKeys.MENU_FG), surfaceFg), surfaceFg),
            menuBg, MIN_PRIMARY_CONTRAST, away,
        )
        val scrollbar = flattenOr(raw(UiKeys.SCROLLBAR), editorBg) {
            mix(editorBg, editorFg, 0.25f)
        }

        return CopeColors(
            isDark = isDark,
            editorBg = editorBg,
            editorFg = readable(editorFg, editorBg, MIN_PRIMARY_CONTRAST, away),
            lineHighlight = lineHighlight,
            selection = selection,
            occurrence = occurrence,
            caret = caret,
            lineNumber = lineNumber,
            lineNumberActive = lineNumberActive,
            indentGuide = indentGuide,
            indentGuideActive = indentGuideActive,
            bracketMatchBg = bracketMatchBg,
            bracketMatchBorder = bracketMatchBorder,
            gutterModified = gutterModified,
            gutterAdded = gutterAdded,
            gutterDeleted = gutterDeleted,
            surface = surface,
            surfaceFg = surfaceFg,
            surfaceHeaderBg = surfaceHeaderBg,
            statusBg = statusBg,
            statusFg = statusFg,
            tabActiveBg = tabActiveBg,
            tabActiveFg = tabActiveFg,
            tabInactiveBg = tabInactiveBg,
            tabInactiveFg = tabInactiveFg,
            tabsBg = tabsBg,
            border = border,
            hover = hover,
            listActiveBg = listActiveBg,
            listActiveFg = listActiveFg,
            inputBg = inputBg,
            inputFg = inputFg,
            accent = accent,
            menuBg = menuBg,
            menuFg = menuFg,
            warning = warning,
            error = error,
            ok = mixTowardHue(surfaceFg, if (isDark) 0xFF4A8A5A.toInt() else 0xFF2E7D32.toInt()),
            scrollbar = scrollbar,
            dim = readable(mix(surface, surfaceFg, 0.55f), surface, 2.0f, away),
            keyBg = mix(surface, toward, if (isDark) 0.18f else 0.04f),
            keyFg = surfaceFg,
            surfaceWasSynthesized = synthesized,
        )
    }

    // --- colour maths -------------------------------------------------------

    public fun luma(argb: Int): Float {
        fun channel(value: Int): Float {
            val c = value / 255f
            return if (c <= 0.03928f) c / 12.92f else ((c + 0.055f) / 1.055f).pow(2.4f)
        }
        return 0.2126f * channel((argb shr 16) and 0xFF) +
            0.7152f * channel((argb shr 8) and 0xFF) +
            0.0722f * channel(argb and 0xFF)
    }

    public fun contrast(a: Int, b: Int): Float {
        val la = luma(a)
        val lb = luma(b)
        val hi = max(la, lb)
        val lo = min(la, lb)
        return (hi + 0.05f) / (lo + 0.05f)
    }

    /** Linear mix in sRGB space; `t` = 0 keeps `a`. Result is always opaque. */
    public fun mix(a: Int, b: Int, t: Float): Int {
        val k = t.coerceIn(0f, 1f)
        fun lerp(shift: Int): Int {
            val from = (a shr shift) and 0xFF
            val to = (b shr shift) and 0xFF
            return (from + ((to - from) * k)).toInt().coerceIn(0, 255)
        }
        return (0xFF shl 24) or (lerp(16) shl 16) or (lerp(8) shl 8) or lerp(0)
    }

    /** Composites `over` (any alpha) onto `under`, yielding an opaque colour. */
    public fun flatten(over: Int, under: Int): Int {
        val alpha = ((over ushr 24) and 0xFF) / 255f
        if (alpha >= 1f) return over or (0xFF shl 24)
        return mix(under, over or (0xFF shl 24), alpha)
    }

    private inline fun flattenOr(value: Int, under: Int, fallback: () -> Int): Int =
        if (value == 0) fallback() else flatten(value, under)

    private fun opaque(value: Int, fallback: Int): Int =
        if (value == 0) fallback or (0xFF shl 24) else value or (0xFF shl 24)

    private fun firstNonZero(vararg values: Int): Int {
        for (value in values) if (value != 0) return value
        return 0
    }

    private fun accentOf(snapshot: ThemeSnapshot?, editorFg: Int): Int {
        val focus = snapshot?.uiColors?.getOrNull(UiKeys.FOCUS_BORDER) ?: 0
        if (focus != 0) return focus or (0xFF shl 24)
        val caret = snapshot?.uiColors?.getOrNull(UiKeys.CARET) ?: 0
        if (caret != 0) return caret or (0xFF shl 24)
        return editorFg
    }

    /**
     * Pushes `fg` away from `surface` until it clears `minRatio`, in eight bounded
     * steps. Bounded rather than iterative-to-success: a theme whose text can
     * never clear the bar gets the extreme colour, not an infinite loop.
     */
    private fun readable(fg: Int, surface: Int, minRatio: Float, away: Int): Int {
        if (contrast(fg, surface) >= minRatio) return fg
        var result = fg
        var t = 0.12f
        repeat(8) {
            result = mix(fg, away, t)
            if (contrast(result, surface) >= minRatio) return result
            t += 0.12f
        }
        return result
    }

    /** Nudges `base` toward `hint` just enough to read as that hue. */
    private fun mixTowardHue(base: Int, hint: Int): Int = mix(base, hint, 0.75f)
}

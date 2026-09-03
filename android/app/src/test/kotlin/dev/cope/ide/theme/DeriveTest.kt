// The derivation layer is the only place a chrome colour is decided, and it has to
// survive 60 real VS Code themes — including the ones that define ten keys out of
// forty, and the ones whose sidebar is the same colour as the editor.
//
// These tests pin the two guards described in CopeColors.kt, plus the alpha
// flattening, because a regression in any of them makes the app look broken rather
// than throw anything.
package dev.cope.ide.theme

import dev.cope.ide.core.ThemeSnapshot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class DeriveTest {

    /** A snapshot with only the keys named, everything else absent (0). */
    private fun snapshot(
        isDark: Boolean = true,
        vararg keys: Pair<Int, Int>,
        palette: IntArray = intArrayOf(0, 0, 0),
    ): ThemeSnapshot {
        val ui = IntArray(UiKeys.ALL.size)
        for ((index, argb) in keys) ui[index] = argb
        return ThemeSnapshot("test", isDark, palette, ui)
    }

    // --- colour maths -------------------------------------------------------

    @Test
    fun `luma is monotonic between black and white`() {
        val black = Derive.luma(0xFF000000.toInt())
        val grey = Derive.luma(0xFF808080.toInt())
        val white = Derive.luma(0xFFFFFFFF.toInt())
        assertTrue(black < grey)
        assertTrue(grey < white)
        assertEquals(0f, black, 0.001f)
        assertEquals(1f, white, 0.001f)
    }

    @Test
    fun `contrast is symmetric and bounded`() {
        val a = 0xFF101010.toInt()
        val b = 0xFFEEEEEE.toInt()
        assertEquals(Derive.contrast(a, b), Derive.contrast(b, a), 0.0001f)
        assertEquals(1f, Derive.contrast(a, a), 0.0001f)
        assertTrue(Derive.contrast(0xFF000000.toInt(), 0xFFFFFFFF.toInt()) > 20f)
    }

    @Test
    fun `mix at the endpoints returns the endpoints`() {
        val a = 0xFF102030.toInt()
        val b = 0xFF405060.toInt()
        assertEquals(a, Derive.mix(a, b, 0f))
        assertEquals(b, Derive.mix(a, b, 1f))
    }

    @Test
    fun `mix clamps out-of-range factors`() {
        val a = 0xFF000000.toInt()
        val b = 0xFFFFFFFF.toInt()
        assertEquals(a, Derive.mix(a, b, -3f))
        assertEquals(b, Derive.mix(a, b, 7f))
    }

    @Test
    fun `mix always returns an opaque colour`() {
        val result = Derive.mix(0x00112233, 0x00445566, 0.5f)
        assertEquals(0xFF, (result ushr 24) and 0xFF)
    }

    @Test
    fun `flatten composites a translucent colour over its parent`() {
        // 50% white over black is mid grey, and opaque.
        val flattened = Derive.flatten(0x80FFFFFF.toInt(), 0xFF000000.toInt())
        assertEquals(0xFF, (flattened ushr 24) and 0xFF)
        val channel = (flattened shr 16) and 0xFF
        assertTrue("expected mid grey, got $channel", abs(channel - 128) <= 2)
    }

    @Test
    fun `flatten of an opaque colour is the colour itself`() {
        val colour = 0xFF336699.toInt()
        assertEquals(colour, Derive.flatten(colour, 0xFF000000.toInt()))
    }

    @Test
    fun `flatten of a fully transparent colour is the parent`() {
        assertEquals(0xFF223344.toInt(), Derive.flatten(0x00FFFFFF, 0xFF223344.toInt()))
    }

    // --- the fallback scheme ------------------------------------------------

    @Test
    fun `a null snapshot yields a usable dark scheme`() {
        val colors = Derive.from(null)
        assertTrue(colors.isDark)
        assertTrue(Derive.contrast(colors.editorFg, colors.editorBg) >= 3.4f)
        assertTrue(Derive.contrast(colors.surfaceFg, colors.surface) >= 2.5f)
        // Every colour must be opaque: they are handed to Canvas and to Compose
        // without further compositing.
        for (colour in listOf(
            colors.editorBg, colors.editorFg, colors.surface, colors.surfaceFg,
            colors.statusBg, colors.statusFg, colors.tabsBg, colors.border,
            colors.accent, colors.menuBg, colors.menuFg, colors.keyBg, colors.keyFg,
            colors.selection, colors.lineHighlight, colors.caret, colors.dim,
        )) {
            assertEquals("not opaque: ${Integer.toHexString(colour)}", 0xFF, (colour ushr 24) and 0xFF)
        }
    }

    // --- the contrast guard -------------------------------------------------

    @Test
    fun `contrast guard fires when the sidebar equals the editor background`() {
        val bg = 0xFF1E1E1E.toInt()
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to bg,
                UiKeys.EDITOR_FG to 0xFFD4D4D4.toInt(),
                UiKeys.SIDEBAR_BG to bg, // the pathological case
            ),
        )
        assertTrue(colors.surfaceWasSynthesized)
        assertNotEquals(colors.editorBg, colors.surface)
        assertTrue(
            "surfaces still indistinguishable",
            abs(Derive.luma(colors.surface) - Derive.luma(colors.editorBg)) >= 0.02f,
        )
    }

    @Test
    fun `a distinct sidebar is left alone`() {
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF1E1E1E.toInt(),
                UiKeys.EDITOR_FG to 0xFFD4D4D4.toInt(),
                UiKeys.SIDEBAR_BG to 0xFF252526.toInt(),
            ),
        )
        assertFalse(colors.surfaceWasSynthesized)
        assertEquals(0xFF252526.toInt(), colors.surface)
    }

    // --- the readability guard ----------------------------------------------

    @Test
    fun `readability guard rescues an unreadable line number colour`() {
        // A theme that puts near-black line numbers on a near-black editor. The
        // guard must push them until they are legible.
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF101010.toInt(),
                UiKeys.EDITOR_FG to 0xFFE0E0E0.toInt(),
                UiKeys.LINE_NUMBER to 0xFF121212.toInt(),
            ),
        )
        assertTrue(
            "line numbers unreadable at ${Derive.contrast(colors.lineNumber, colors.editorBg)}",
            Derive.contrast(colors.lineNumber, colors.editorBg) >= 2.5f,
        )
    }

    @Test
    fun `readability guard rescues an unreadable editor foreground`() {
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF202020.toInt(),
                UiKeys.EDITOR_FG to 0xFF262626.toInt(),
            ),
        )
        assertTrue(Derive.contrast(colors.editorFg, colors.editorBg) >= 3.3f)
    }

    @Test
    fun `a light theme derives light surfaces`() {
        val colors = Derive.from(
            snapshot(
                isDark = false,
                UiKeys.EDITOR_BG to 0xFFFFFFFF.toInt(),
                UiKeys.EDITOR_FG to 0xFF1F1F1F.toInt(),
            ),
        )
        assertFalse(colors.isDark)
        assertTrue(Derive.luma(colors.surface) > 0.5f)
        assertTrue(Derive.contrast(colors.surfaceFg, colors.surface) >= 2.5f)
    }

    // --- alpha and fallbacks ------------------------------------------------

    @Test
    fun `a translucent selection is flattened over the editor background`() {
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF000000.toInt(),
                UiKeys.EDITOR_FG to 0xFFFFFFFF.toInt(),
                UiKeys.SELECTION to 0x80FFFFFF.toInt(),
            ),
        )
        assertEquals(0xFF, (colors.selection ushr 24) and 0xFF)
        val channel = (colors.selection shr 16) and 0xFF
        assertTrue("expected mid grey, got $channel", channel in 120..136)
    }

    @Test
    fun `the caret falls back to the editor foreground`() {
        val fg = 0xFFCCCCCC.toInt()
        val colors = Derive.from(
            snapshot(isDark = true, UiKeys.EDITOR_BG to 0xFF000000.toInt(), UiKeys.EDITOR_FG to fg),
        )
        assertEquals(fg, colors.caret)
    }

    @Test
    fun `focusBorder becomes the accent when present`() {
        val focus = 0xFF00AAFF.toInt()
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF000000.toInt(),
                UiKeys.EDITOR_FG to 0xFFFFFFFF.toInt(),
                UiKeys.FOCUS_BORDER to focus,
            ),
        )
        assertEquals(focus, colors.accent)
    }

    @Test
    fun `the editor foreground can come from the palette default style`() {
        // 57 of 60 themes set editor.foreground; the others only have a default
        // token colour, which is where this path matters.
        val palette = intArrayOf(0xFFABCDEF.toInt(), 0, ThemeSnapshot.FLAG_HAS_FG)
        val colors = Derive.from(
            snapshot(
                isDark = true,
                UiKeys.EDITOR_BG to 0xFF000000.toInt(),
                palette = palette,
            ),
        )
        assertEquals(0xFFABCDEF.toInt(), colors.editorFg)
    }

    @Test
    fun `every derived colour of a minimal theme is opaque`() {
        // The worst realistic case: one key defined out of forty.
        val colors = Derive.from(snapshot(isDark = true, UiKeys.EDITOR_BG to 0xFF0B0E14.toInt()))
        for (colour in listOf(
            colors.editorBg, colors.editorFg, colors.lineHighlight, colors.selection,
            colors.occurrence, colors.caret, colors.lineNumber, colors.lineNumberActive,
            colors.indentGuide, colors.indentGuideActive, colors.bracketMatchBg,
            colors.bracketMatchBorder, colors.surface, colors.surfaceFg,
            colors.surfaceHeaderBg, colors.statusBg, colors.statusFg, colors.tabActiveBg,
            colors.tabActiveFg, colors.tabInactiveBg, colors.tabInactiveFg, colors.tabsBg,
            colors.border, colors.hover, colors.listActiveBg, colors.listActiveFg,
            colors.inputBg, colors.inputFg, colors.accent, colors.menuBg, colors.menuFg,
            colors.warning, colors.error, colors.ok, colors.scrollbar, colors.dim,
            colors.keyBg, colors.keyFg,
        )) {
            assertEquals("not opaque: ${Integer.toHexString(colour)}", 0xFF, (colour ushr 24) and 0xFF)
        }
    }

    @Test
    fun `a short uiColors array cannot crash the derivation`() {
        // A truncated JNI result must degrade to fallbacks, not an exception.
        val snapshot = ThemeSnapshot("short", true, intArrayOf(0, 0, 0), IntArray(3))
        val colors = Derive.from(snapshot)
        assertTrue(Derive.contrast(colors.editorFg, colors.editorBg) >= 3.3f)
    }

    @Test
    fun `an empty palette cannot crash the derivation`() {
        val snapshot = ThemeSnapshot("empty", true, IntArray(0), IntArray(UiKeys.ALL.size))
        val colors = Derive.from(snapshot)
        assertEquals(0, snapshot.styleCount)
        assertTrue(Derive.contrast(colors.editorFg, colors.editorBg) >= 3.3f)
    }

    @Test
    fun `the ui key list and the index constants agree`() {
        // Off-by-one here silently colours the app with the wrong keys, which is
        // exactly the kind of bug no screenshot review would catch.
        assertEquals(40, UiKeys.ALL.size)
        assertEquals("editor.background", UiKeys.ALL[UiKeys.EDITOR_BG])
        assertEquals("editor.foreground", UiKeys.ALL[UiKeys.EDITOR_FG])
        assertEquals("editor.selectionBackground", UiKeys.ALL[UiKeys.SELECTION])
        assertEquals("editorCursor.foreground", UiKeys.ALL[UiKeys.CARET])
        assertEquals("sideBar.background", UiKeys.ALL[UiKeys.SIDEBAR_BG])
        assertEquals("statusBar.background", UiKeys.ALL[UiKeys.STATUS_BG])
        assertEquals("tab.activeBackground", UiKeys.ALL[UiKeys.TAB_ACTIVE_BG])
        assertEquals("panel.border", UiKeys.ALL[UiKeys.PANEL_BORDER])
        assertEquals("focusBorder", UiKeys.ALL[UiKeys.FOCUS_BORDER])
        assertEquals("scrollbarSlider.background", UiKeys.ALL[UiKeys.SCROLLBAR])
    }
}

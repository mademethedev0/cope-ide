package dev.cope.ide.editor

import dev.cope.ide.core.ThemeSnapshot
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TokenBackgroundTest {
    @Test fun inheritedBackgroundDoesNotEraseSelectionOrCurrentLine() {
        val bg = 0xFF101010.toInt()
        assertFalse(TokenBackground.shouldPaint(ThemeSnapshot.FLAG_HAS_BG, bg, bg, bg))
        assertFalse(TokenBackground.shouldPaint(ThemeSnapshot.FLAG_HAS_BG, 0, bg, bg))
        assertFalse(TokenBackground.shouldPaint(0, 0xFF802020.toInt(), bg, bg))
    }

    @Test fun explicitDistinctBackgroundIsPreserved() {
        assertTrue(TokenBackground.shouldPaint(ThemeSnapshot.FLAG_HAS_BG,
            0x80802020.toInt(), 0xFF101010.toInt(), 0xFF202020.toInt()))
    }
}

// LineLayout is the Kotlin half of a contract with the C++ engine: both must
// agree on what display column a byte offset sits at. Every case here mirrors a
// rule in Document::displayColumnOf.
package dev.cope.ide.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LineLayoutTest {

    private val layout = LineLayout()

    private fun lay(text: String, tabWidth: Int = 4) {
        val bytes = text.toByteArray(Charsets.UTF_8)
        layout.layout(bytes, 0, bytes.size, tabWidth)
    }

    @Test
    fun `ascii is one cell per byte`() {
        lay("abc")
        assertEquals(3, layout.cellCount)
        assertEquals(3, layout.columns)
        assertEquals(3, layout.byteLength)
        assertEquals(0, layout.cellColumn[0])
        assertEquals(1, layout.cellColumn[1])
        assertEquals(2, layout.cellColumn[2])
    }

    @Test
    fun `a tab advances to the next multiple of the tab width`() {
        lay("a\tb", tabWidth = 4)
        assertEquals(3, layout.cellCount)
        assertEquals(0, layout.cellColumn[0])
        assertEquals(1, layout.cellColumn[1]) // the tab starts at column 1
        assertEquals(4, layout.cellColumn[2]) // and lands 'b' on column 4
        assertEquals(5, layout.columns)
        assertTrue(layout.cellIsTab[1])
        assertFalse(layout.cellIsTab[0])
    }

    @Test
    fun `a tab at a stop still advances a full width`() {
        lay("abcd\te", tabWidth = 4)
        assertEquals(4, layout.cellColumn[4]) // tab begins exactly on a stop
        assertEquals(8, layout.cellColumn[5]) // so it must jump to the next one
    }

    @Test
    fun `a tab emits no characters`() {
        lay("a\tb")
        // 'a' and 'b' only: the tab is a gap, so drawText never sees it.
        assertEquals(2, layout.charCount)
        assertEquals(0, layout.cellCharCount[1])
    }

    @Test
    fun `tab width of one collapses tabs to a single column`() {
        lay("a\tb", tabWidth = 1)
        assertEquals(3, layout.columns)
    }

    @Test
    fun `a tab width below one is clamped rather than dividing by zero`() {
        lay("a\tb", tabWidth = 0)
        assertEquals(3, layout.columns)
    }

    @Test
    fun `cjk takes two columns and three bytes`() {
        lay("\u4E2D\u6587a") // 中文a
        assertEquals(3, layout.cellCount)
        assertEquals(0, layout.cellColumn[0])
        assertEquals(2, layout.cellColumn[1])
        assertEquals(4, layout.cellColumn[2])
        assertEquals(5, layout.columns)
        assertEquals(0, layout.cellByteOffset[0])
        assertEquals(3, layout.cellByteOffset[1])
        assertEquals(6, layout.cellByteOffset[2])
    }

    @Test
    fun `a combining mark takes no column`() {
        lay("e\u0301x") // e + combining acute
        assertEquals(3, layout.cellCount)
        assertEquals(0, layout.cellColumn[0])
        assertEquals(1, layout.cellColumn[1]) // the mark sits on the 'e' cell edge
        assertEquals(1, layout.cellColumn[2]) // and contributes nothing
        assertEquals(2, layout.columns)
    }

    @Test
    fun `an astral codepoint is two columns and a surrogate pair`() {
        lay("\uD83D\uDE80x") // 🚀 U+1F680
        assertEquals(2, layout.cellCount)
        assertEquals(2, layout.cellCharCount[0]) // two UTF-16 units
        assertEquals(2, layout.cellColumn[1])
        assertEquals(3, layout.columns)
        assertEquals(3, layout.charCount)
    }

    @Test
    fun `columnOfByte agrees with the cell table`() {
        lay("ab\tcd")
        assertEquals(0, layout.columnOfByte(0))
        assertEquals(1, layout.columnOfByte(1))
        assertEquals(2, layout.columnOfByte(2)) // the tab
        assertEquals(4, layout.columnOfByte(3)) // 'c' after the tab
        assertEquals(5, layout.columnOfByte(4))
        assertEquals(6, layout.columnOfByte(5)) // end of line
    }

    @Test
    fun `columnOfByte clamps outside the line`() {
        lay("abc")
        assertEquals(0, layout.columnOfByte(-5))
        assertEquals(3, layout.columnOfByte(99))
    }

    @Test
    fun `byteOfColumn snaps to the nearer cell edge`() {
        lay("a\tb", tabWidth = 4)
        // The tab spans columns 1..3. Tapping its left half stays before it, the
        // right half lands after it — the behaviour every editor has.
        assertEquals(1, layout.byteOfColumn(1))
        assertEquals(2, layout.byteOfColumn(3))
        assertEquals(0, layout.byteOfColumn(0))
    }

    @Test
    fun `byteOfColumn round-trips every column of an ascii line`() {
        lay("hello world")
        for (byte in 0..11) {
            val column = layout.columnOfByte(byte)
            assertEquals(byte, layout.byteOfColumn(column))
        }
    }

    @Test
    fun `byteOfColumn past the end returns the line length`() {
        lay("abc")
        assertEquals(3, layout.byteOfColumn(50))
    }

    @Test
    fun `cellIndexOfByte finds the cell starting at or after an offset`() {
        lay("a\u4E2Db") // a 中 b -> bytes 0, 1..3, 4
        assertEquals(0, layout.cellIndexOfByte(0))
        assertEquals(1, layout.cellIndexOfByte(1))
        // Mid-codepoint: the next cell that starts at or after byte 2 is index 2.
        assertEquals(2, layout.cellIndexOfByte(2))
        assertEquals(2, layout.cellIndexOfByte(4))
        assertEquals(3, layout.cellIndexOfByte(5))
    }

    @Test
    fun `an empty line has no cells and zero columns`() {
        lay("")
        assertEquals(0, layout.cellCount)
        assertEquals(0, layout.columns)
        assertEquals(0, layout.columnOfByte(0))
        assertEquals(0, layout.byteOfColumn(0))
        assertEquals(0, layout.byteOfColumn(9))
    }

    @Test
    fun `invalid utf8 becomes one replacement cell per bad byte`() {
        val bytes = byteArrayOf(0x61, 0xC3.toByte(), 0x28, 0x62)
        layout.layout(bytes, 0, bytes.size, 4)
        // 'a', a truncated 2-byte lead, '(', 'b' — four cells, four columns, no
        // crash and no swallowed bytes.
        assertEquals(4, layout.cellCount)
        assertEquals(4, layout.columns)
        assertEquals(0xFFFD.toChar(), layout.chars[1])
    }

    @Test
    fun `a lone continuation byte is one replacement cell`() {
        val bytes = byteArrayOf(0x80.toByte(), 0x61)
        layout.layout(bytes, 0, bytes.size, 4)
        assertEquals(2, layout.cellCount)
        assertEquals(0xFFFD.toChar(), layout.chars[0])
    }

    @Test
    fun `an overlong encoding is rejected as a replacement`() {
        // C0 80 encodes U+0000 in two bytes. Accepting it is a classic security bug.
        val bytes = byteArrayOf(0xC0.toByte(), 0x80.toByte())
        layout.layout(bytes, 0, bytes.size, 4)
        assertEquals(1, layout.cellCount)
        assertEquals(0xFFFD.toChar(), layout.chars[0])
    }

    @Test
    fun `a surrogate encoded in utf8 is rejected`() {
        // ED A0 80 is U+D800, illegal in UTF-8.
        val bytes = byteArrayOf(0xED.toByte(), 0xA0.toByte(), 0x80.toByte())
        layout.layout(bytes, 0, bytes.size, 4)
        assertEquals(1, layout.cellCount)
        assertEquals(0xFFFD.toChar(), layout.chars[0])
    }

    @Test
    fun `a truncated multibyte sequence at end of line does not read past it`() {
        val bytes = byteArrayOf(0x61, 0xE4.toByte(), 0xB8.toByte())
        layout.layout(bytes, 0, bytes.size, 4)
        // 'a' plus two undecodable bytes. The important part is that it terminates.
        assertEquals(3, layout.cellCount)
        assertEquals(3, layout.byteLength)
    }

    @Test
    fun `layout honours an offset into a larger buffer`() {
        val bytes = "XXXabcYYY".toByteArray(Charsets.UTF_8)
        layout.layout(bytes, 3, 3, 4)
        assertEquals(3, layout.cellCount)
        assertEquals(3, layout.byteLength)
        assertEquals('a', layout.chars[0])
        assertEquals('c', layout.chars[2])
    }

    @Test
    fun `reuse across lines resets state`() {
        lay("a very long line indeed, quite long")
        val firstColumns = layout.columns
        lay("x")
        assertEquals(1, layout.cellCount)
        assertEquals(1, layout.columns)
        assertEquals(1, layout.charCount)
        assertTrue(firstColumns > 1)
    }

    @Test
    fun `display widths match the generated tables`() {
        assertEquals(1, displayWidthOf('a'.code))
        assertEquals(1, displayWidthOf(0x00E9)) // é
        assertEquals(0, displayWidthOf(0x0301)) // combining acute
        assertEquals(0, displayWidthOf(0x200B)) // zero width space
        assertEquals(2, displayWidthOf(0x4E2D)) // 中
        assertEquals(2, displayWidthOf(0x1F680)) // 🚀
        assertEquals(1, displayWidthOf(0x2500)) // box drawing: narrow
    }
}

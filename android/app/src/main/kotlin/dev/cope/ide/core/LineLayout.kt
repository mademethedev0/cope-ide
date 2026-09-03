// One line, decoded once per draw into flat reusable arrays.
//
// Why this class exists: the editor is a character grid, so drawing a line needs
// three things the raw UTF-8 bytes do not give directly — the UTF-16 units the
// platform Canvas draws, the display column each codepoint starts at (tabs
// expand, combining marks take no cell, CJK and emoji take two), and the mapping
// both ways between byte offsets (what the engine speaks) and columns (what the
// screen speaks).
//
// Everything is a preallocated array that grows but never shrinks, so scrolling
// allocates nothing after the first few frames.
//
// This duplicates Document::displayColumnOf's rules on purpose (see
// DisplayWidth.kt, generated from the same tables): a JNI call per codepoint per
// frame is not affordable, and hit-testing must be instant.
package dev.cope.ide.core

public class LineLayout {

    /** UTF-16 units of the line, decoded from UTF-8 with U+FFFD for bad bytes. */
    public var chars: CharArray = CharArray(128)
        private set
    public var charCount: Int = 0
        private set

    /** Per cell (= per codepoint) parallel arrays; `cellCount` entries are valid. */
    public var cellColumn: IntArray = IntArray(128)
        private set
    public var cellCharIndex: IntArray = IntArray(128)
        private set
    public var cellCharCount: IntArray = IntArray(128)
        private set
    public var cellByteOffset: IntArray = IntArray(128)
        private set
    public var cellIsTab: BooleanArray = BooleanArray(128)
        private set
    public var cellCount: Int = 0
        private set

    /** Total display columns of the line, and its length in bytes. */
    public var columns: Int = 0
        private set
    public var byteLength: Int = 0
        private set

    public fun layout(bytes: ByteArray, offset: Int, length: Int, tabWidth: Int) {
        val tab = if (tabWidth < 1) 1 else tabWidth
        ensure(length + 1)
        charCount = 0
        cellCount = 0
        byteLength = length
        var column = 0
        var at = 0
        while (at < length) {
            val start = at
            val b0 = bytes[offset + at].toInt() and 0xFF
            var cp: Int
            var width: Int
            when {
                b0 < 0x80 -> {
                    cp = b0
                    width = 1
                }
                b0 and 0xE0 == 0xC0 && at + 1 < length && isCont(bytes, offset + at + 1) -> {
                    cp = ((b0 and 0x1F) shl 6) or cont(bytes, offset + at + 1)
                    width = 2
                    if (cp < 0x80) cp = REPLACEMENT
                }
                b0 and 0xF0 == 0xE0 && at + 2 < length &&
                    isCont(bytes, offset + at + 1) && isCont(bytes, offset + at + 2) -> {
                    cp = ((b0 and 0x0F) shl 12) or
                        (cont(bytes, offset + at + 1) shl 6) or
                        cont(bytes, offset + at + 2)
                    width = 3
                    if (cp < 0x800 || (cp in 0xD800..0xDFFF)) cp = REPLACEMENT
                }
                b0 and 0xF8 == 0xF0 && at + 3 < length &&
                    isCont(bytes, offset + at + 1) && isCont(bytes, offset + at + 2) &&
                    isCont(bytes, offset + at + 3) -> {
                    cp = ((b0 and 0x07) shl 18) or
                        (cont(bytes, offset + at + 1) shl 12) or
                        (cont(bytes, offset + at + 2) shl 6) or
                        cont(bytes, offset + at + 3)
                    width = 4
                    if (cp < 0x10000 || cp > 0x10FFFF) cp = REPLACEMENT
                }
                else -> {
                    cp = REPLACEMENT
                    width = 1
                }
            }
            at += width

            val isTab = cp == '\t'.code
            val charIndex = charCount
            val startColumn = column
            if (isTab) {
                // A tab is a gap, not a glyph: no chars are emitted for it.
                column = ((column / tab) + 1) * tab
            } else {
                if (cp <= 0xFFFF) {
                    chars[charCount++] = cp.toChar()
                } else {
                    val v = cp - 0x10000
                    chars[charCount++] = (0xD800 + (v shr 10)).toChar()
                    chars[charCount++] = (0xDC00 + (v and 0x3FF)).toChar()
                }
                column += displayWidthOf(cp)
            }

            cellColumn[cellCount] = startColumn
            cellCharIndex[cellCount] = charIndex
            cellCharCount[cellCount] = charCount - charIndex
            cellByteOffset[cellCount] = start
            cellIsTab[cellCount] = isTab
            cellCount++
        }
        columns = column
    }

    /** Display column of a byte offset within the line, clamped. */
    public fun columnOfByte(byteOffset: Int): Int {
        if (byteOffset <= 0) return 0
        if (byteOffset >= byteLength) return columns
        var lo = 0
        var hi = cellCount
        while (lo < hi) {
            val mid = lo + (hi - lo) / 2
            if (cellByteOffset[mid] < byteOffset) lo = mid + 1 else hi = mid
        }
        return if (lo < cellCount) cellColumn[lo] else columns
    }

    /**
     * Byte offset for a display column, snapped to the nearer cell edge. Snapping
     * to the nearer edge (rather than always down) is what makes tapping the right
     * half of a tab or a CJK glyph put the caret after it, like every other editor.
     */
    public fun byteOfColumn(column: Int): Int {
        if (column <= 0 || cellCount == 0) return 0
        var index = -1
        for (i in 0 until cellCount) {
            if (cellColumn[i] <= column) index = i else break
        }
        if (index < 0) return 0
        val startColumn = cellColumn[index]
        val endColumn = if (index + 1 < cellCount) cellColumn[index + 1] else columns
        val nextByte = if (index + 1 < cellCount) cellByteOffset[index + 1] else byteLength
        val span = endColumn - startColumn
        if (span <= 0) return nextByte
        return if (column - startColumn <= span / 2) cellByteOffset[index] else nextByte
    }

    /** Index of the cell starting at or after `byteOffset`. */
    public fun cellIndexOfByte(byteOffset: Int): Int {
        var lo = 0
        var hi = cellCount
        while (lo < hi) {
            val mid = lo + (hi - lo) / 2
            if (cellByteOffset[mid] < byteOffset) lo = mid + 1 else hi = mid
        }
        return lo
    }

    private fun ensure(cells: Int) {
        // Worst case: every byte is one cell and one char (ASCII), plus a slot for
        // the end-of-line position.
        if (chars.size < cells * 2) chars = CharArray(cells * 2)
        if (cellColumn.size < cells) {
            cellColumn = IntArray(cells)
            cellCharIndex = IntArray(cells)
            cellCharCount = IntArray(cells)
            cellByteOffset = IntArray(cells)
            cellIsTab = BooleanArray(cells)
        }
    }

    private companion object {
        const val REPLACEMENT = 0xFFFD

        fun isCont(bytes: ByteArray, index: Int): Boolean =
            (bytes[index].toInt() and 0xC0) == 0x80

        fun cont(bytes: ByteArray, index: Int): Int = bytes[index].toInt() and 0x3F
    }
}

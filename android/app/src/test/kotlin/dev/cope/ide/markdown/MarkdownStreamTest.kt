// The markdown preview stream is a text protocol between md_stream.h and
// MarkdownStream.kt. These tests pin the escaping (both directions), the block
// nesting, and the malformed-input behaviour — a truncated stream must render what
// it can, never throw.
package dev.cope.ide.markdown

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MarkdownStreamTest {

    // --- escaping -----------------------------------------------------------

    @Test
    fun `escape mirrors escapeInto`() {
        assertEquals("a\\\\b", MarkdownStream.escape("a\\b"))
        assertEquals("a\\nb", MarkdownStream.escape("a\nb"))
        assertEquals("a\\tb", MarkdownStream.escape("a\tb"))
        assertEquals("ab", MarkdownStream.escape("a\rb"))
        assertEquals("plain", MarkdownStream.escape("plain"))
    }

    @Test
    fun `unescape reverses escape`() {
        for (text in listOf(
            "plain",
            "a\\b",
            "line\nbreak",
            "tab\there",
            "\\\\\\",
            "mixed \\ \n \t end",
            "",
        )) {
            assertEquals(text, MarkdownStream.unescape(MarkdownStream.escape(text)))
        }
    }

    @Test
    fun `unescape keeps an unknown escape verbatim`() {
        // Not a sequence the format defines: dropping the backslash would silently
        // corrupt Windows paths in code spans.
        assertEquals("\\q", MarkdownStream.unescape("\\q"))
    }

    @Test
    fun `unescape tolerates a trailing lone backslash`() {
        assertEquals("abc\\", MarkdownStream.unescape("abc\\"))
    }

    @Test
    fun `unescape without a backslash returns the same string`() {
        val text = "nothing to do here"
        assertEquals(text, MarkdownStream.unescape(text))
    }

    // --- structure ----------------------------------------------------------

    @Test
    fun `a heading and a paragraph parse`() {
        val stream = "b\th\t2\nt\t0\t\tTitle\ne\nb\tp\t\nt\t0\t\tBody text\ne\n"
        val blocks = MarkdownStream.parse(stream)
        assertEquals(2, blocks.size)
        assertEquals("h", blocks[0].kind)
        assertEquals(2, blocks[0].level)
        assertEquals("Title", blocks[0].text())
        assertEquals("p", blocks[1].kind)
        assertEquals("Body text", blocks[1].text())
    }

    @Test
    fun `style bits decode`() {
        val mask = MdStyle.BOLD or MdStyle.ITALIC or MdStyle.LINK
        val blocks = MarkdownStream.parse("b\tp\t\nt\t$mask\thttps://x\tclick\ne\n")
        val run = blocks[0].runs[0]
        assertTrue(run.bold)
        assertTrue(run.italic)
        assertTrue(run.link)
        assertFalse(run.code)
        assertFalse(run.strike)
        assertEquals("https://x", run.href)
        assertEquals("click", run.text)
    }

    @Test
    fun `an unparsable mask becomes zero rather than throwing`() {
        val blocks = MarkdownStream.parse("b\tp\t\nt\tnotanumber\t\thello\ne\n")
        assertEquals(0, blocks[0].runs[0].mask)
        assertEquals("hello", blocks[0].runs[0].text)
    }

    @Test
    fun `nested blocks build a tree`() {
        val stream = buildString {
            append("b\tul\t\n")
            append("b\tli\t\n")
            append("b\tp\t\n")
            append("t\t0\t\tfirst\n")
            append("e\n")
            append("e\n")
            append("b\tli\t\n")
            append("b\tp\t\n")
            append("t\t0\t\tsecond\n")
            append("e\n")
            append("e\n")
            append("e\n")
        }
        val blocks = MarkdownStream.parse(stream)
        assertEquals(1, blocks.size)
        assertEquals("ul", blocks[0].kind)
        assertEquals(2, blocks[0].children.size)
        assertEquals("li", blocks[0].children[0].kind)
        assertEquals("first", blocks[0].children[0].children[0].text())
        assertEquals("second", blocks[0].children[1].children[0].text())
    }

    @Test
    fun `a code block carries its language and body`() {
        val body = MarkdownStream.escape("fn main() {\n    println!(\"hi\");\n}\n")
        val blocks = MarkdownStream.parse("b\tcode\trust\nt\t${MdStyle.CODE}\t\t$body\ne\n")
        assertEquals("code", blocks[0].kind)
        assertEquals("rust", blocks[0].arg)
        assertTrue(blocks[0].text().startsWith("fn main()"))
        assertTrue(blocks[0].text().contains('\n'))
        assertTrue(blocks[0].runs[0].code)
    }

    @Test
    fun `a table keeps rows, cells and alignment`() {
        val stream = buildString {
            append("b\ttable\t\n")
            append("b\tthead\t\n")
            append("b\tcell\tl\nt\t0\t\tName\ne\n")
            append("b\tcell\tr\nt\t0\t\tSize\ne\n")
            append("e\n")
            append("b\ttrow\t\n")
            append("b\tcell\tl\nt\t0\t\ta.txt\ne\n")
            append("b\tcell\tr\nt\t0\t\t12\ne\n")
            append("e\n")
            append("e\n")
        }
        val table = MarkdownStream.parse(stream).single()
        assertEquals("table", table.kind)
        assertEquals(2, table.children.size)
        assertEquals("thead", table.children[0].kind)
        assertEquals("trow", table.children[1].kind)
        assertEquals('l', table.children[0].children[0].align)
        assertEquals('r', table.children[0].children[1].align)
        assertEquals("12", table.children[1].children[1].text())
    }

    @Test
    fun `a heading level outside 1 to 6 is clamped`() {
        assertEquals(6, MarkdownStream.parse("b\th\t9\ne\n").single().level)
        assertEquals(1, MarkdownStream.parse("b\th\t0\ne\n").single().level)
        assertEquals(1, MarkdownStream.parse("b\th\tnope\ne\n").single().level)
    }

    @Test
    fun `level is zero for anything that is not a heading`() {
        assertEquals(0, MarkdownStream.parse("b\tp\t\ne\n").single().level)
    }

    @Test
    fun `text() concatenates multiple runs`() {
        val blocks = MarkdownStream.parse("b\tp\t\nt\t0\t\tone \nt\t1\t\ttwo\ne\n")
        assertEquals("one two", blocks[0].text())
    }

    // --- malformed input ----------------------------------------------------

    @Test
    fun `an empty stream yields no blocks`() {
        assertTrue(MarkdownStream.parse("").isEmpty())
        assertTrue(MarkdownStream.parse(ByteArray(0)).isEmpty())
    }

    @Test
    fun `a stream missing its end records still yields the blocks`() {
        val blocks = MarkdownStream.parse("b\tp\t\nt\t0\t\tunterminated\n")
        assertEquals(1, blocks.size)
        assertEquals("unterminated", blocks[0].text())
    }

    @Test
    fun `surplus end records are ignored`() {
        val blocks = MarkdownStream.parse("e\ne\nb\tp\t\nt\t0\t\tok\ne\ne\ne\n")
        assertEquals(1, blocks.size)
        assertEquals("ok", blocks[0].text())
    }

    @Test
    fun `a run with no open block is wrapped in a paragraph`() {
        val blocks = MarkdownStream.parse("t\t0\t\torphan\n")
        assertEquals(1, blocks.size)
        assertEquals("p", blocks[0].kind)
        assertEquals("orphan", blocks[0].text())
    }

    @Test
    fun `an unknown record type is skipped`() {
        val blocks = MarkdownStream.parse("z\twhatever\nb\tp\t\nt\t0\t\tkept\ne\n")
        assertEquals(1, blocks.size)
        assertEquals("kept", blocks[0].text())
    }

    @Test
    fun `missing fields come back empty rather than throwing`() {
        val blocks = MarkdownStream.parse("b\n")
        assertEquals(1, blocks.size)
        assertEquals("", blocks[0].kind)
        assertEquals("", blocks[0].arg)
    }

    @Test
    fun `absurd nesting is flattened instead of overflowing`() {
        val stream = buildString {
            repeat(500) { append("b\tquote\t\n") }
            append("t\t0\t\tdeep\n")
            repeat(500) { append("e\n") }
        }
        val blocks = MarkdownStream.parse(stream)
        // One root, and the text survived somewhere in the tree.
        assertEquals(1, blocks.size)
        var depth = 0
        var node = blocks[0]
        while (node.children.isNotEmpty()) {
            node = node.children[0]
            depth++
            if (depth > 600) break
        }
        assertTrue("nesting was not bounded: $depth", depth <= 64)
    }

    @Test
    fun `a stream without a trailing newline parses its last record`() {
        val blocks = MarkdownStream.parse("b\tp\t\nt\t0\t\tlast")
        assertEquals("last", blocks[0].text())
    }

    @Test
    fun `parsing from bytes decodes utf8`() {
        val stream = "b\tp\t\nt\t0\t\t\u4E2D\u6587 \uD83D\uDE80\ne\n"
        val blocks = MarkdownStream.parse(stream.toByteArray(Charsets.UTF_8))
        assertEquals("\u4E2D\u6587 \uD83D\uDE80", blocks[0].text())
    }

    @Test
    fun `tabs inside a payload cannot break the framing`() {
        // The C++ side escapes tabs, so a code span containing one arrives as \t and
        // must not be read as a field separator.
        val payload = MarkdownStream.escape("a\tb")
        val blocks = MarkdownStream.parse("b\tp\t\nt\t${MdStyle.CODE}\t\t$payload\ne\n")
        assertEquals(1, blocks[0].runs.size)
        assertEquals("a\tb", blocks[0].runs[0].text)
    }

    @Test
    fun `an image run carries its target as href`() {
        val blocks = MarkdownStream.parse("b\tp\t\nt\t${MdStyle.IMAGE}\tdiagram.png\talt words\ne\n")
        val run = blocks[0].runs[0]
        assertTrue(run.image)
        assertEquals("diagram.png", run.href)
        assertEquals("alt words", run.text)
    }
}

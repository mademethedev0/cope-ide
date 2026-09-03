// Parser for the markdown instruction stream produced by md_stream.h.
//
// The C++ side walks its Doc tree in preorder and emits one record per line,
// tab-separated:
//
//   b <kind> <arg>            begin a block
//   e                         end the current block
//   t <mask> <href> <text>    an inline run inside the current block
//
// Text payloads are escaped (\\ \n \t) so document content can never break the
// framing. This file rebuilds the tree with an explicit stack — no recursion, so
// a pathologically nested document cannot blow the JVM stack.
//
// md_stream.h and this file must be changed together.
package dev.cope.ide.markdown

/** Inline style bits. Must match the kStyle* constants in md_stream.h. */
public object MdStyle {
    public const val BOLD: Int = 1
    public const val ITALIC: Int = 2
    public const val STRIKE: Int = 4
    public const val CODE: Int = 8
    public const val LINK: Int = 16
    public const val IMAGE: Int = 32
    public const val MATH: Int = 64
}

/** One styled run of text. `href` is empty unless LINK or IMAGE is set. */
public class MdRun(
    public val mask: Int,
    public val href: String,
    public val text: String,
) {
    public val bold: Boolean get() = (mask and MdStyle.BOLD) != 0
    public val italic: Boolean get() = (mask and MdStyle.ITALIC) != 0
    public val strike: Boolean get() = (mask and MdStyle.STRIKE) != 0
    public val code: Boolean get() = (mask and MdStyle.CODE) != 0
    public val link: Boolean get() = (mask and MdStyle.LINK) != 0
    public val image: Boolean get() = (mask and MdStyle.IMAGE) != 0
    public val math: Boolean get() = (mask and MdStyle.MATH) != 0
}

/**
 * A block node. `kind` is the raw tag from the stream, so the renderer switches on
 * strings rather than on a duplicated enum: the set is small and adding one on the
 * C++ side must not require a matching Kotlin enum to compile.
 */
public class MdBlock(
    public val kind: String,
    public val arg: String,
) {
    public val runs: MutableList<MdRun> = ArrayList(2)
    public val children: MutableList<MdBlock> = ArrayList(0)

    /** Concatenated text of every run, for code blocks and plain-text needs. */
    public fun text(): String {
        if (runs.isEmpty()) return ""
        if (runs.size == 1) return runs[0].text
        val builder = StringBuilder()
        for (run in runs) builder.append(run.text)
        return builder.toString()
    }

    /** Heading level, 1..6; 0 for anything that is not a heading. */
    public val level: Int
        get() = if (kind == "h") arg.toIntOrNull()?.coerceIn(1, 6) ?: 1 else 0

    /** Cell alignment: 'l', 'c', 'r' or 'n'. */
    public val align: Char
        get() = arg.firstOrNull() ?: 'n'
}

public object MarkdownStream {

    /** Deeper than this, a document is malformed or malicious; the rest is flattened. */
    private const val MAX_DEPTH = 48

    /**
     * Parses a stream into top-level blocks. Never throws: a truncated or
     * malformed stream yields the blocks that were complete, because a preview
     * that renders most of a file beats one that renders an error.
     */
    public fun parse(bytes: ByteArray): List<MdBlock> =
        parse(String(bytes, Charsets.UTF_8))

    public fun parse(text: String): List<MdBlock> {
        val root = ArrayList<MdBlock>()
        val stack = ArrayList<MdBlock>(8)
        var at = 0
        val length = text.length
        while (at < length) {
            var end = text.indexOf('\n', at)
            if (end < 0) end = length
            if (end > at) {
                handle(text, at, end, root, stack)
            }
            at = end + 1
        }
        return root
    }

    private fun handle(
        text: String,
        from: Int,
        to: Int,
        root: MutableList<MdBlock>,
        stack: MutableList<MdBlock>,
    ) {
        when (text[from]) {
            'b' -> {
                val fields = split(text, from, to, 3)
                val block = MdBlock(fields[1], unescape(fields[2]))
                if (stack.isEmpty()) {
                    root += block
                } else {
                    stack[stack.size - 1].children += block
                }
                // Past MAX_DEPTH the block is still attached (so no content is
                // lost) but not pushed, which flattens the remainder instead of
                // recursing without bound.
                if (stack.size < MAX_DEPTH) stack += block
            }
            'e' -> {
                if (stack.isNotEmpty()) stack.removeAt(stack.size - 1)
            }
            't' -> {
                val fields = split(text, from, to, 4)
                val run = MdRun(
                    mask = fields[1].toIntOrNull() ?: 0,
                    href = unescape(fields[2]),
                    text = unescape(fields[3]),
                )
                if (stack.isNotEmpty()) {
                    stack[stack.size - 1].runs += run
                } else {
                    // A run with no open block cannot happen from md_stream.h, but
                    // a truncated stream can start mid-record. Wrap it rather than
                    // dropping the text.
                    val paragraph = MdBlock("p", "")
                    paragraph.runs += run
                    root += paragraph
                }
            }
            else -> Unit // unknown record type: ignore, do not abort the document
        }
    }

    /**
     * Splits `count` tab-separated fields out of text[from, to). The last field
     * keeps any remaining tabs, which cannot occur (payloads are escaped) but is
     * the safe interpretation. Missing fields come back empty.
     */
    private fun split(text: String, from: Int, to: Int, count: Int): Array<String> {
        val fields = Array(count) { "" }
        var index = 0
        var start = from
        var at = from
        while (at < to && index < count - 1) {
            if (text[at] == '\t') {
                fields[index] = text.substring(start, at)
                index++
                start = at + 1
            }
            at++
        }
        fields[index] = text.substring(start.coerceAtMost(to), to)
        return fields
    }

    /** Reverses md_stream.h's escapeInto. A trailing lone backslash is literal. */
    public fun unescape(text: String): String {
        if (text.indexOf('\\') < 0) return text
        val out = StringBuilder(text.length)
        var at = 0
        while (at < text.length) {
            val c = text[at]
            if (c != '\\' || at + 1 >= text.length) {
                out.append(c)
                at++
                continue
            }
            when (text[at + 1]) {
                '\\' -> out.append('\\')
                'n' -> out.append('\n')
                't' -> out.append('\t')
                else -> {
                    // Not an escape this format defines: keep both bytes so the
                    // text round-trips instead of silently losing a backslash.
                    out.append(c)
                    out.append(text[at + 1])
                }
            }
            at += 2
        }
        return out.toString()
    }

    /** The encoder, used only by tests. Mirrors escapeInto exactly. */
    public fun escape(text: String): String {
        val out = StringBuilder(text.length)
        for (c in text) {
            when (c) {
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\t' -> out.append("\\t")
                '\r' -> Unit
                else -> out.append(c)
            }
        }
        return out.toString()
    }
}

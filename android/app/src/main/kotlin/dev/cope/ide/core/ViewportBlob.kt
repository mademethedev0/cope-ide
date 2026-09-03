// Decoder for the viewport blob produced by Session::viewport in cope_jni.cpp.
//
// The blob is the whole point of the JNI design: one direct ByteBuffer per fetch
// instead of one call per token. Layout (little-endian int32 unless stated):
//
//   header  8 ints   magic | version | firstLine | lineCount | spanCount |
//                    textBytes | tier | flags
//   lines   lineCount x 5   byteOffset | textOffset | textLength | spanOffset | spanCount
//   spans   spanCount x 3   begin | end | styleId     (byte offsets in the line)
//   text    textBytes       concatenated line content, UTF-8, no terminators
//
// byteOffset is the line's absolute offset in the document, carried here so the
// renderer never makes a JNI call per visible line to place a caret or paint a
// selection rectangle.
//
// LIFETIME: the buffer is owned and reused by the native session, so decode()
// copies everything it keeps. That copy is the price of not having to reason
// about a dangling direct buffer on a background thread, and it is one arraycopy
// per screen.
package dev.cope.ide.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Tier reported by the highlight cascade. Values match ide::highlight::Tier. */
public enum class Tier(public val code: Int, public val label: String) {
    GRAMMAR(1, "grammar"),
    FALLBACK(2, "fallback"),
    PLAIN(3, "plain"),
    ;

    public companion object {
        public fun of(code: Int): Tier = when (code) {
            1 -> GRAMMAR
            2 -> FALLBACK
            else -> PLAIN
        }
    }
}

/**
 * One decoded screenful. Flat arrays on purpose: a data class per line would
 * allocate hundreds of objects per scroll frame.
 *
 * For line i (0-based within this viewport):
 *   absolute offset = lineByteOffset[i]
 *   text bytes  = text[lineTextOffset[i] until lineTextOffset[i] + lineTextLength[i]]
 *   spans       = spanOffset[i] until spanOffset[i] + spanCount[i]  (indices into spanBegin/End/Style)
 */
public class Viewport(
    public val firstLine: Int,
    public val lineCount: Int,
    public val tier: Tier,
    public val hasGrammar: Boolean,
    public val dirty: Boolean,
    public val inMemory: Boolean,
    public val text: ByteArray,
    public val lineByteOffset: IntArray,
    public val lineTextOffset: IntArray,
    public val lineTextLength: IntArray,
    public val lineSpanOffset: IntArray,
    public val lineSpanCount: IntArray,
    public val spanBegin: IntArray,
    public val spanEnd: IntArray,
    public val spanStyle: IntArray,
) {
    public fun containsLine(line: Int): Boolean =
        line >= firstLine && line < firstLine + lineCount

    public companion object {
        public val EMPTY: Viewport = Viewport(
            firstLine = 0,
            lineCount = 0,
            tier = Tier.PLAIN,
            hasGrammar = false,
            dirty = false,
            inMemory = false,
            text = ByteArray(0),
            lineByteOffset = IntArray(0),
            lineTextOffset = IntArray(0),
            lineTextLength = IntArray(0),
            lineSpanOffset = IntArray(0),
            lineSpanCount = IntArray(0),
            spanBegin = IntArray(0),
            spanEnd = IntArray(0),
            spanStyle = IntArray(0),
        )
    }
}

public object ViewportBlob {

    private const val MAGIC = 0x45504F43 // 'COPE' little-endian
    private const val VERSION = 1
    private const val HEADER_INTS = 8

    /**
     * Decodes a blob. Returns null for anything that does not look like one —
     * a version bump, a truncated buffer, a null. Never throws: a malformed blob
     * must degrade to "no highlighting this frame", not crash the editor.
     */
    public fun decode(buffer: ByteBuffer?): Viewport? {
        if (buffer == null) return null
        val view = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        if (view.remaining() < HEADER_INTS * 4) return null
        val ints = view.asIntBuffer()

        if (ints.get(0) != MAGIC || ints.get(1) != VERSION) return null
        val firstLine = ints.get(2)
        val lineCount = ints.get(3)
        val spanCount = ints.get(4)
        val textBytes = ints.get(5)
        val tier = Tier.of(ints.get(6))
        val flags = ints.get(7)
        if (firstLine < 0 || lineCount < 0 || spanCount < 0 || textBytes < 0) return null

        val intsNeeded = HEADER_INTS + lineCount * 5 + spanCount * 3
        if (ints.limit() < intsNeeded) return null
        if (view.remaining() < intsNeeded * 4 + textBytes) return null

        val lineByteOffset = IntArray(lineCount)
        val lineTextOffset = IntArray(lineCount)
        val lineTextLength = IntArray(lineCount)
        val lineSpanOffset = IntArray(lineCount)
        val lineSpanCount = IntArray(lineCount)
        var at = HEADER_INTS
        for (i in 0 until lineCount) {
            lineByteOffset[i] = ints.get(at)
            lineTextOffset[i] = ints.get(at + 1)
            lineTextLength[i] = ints.get(at + 2)
            lineSpanOffset[i] = ints.get(at + 3)
            lineSpanCount[i] = ints.get(at + 4)
            at += 5
            // Guard every index the renderer will trust, once, here.
            if (lineTextOffset[i] < 0 || lineTextLength[i] < 0 ||
                lineByteOffset[i] < 0 ||
                lineTextOffset[i] + lineTextLength[i] > textBytes ||
                lineSpanOffset[i] < 0 || lineSpanCount[i] < 0 ||
                lineSpanOffset[i] + lineSpanCount[i] > spanCount
            ) {
                return null
            }
        }

        val spanBegin = IntArray(spanCount)
        val spanEnd = IntArray(spanCount)
        val spanStyle = IntArray(spanCount)
        for (i in 0 until spanCount) {
            spanBegin[i] = ints.get(at)
            spanEnd[i] = ints.get(at + 1)
            spanStyle[i] = ints.get(at + 2)
            at += 3
        }

        val text = ByteArray(textBytes)
        if (textBytes > 0) {
            val bytes = view.duplicate()
            bytes.position(intsNeeded * 4)
            bytes.get(text, 0, textBytes)
        }

        return Viewport(
            firstLine = firstLine,
            lineCount = lineCount,
            tier = tier,
            hasGrammar = (flags and 1) != 0,
            dirty = (flags and 2) != 0,
            inMemory = (flags and 4) != 0,
            text = text,
            lineByteOffset = lineByteOffset,
            lineTextOffset = lineTextOffset,
            lineTextLength = lineTextLength,
            lineSpanOffset = lineSpanOffset,
            lineSpanCount = lineSpanCount,
            spanBegin = spanBegin,
            spanEnd = spanEnd,
            spanStyle = spanStyle,
        )
    }
}

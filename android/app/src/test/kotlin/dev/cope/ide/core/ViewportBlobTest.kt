// Round-trips the viewport blob: a hand-built buffer in, decoded arrays out.
//
// This test is the contract between cope_jni.cpp's writeViewportBlob and
// ViewportBlob.decode. If the two ever disagree the editor draws garbage, and
// this is the only place that disagreement can be caught without a device.
package dev.cope.ide.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ViewportBlobTest {

    private class Line(
        val byteOffset: Int,
        val text: String,
        val spans: List<Triple<Int, Int, Int>>,
    )

    /** Builds a blob exactly the way the native side does. */
    private fun build(
        firstLine: Int,
        lines: List<Line>,
        tier: Int = 1,
        flags: Int = 1,
        magic: Int = 0x45504F43,
        version: Int = 1,
    ): ByteBuffer {
        val textBytes = lines.sumOf { it.text.toByteArray(Charsets.UTF_8).size }
        val spanCount = lines.sumOf { it.spans.size }
        val ints = 8 + lines.size * 5 + spanCount * 3
        val buffer = ByteBuffer.allocateDirect(ints * 4 + textBytes).order(ByteOrder.LITTLE_ENDIAN)

        buffer.putInt(magic)
        buffer.putInt(version)
        buffer.putInt(firstLine)
        buffer.putInt(lines.size)
        buffer.putInt(spanCount)
        buffer.putInt(textBytes)
        buffer.putInt(tier)
        buffer.putInt(flags)

        var textAt = 0
        var spanAt = 0
        for (line in lines) {
            val encoded = line.text.toByteArray(Charsets.UTF_8)
            buffer.putInt(line.byteOffset)
            buffer.putInt(textAt)
            buffer.putInt(encoded.size)
            buffer.putInt(spanAt)
            buffer.putInt(line.spans.size)
            textAt += encoded.size
            spanAt += line.spans.size
        }
        for (line in lines) {
            for ((begin, end, style) in line.spans) {
                buffer.putInt(begin)
                buffer.putInt(end)
                buffer.putInt(style)
            }
        }
        for (line in lines) {
            buffer.put(line.text.toByteArray(Charsets.UTF_8))
        }
        buffer.flip()
        return buffer
    }

    @Test
    fun `decodes a two-line blob`() {
        val blob = build(
            firstLine = 40,
            lines = listOf(
                Line(1000, "int x = 1;", listOf(Triple(0, 3, 5), Triple(3, 10, 0))),
                Line(1011, "  return x;", listOf(Triple(0, 11, 7))),
            ),
        )
        val viewport = ViewportBlob.decode(blob)
        requireNotNull(viewport)

        assertEquals(40, viewport.firstLine)
        assertEquals(2, viewport.lineCount)
        assertEquals(Tier.GRAMMAR, viewport.tier)
        assertTrue(viewport.hasGrammar)
        assertFalse(viewport.dirty)

        assertEquals(1000, viewport.lineByteOffset[0])
        assertEquals(1011, viewport.lineByteOffset[1])
        assertEquals(0, viewport.lineTextOffset[0])
        assertEquals(10, viewport.lineTextLength[0])
        assertEquals(10, viewport.lineTextOffset[1])
        assertEquals(11, viewport.lineTextLength[1])

        assertEquals(
            "int x = 1;",
            String(viewport.text, 0, viewport.lineTextLength[0], Charsets.UTF_8),
        )
        assertEquals(
            "  return x;",
            String(
                viewport.text,
                viewport.lineTextOffset[1],
                viewport.lineTextLength[1],
                Charsets.UTF_8,
            ),
        )

        assertEquals(2, viewport.lineSpanCount[0])
        assertEquals(1, viewport.lineSpanCount[1])
        assertEquals(2, viewport.lineSpanOffset[1])
        assertEquals(5, viewport.spanStyle[0])
        assertEquals(7, viewport.spanStyle[2])
    }

    @Test
    fun `containsLine covers exactly the fetched range`() {
        val blob = build(10, listOf(Line(0, "a", emptyList()), Line(2, "b", emptyList())))
        val viewport = requireNotNull(ViewportBlob.decode(blob))
        assertTrue(viewport.containsLine(10))
        assertTrue(viewport.containsLine(11))
        assertFalse(viewport.containsLine(9))
        assertFalse(viewport.containsLine(12))
    }

    @Test
    fun `rejects a wrong magic`() {
        assertNull(ViewportBlob.decode(build(0, listOf(Line(0, "x", emptyList())), magic = 12345)))
    }

    @Test
    fun `rejects a future version`() {
        assertNull(ViewportBlob.decode(build(0, listOf(Line(0, "x", emptyList())), version = 2)))
    }

    @Test
    fun `rejects a truncated buffer`() {
        val full = build(0, listOf(Line(0, "hello", listOf(Triple(0, 5, 1)))))
        val truncated = ByteBuffer.allocateDirect(full.remaining() - 4)
            .order(ByteOrder.LITTLE_ENDIAN)
        val bytes = ByteArray(full.remaining() - 4)
        full.get(bytes)
        truncated.put(bytes)
        truncated.flip()
        assertNull(ViewportBlob.decode(truncated))
    }

    @Test
    fun `rejects a header shorter than eight ints`() {
        val tiny = ByteBuffer.allocateDirect(12).order(ByteOrder.LITTLE_ENDIAN)
        tiny.putInt(0x45504F43)
        tiny.putInt(1)
        tiny.putInt(0)
        tiny.flip()
        assertNull(ViewportBlob.decode(tiny))
    }

    @Test
    fun `rejects a null buffer`() {
        assertNull(ViewportBlob.decode(null))
    }

    @Test
    fun `rejects a line whose text runs past the text block`() {
        // Built by hand: textLength claims 99 bytes in a 1-byte text block. The
        // renderer trusts these indices, so decode must reject rather than let an
        // out-of-bounds read happen in the draw path.
        val buffer = ByteBuffer.allocateDirect(8 * 4 + 5 * 4 + 1).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(0x45504F43)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(1)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(0)
        buffer.putInt(0)
        buffer.putInt(99)
        buffer.putInt(0)
        buffer.putInt(0)
        buffer.put('x'.code.toByte())
        buffer.flip()
        assertNull(ViewportBlob.decode(buffer))
    }

    @Test
    fun `rejects a line whose spans run past the span block`() {
        val buffer = ByteBuffer.allocateDirect(8 * 4 + 5 * 4 + 1).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(0x45504F43)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(1)
        buffer.putInt(0) // spanCount = 0 in the header
        buffer.putInt(1)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(0)
        buffer.putInt(0)
        buffer.putInt(1)
        buffer.putInt(0)
        buffer.putInt(4) // but this line claims 4 spans
        buffer.put('x'.code.toByte())
        buffer.flip()
        assertNull(ViewportBlob.decode(buffer))
    }

    @Test
    fun `zero lines is valid and decodes to an empty viewport`() {
        val viewport = requireNotNull(ViewportBlob.decode(build(0, emptyList())))
        assertEquals(0, viewport.lineCount)
        assertEquals(0, viewport.text.size)
    }

    @Test
    fun `flags decode independently`() {
        val viewport = requireNotNull(
            ViewportBlob.decode(build(0, listOf(Line(0, "a", emptyList())), tier = 2, flags = 6)),
        )
        assertEquals(Tier.FALLBACK, viewport.tier)
        assertFalse(viewport.hasGrammar)
        assertTrue(viewport.dirty)
        assertTrue(viewport.inMemory)
    }

    @Test
    fun `tier codes map to the highlight cascade`() {
        assertEquals(Tier.GRAMMAR, Tier.of(1))
        assertEquals(Tier.FALLBACK, Tier.of(2))
        assertEquals(Tier.PLAIN, Tier.of(3))
        // Anything unexpected is plain: the safe interpretation, never a crash.
        assertEquals(Tier.PLAIN, Tier.of(0))
        assertEquals(Tier.PLAIN, Tier.of(77))
    }
}

// The palette's ranking and the preview's link resolution. Both are pure, so both
// are testable without a device.
package dev.cope.ide.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FuzzyTest {

    @Test
    fun `an exact prefix matches`() {
        assertTrue(fuzzyScore("Save", "sav") > 0)
    }

    @Test
    fun `an initialism matches across words`() {
        assertTrue(fuzzyScore("Go to line", "gtl") > 0)
    }

    @Test
    fun `a non-subsequence does not match`() {
        assertEquals(0, fuzzyScore("Save", "svae"))
        assertEquals(0, fuzzyScore("Save", "zz"))
        assertEquals(0, fuzzyScore("", "a"))
    }

    @Test
    fun `an empty query matches everything with a low score`() {
        assertEquals(1, fuzzyScore("anything", ""))
    }

    @Test
    fun `matching is case insensitive both ways`() {
        assertTrue(fuzzyScore("Save As", "SAVE") > 0)
        assertTrue(fuzzyScore("SAVE AS", "save") > 0)
    }

    @Test
    fun `word boundary hits outrank scattered hits`() {
        val initialism = fuzzyScore("Go to line", "gtl")
        val scattered = fuzzyScore("Toggle indent guides", "gtl")
        assertTrue("$initialism should beat $scattered", initialism > scattered)
    }

    @Test
    fun `a contiguous run outranks a gappy one`() {
        val contiguous = fuzzyScore("theme picker", "theme")
        val gappy = fuzzyScore("the memory example", "theme")
        assertTrue("$contiguous should beat $gappy", contiguous > gappy)
    }

    @Test
    fun `the shorter candidate wins a tie`() {
        assertTrue(fuzzyScore("Save", "save") > fuzzyScore("Save as…", "save"))
    }

    @Test
    fun `spaces in the query are ignored`() {
        assertEquals(fuzzyScore("Go to line", "gtl"), fuzzyScore("Go to line", "g t l"))
    }

    @Test
    fun `a full match of the whole candidate scores highest of its length`() {
        val whole = fuzzyScore("undo", "undo")
        val partial = fuzzyScore("undo", "un")
        assertTrue(whole > partial)
    }

    @Test
    fun `filename separators are word boundaries`() {
        // "pt" should find piece_table.cpp via the underscore boundary.
        val boundary = fuzzyScore("piece_table.cpp", "pt")
        val inside = fuzzyScore("xpxtx", "pt")
        assertTrue("$boundary should beat $inside", boundary > inside)
    }

    // --- path normalisation -------------------------------------------------

    @Test
    fun `normalisePath resolves dot and dotdot`() {
        assertEquals("/a/b/c", normalisePath("/a/b/c"))
        assertEquals("/a/c", normalisePath("/a/b/../c"))
        assertEquals("/a/b", normalisePath("/a/./b"))
        assertEquals("/a/b", normalisePath("/a//b"))
        assertEquals("/docs/img.png", normalisePath("/docs/ui/../img.png"))
    }

    @Test
    fun `normalisePath cannot escape the root`() {
        assertEquals("/etc", normalisePath("/../../../etc"))
        assertEquals("/", normalisePath("/.."))
        assertEquals("/", normalisePath(""))
        assertEquals("/", normalisePath("/"))
    }

    @Test
    fun `normalisePath drops a trailing slash`() {
        assertEquals("/a/b", normalisePath("/a/b/"))
    }

    @Test
    fun `shortenTail keeps the last segments`() {
        assertEquals("…/text/src", shortenTail("/home/user/text/src"))
        assertEquals("/a/b", shortenTail("/a/b"))
        assertEquals("a", shortenTail("a"))
        assertEquals("…/c/d/e", shortenTail("/a/b/c/d/e", keep = 3))
    }
}

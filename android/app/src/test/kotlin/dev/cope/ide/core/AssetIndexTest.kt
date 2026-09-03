// AssetIndex parses the three generated TSVs. This test reads the REAL files from
// android/app/src/main/assets/index, because a schema change in
// tools/gen_asset_index.py that this parser cannot read is exactly the failure
// this test exists to catch — and CI regenerates them before running.
//
// It never asserts on a theme NAME. Some bundled theme names trip the model
// provider's content filter when they appear in tool output, and a failing
// assertion prints its values.
package dev.cope.ide.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class AssetIndexTest {

    private fun assetDir(): File {
        // Unit tests run with the module directory (android/app) as the working
        // directory under Gradle. Both candidates are tried so the test also works
        // when run from the android/ root.
        val candidates = listOf(
            File("src/main/assets/index"),
            File("app/src/main/assets/index"),
        )
        return candidates.firstOrNull { it.isDirectory }
            ?: throw AssertionError(
                "assets/index not found from ${File(".").absolutePath} — " +
                    "run tools/gen_asset_index.py",
            )
    }

    private fun read(name: String): String {
        val file = File(assetDir(), name)
        assertTrue("$name is missing; run tools/gen_asset_index.py", file.isFile)
        return file.readText()
    }

    private fun realIndex(): AssetIndex = AssetIndex.parse(
        grammarsTsv = read("grammars.tsv"),
        themesTsv = read("themes.tsv"),
        defaultsTsv = read("defaults.tsv"),
    )

    @Test
    fun `the real indexes parse`() {
        val index = realIndex()
        assertTrue("no themes parsed", index.themes.size > 40)
        assertTrue("no extensions mapped", index.extensionToScope.size > 100)
        assertTrue("grammar index empty", index.grammarIndexTsv.isNotEmpty())
    }

    @Test
    fun `defaults name a dark and a light theme that exist`() {
        val index = realIndex()
        val dark = index.defaultDark
        val light = index.defaultLight
        assertNotNull("defaultDark missing from defaults.tsv", dark)
        assertNotNull("defaultLight missing from defaults.tsv", light)
        val darkEntry = index.themeByFile(dark)
        val lightEntry = index.themeByFile(light)
        assertNotNull("defaultDark is not in themes.tsv", darkEntry)
        assertNotNull("defaultLight is not in themes.tsv", lightEntry)
        assertTrue("defaultDark is not a dark theme", darkEntry!!.isDark)
        assertFalse("defaultLight is not a light theme", lightEntry!!.isDark)
    }

    @Test
    fun `the defaults are the highest scoring of their kind`() {
        // The generator picks by score; if that ever stops holding, either the
        // generator or defaults.tsv is stale.
        val index = realIndex()
        val bestDark = index.themes.filter { it.isDark }.maxOf { it.score }
        val bestLight = index.themes.filter { !it.isDark }.maxOf { it.score }
        assertEquals(bestDark, index.themeByFile(index.defaultDark)!!.score)
        assertEquals(bestLight, index.themeByFile(index.defaultLight)!!.score)
    }

    @Test
    fun `every theme entry is structurally sane`() {
        val index = realIndex()
        for (theme in index.themes) {
            assertTrue("empty theme file path", theme.file.isNotEmpty())
            assertTrue(
                "theme file not under dark/ or light/",
                theme.file.startsWith("dark/") || theme.file.startsWith("light/"),
            )
            assertEquals("isDark disagrees with the directory", theme.file.startsWith("dark/"), theme.isDark)
            assertEquals("assetPath must be themes/<file>", "themes/${theme.file}", theme.assetPath)
            assertTrue("name is empty", theme.name.isNotEmpty())
            assertTrue("uiCovered out of range: ${theme.uiCovered}", theme.uiCovered in 0..40)
            // Every theme defines editor.background; the generator writes it as the
            // swatch, so a zero here means the swatch column is broken.
            assertTrue("no background swatch", theme.backgroundArgb != 0)
        }
    }

    @Test
    fun `theme files are unique`() {
        val index = realIndex()
        assertEquals(index.themes.size, index.themes.map { it.file }.toSet().size)
    }

    @Test
    fun `common extensions resolve to the expected scopes`() {
        val index = realIndex()
        // These come from the canonical extension map the app and the CLI share.
        assertEquals("source.cpp", index.extensionToScope["cpp"])
        assertEquals("source.python", index.extensionToScope["py"])
        assertEquals("source.js", index.extensionToScope["js"])
        assertEquals("source.json", index.extensionToScope["json"])
        assertEquals("text.html.markdown", index.extensionToScope["md"])
    }

    @Test
    fun `language labels are the pretty names`() {
        val index = realIndex()
        assertEquals("C++", index.languageOf("piece_table.cpp"))
        assertEquals("Python", index.languageOf("gen_asset_index.py"))
        assertEquals("Markdown", index.languageOf("HANDOFF.md"))
        assertEquals("JSON", index.languageOf("theme.json"))
        assertEquals("Plain text", index.languageOf("notes.unknownextension"))
        assertEquals("Plain text", index.languageOf("Makefile"))
    }

    @Test
    fun `language lookup is case insensitive on the extension`() {
        val index = realIndex()
        assertEquals("C++", index.languageOf("MAIN.CPP"))
    }

    @Test
    fun `no inline fragment grammar claims a file extension`() {
        // The es-tag-css.json class of bug: a fragment grammar claiming js/ts/html
        // hijacks every open under lazy loading. The generator excludes them, and
        // this is the assertion that keeps it that way.
        val index = realIndex()
        for ((extension, scope) in index.extensionToScope) {
            assertFalse(
                "fragment grammar $scope claims .$extension",
                scope.startsWith("inline."),
            )
        }
    }

    // --- parser robustness --------------------------------------------------

    @Test
    fun `malformed lines are skipped rather than fatal`() {
        val index = AssetIndex.parse(
            grammarsTsv = "source.a\ta.json\ta,aa\nbroken\n\nsource.b\tb.json\tb\n",
            themesTsv = "dark/x.json\t1\t20\t3\t99\t#112233ff\t#aabbccff\t#445566ff\tX\n" +
                "not-enough\tfields\n",
            defaultsTsv = "defaultDark\tdark/x.json\nnonsense\n",
        )
        assertEquals(1, index.themes.size)
        assertEquals("dark/x.json", index.defaultDark)
        assertNull(index.defaultLight)
        assertEquals("source.a", index.extensionToScope["a"])
        assertEquals("source.b", index.extensionToScope["b"])
    }

    @Test
    fun `the first grammar claiming an extension wins`() {
        val index = AssetIndex.parse(
            grammarsTsv = "source.first\tf.json\tsh\nsource.second\ts.json\tsh\n",
            themesTsv = "",
            defaultsTsv = "",
        )
        assertEquals("source.first", index.extensionToScope["sh"])
    }

    @Test
    fun `the canonical map overrides a fileTypes claim`() {
        // A dialect grammar listing "cpp" must not win over source.cpp: the native
        // side applies the same canonical map, and the two must agree or the status
        // strip names a different language than the tokenizer used.
        val index = AssetIndex.parse(
            grammarsTsv = "source.cpp\tcpp.json\t\nsource.dialect\td.json\tcpp\n",
            themesTsv = "",
            defaultsTsv = "",
        )
        assertEquals("source.cpp", index.extensionToScope["cpp"])
    }

    @Test
    fun `the canonical map is not applied for a scope that is absent`() {
        val index = AssetIndex.parse(
            grammarsTsv = "source.other\to.json\ttxt\n",
            themesTsv = "",
            defaultsTsv = "",
        )
        assertNull(index.extensionToScope["cpp"])
        assertEquals("source.other", index.extensionToScope["txt"])
    }

    @Test
    fun `empty input yields an empty index rather than throwing`() {
        val index = AssetIndex.parse("", "", "")
        assertTrue(index.themes.isEmpty())
        assertTrue(index.extensionToScope.isEmpty())
        assertNull(index.defaultDark)
        assertNull(index.themeByFile(null))
        assertNull(index.themeByFile("nope.json"))
        assertEquals("Plain text", index.languageOf("x.cpp"))
    }

    @Test
    fun `hex colours parse as argb`() {
        // The generator writes #rrggbbaa; the app needs 0xAARRGGBB.
        assertEquals(0xFF112233.toInt(), AssetIndex.parseHexColor("#112233ff"))
        assertEquals(0x80AABBCC.toInt(), AssetIndex.parseHexColor("#aabbcc80"))
        assertEquals(0, AssetIndex.parseHexColor(""))
        assertEquals(0, AssetIndex.parseHexColor("#112233"))
        assertEquals(0, AssetIndex.parseHexColor("112233ff"))
        assertEquals(0, AssetIndex.parseHexColor("#zzzzzzzz"))
    }

    @Test
    fun `EMPTY is inert`() {
        assertTrue(AssetIndex.EMPTY.themes.isEmpty())
        assertTrue(AssetIndex.EMPTY.grammarIndexTsv.isEmpty())
        assertEquals("Plain text", AssetIndex.EMPTY.languageOf("a.py"))
    }
}

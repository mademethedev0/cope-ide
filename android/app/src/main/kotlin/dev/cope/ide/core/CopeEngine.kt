// The safe Kotlin facade over the native engine and one open document.
//
// Everything above this file works with Kotlin types and never sees a handle.
// Two rules make that safe:
//   * a closed object is inert, not a crash: every method checks its handle and
//     returns a documented empty value;
//   * CopeEngine.close() closes its documents first, because a Session holds a
//     reference to its Engine on the native side.
package dev.cope.ide.core

import android.content.res.AssetManager
import java.nio.charset.StandardCharsets

/** Chrome colours a theme was asked for, in the order the native side wants them. */
public class ThemeSnapshot(
    public val name: String,
    public val isDark: Boolean,
    /** 3 ints per style: fgArgb, bgArgb, flags (fontStyle | hasFg<<8 | hasBg<<9). */
    public val palette: IntArray,
    /** ARGB per requested key, 0 when the theme does not define it. */
    public val uiColors: IntArray,
) {
    public val styleCount: Int get() = palette.size / 3

    public fun foreground(styleId: Int): Int = palette[styleId * 3]
    public fun background(styleId: Int): Int = palette[styleId * 3 + 1]
    public fun flags(styleId: Int): Int = palette[styleId * 3 + 2]

    public companion object {
        public const val FLAG_BOLD: Int = 1
        public const val FLAG_ITALIC: Int = 2
        public const val FLAG_UNDERLINE: Int = 4
        public const val FLAG_STRIKE: Int = 8
        public const val FLAG_HAS_FG: Int = 0x100
        public const val FLAG_HAS_BG: Int = 0x200
    }
}

/** What the inspector reports. All of it measured, none of it guessed. */
public data class Inspection(
    public val tier: String,
    public val grammarScope: String,
    public val refusedPatterns: Int,
    public val unusableRules: Int,
    /** 0..1 */
    public val coverage: Float,
    /** 0..1 */
    public val repairRatio: Float,
    public val report: String,
) {
    public companion object {
        public val NONE: Inspection = Inspection("", "", 0, 0, 0f, 0f, "")
    }
}

public class CopeEngine private constructor(private var handle: Long) {

    public val valid: Boolean get() = handle != 0L

    private val documents = ArrayList<CopeDocument>()

    /** Loads a theme from the asset library and applies it to every open document. */
    public fun applyTheme(entry: ThemeEntry, uiKeys: List<String>): ThemeSnapshot? {
        if (handle == 0L) return null
        val json = CopeNative.readAsset(handle, entry.assetPath) ?: return null
        if (!CopeNative.setTheme(handle, String(json, StandardCharsets.UTF_8))) return null
        return snapshot(uiKeys)
    }

    public fun snapshot(uiKeys: List<String>): ThemeSnapshot? {
        if (handle == 0L) return null
        val palette = CopeNative.palette(handle) ?: return null
        val colors = CopeNative.uiColors(handle, uiKeys.joinToString("\n")) ?: IntArray(uiKeys.size)
        val info = (CopeNative.themeInfo(handle) ?: "").split('\t')
        return ThemeSnapshot(
            name = info.getOrElse(0) { "" },
            isDark = info.getOrElse(1) { "1" } == "1",
            palette = palette,
            uiColors = if (colors.size >= uiKeys.size) colors else IntArray(uiKeys.size),
        )
    }

    public fun readAsset(path: String): ByteArray? =
        if (handle == 0L) null else CopeNative.readAsset(handle, path)

    /** Directory entries, already sorted with directories first. */
    public fun listDir(path: String): List<DirEntry> {
        if (handle == 0L) return emptyList()
        val tsv = CopeNative.listDir(handle, path) ?: return emptyList()
        val entries = ArrayList<DirEntry>()
        for (line in tsv.lineSequence()) {
            if (line.isEmpty()) continue
            val parts = line.split('\t')
            if (parts.size < 3) continue
            entries += DirEntry(
                name = parts[0],
                isDirectory = parts[1] == "1",
                size = parts[2].toLongOrNull() ?: 0L,
            )
        }
        entries.sortWith(compareBy({ !it.isDirectory }, { it.name.lowercase() }))
        return entries
    }

    public fun stat(path: String): DirEntry? {
        if (handle == 0L) return null
        val parts = (CopeNative.statPath(handle, path) ?: return null).split('\t')
        if (parts.size < 3 || parts[0] != "1") return null
        val name = path.substringAfterLast('/')
        return DirEntry(name, parts[1] == "1", parts[2].toLongOrNull() ?: 0L)
    }

    public fun scopeForFile(name: String): String =
        if (handle == 0L) "" else CopeNative.scopeForFile(handle, name) ?: ""

    /** Opens a real filesystem path, zero-copy (mmap). Null when unreadable. */
    public fun openPath(path: String): CopeDocument? {
        if (handle == 0L) return null
        val session = CopeNative.openPath(handle, path)
        if (session == 0L) return null
        return CopeDocument(this, handle, session, path.substringAfterLast('/'), path)
            .also { documents += it }
    }

    /** Opens from bytes: the SAF path, and new unsaved buffers. */
    public fun openBytes(name: String, bytes: ByteArray): CopeDocument? {
        if (handle == 0L) return null
        val session = CopeNative.openBytes(handle, name, bytes)
        if (session == 0L) return null
        return CopeDocument(this, handle, session, name, null).also { documents += it }
    }

    internal fun forget(document: CopeDocument) {
        documents.remove(document)
    }

    public fun close() {
        // Sessions must die before the engine they point at.
        for (document in ArrayList(documents)) document.close()
        documents.clear()
        if (handle != 0L) {
            CopeNative.destroyEngine(handle)
            handle = 0L
        }
    }

    public companion object {
        public fun create(assets: AssetManager, index: AssetIndex): CopeEngine? {
            if (!CopeNative.available) return null
            val handle = CopeNative.createEngine(assets, index.grammarIndexTsv)
            if (handle == 0L) return null
            return CopeEngine(handle)
        }
    }
}

public data class DirEntry(
    public val name: String,
    public val isDirectory: Boolean,
    public val size: Long,
)

/** One open document. Not thread confined: the native side takes a lock. */
public class CopeDocument internal constructor(
    private val engine: CopeEngine,
    private val engineHandle: Long,
    private var session: Long,
    public val name: String,
    /** Real filesystem path, or null for a SAF/in-memory document. */
    public var path: String?,
) {
    public val valid: Boolean get() = session != 0L

    public var lineCount: Int = 1
        private set
    public var byteSize: Long = 0
        private set
    public var version: Long = 0
        private set
    public var dirty: Boolean = false
        private set
    public var tier: Tier = Tier.PLAIN
        private set
    public var hasGrammar: Boolean = false
        private set
    public var inMemory: Boolean = false
        private set
    public var canUndo: Boolean = false
        private set
    public var canRedo: Boolean = false
        private set

    init {
        refresh()
    }

    public fun refresh() {
        if (session == 0L) return
        val info = CopeNative.sessionInfo(engineHandle, session) ?: return
        if (info.size < 9) return
        lineCount = info[0].toInt().coerceAtLeast(1)
        byteSize = info[1]
        version = info[2]
        dirty = info[3] != 0L
        tier = Tier.of(info[4].toInt())
        hasGrammar = info[5] != 0L
        inMemory = info[6] != 0L
        canUndo = info[7] != 0L
        canRedo = info[8] != 0L
    }

    public fun viewport(firstLine: Int, count: Int): Viewport? {
        if (session == 0L || count <= 0) return null
        return ViewportBlob.decode(CopeNative.viewport(engineHandle, session, firstLine, count))
    }

    public fun lineBytes(line: Int): ByteArray =
        if (session == 0L) ByteArray(0)
        else CopeNative.lineBytes(engineHandle, session, line) ?: ByteArray(0)

    public fun textRange(offset: Long, length: Long): ByteArray =
        if (session == 0L || length <= 0) ByteArray(0)
        else CopeNative.textRange(engineHandle, session, offset, length) ?: ByteArray(0)

    public fun textRangeString(offset: Long, length: Long): String =
        String(textRange(offset, length), StandardCharsets.UTF_8)

    /** Returns the caret offset after the insert, or -1 on failure. */
    public fun insert(offset: Long, text: String): Long {
        if (session == 0L || text.isEmpty()) return -1
        val result = CopeNative.insert(engineHandle, session, offset, text)
        refresh()
        return result
    }

    public fun erase(offset: Long, length: Long): Long {
        if (session == 0L || length <= 0) return -1
        val result = CopeNative.erase(engineHandle, session, offset, length)
        refresh()
        return result
    }

    public fun replace(offset: Long, length: Long, text: String): Long {
        if (session == 0L) return -1
        val result = CopeNative.replaceRange(engineHandle, session, offset, length, text)
        refresh()
        return result
    }

    public fun undo(): Long {
        if (session == 0L) return -1
        val result = CopeNative.undo(engineHandle, session)
        refresh()
        return result
    }

    public fun redo(): Long {
        if (session == 0L) return -1
        val result = CopeNative.redo(engineHandle, session)
        refresh()
        return result
    }

    /** Saves to `path` (or the document's own path). False on any failure. */
    public fun save(target: String? = null): Boolean {
        if (session == 0L) return false
        val ok = CopeNative.save(engineHandle, session, target)
        if (ok && target != null) path = target
        refresh()
        return ok
    }

    /** Whole document, for the SAF write path. */
    public fun bytes(): ByteArray =
        if (session == 0L) ByteArray(0)
        else CopeNative.documentBytes(engineHandle, session) ?: ByteArray(0)

    public fun markSaved() {
        if (session == 0L) return
        CopeNative.markSaved(engineHandle, session)
        refresh()
    }

    public fun offsetOf(line: Int, byteColumn: Int): Long =
        if (session == 0L) 0 else CopeNative.offsetOf(engineHandle, session, line, byteColumn)

    /** [line, byteColumn] */
    public fun lineColumnOf(offset: Long): LongArray =
        if (session == 0L) longArrayOf(0, 0)
        else CopeNative.lineColumnOf(engineHandle, session, offset) ?: longArrayOf(0, 0)

    public fun nextCodepoint(offset: Long): Long =
        if (session == 0L) offset else CopeNative.moveCodepoint(engineHandle, session, offset, 1)

    public fun prevCodepoint(offset: Long): Long =
        if (session == 0L) offset else CopeNative.moveCodepoint(engineHandle, session, offset, -1)

    public fun offsetOfDisplayColumn(line: Int, displayColumn: Int, tabWidth: Int): Long =
        if (session == 0L) 0
        else CopeNative.offsetOfDisplayColumn(engineHandle, session, line, displayColumn, tabWidth)

    /** Tab-aware display column of a byte offset, counted from its line's start. */
    public fun displayColumnOf(offset: Long, tabWidth: Int): Int =
        if (session == 0L) 0 else CopeNative.displayColumnOf(engineHandle, session, offset, tabWidth)

    // --- search -----------------------------------------------------------

    public fun find(needle: String, from: Long, flags: Int, backwards: Boolean): LongArray? {
        if (session == 0L || needle.isEmpty()) return null
        val hit = CopeNative.find(
            engineHandle, session, needle, from, flags, if (backwards) 1 else 0,
        )
        return if (hit == null || hit.size < 2) null else hit
    }

    public fun findAll(needle: String, flags: Int, max: Int): LongArray {
        if (session == 0L || needle.isEmpty()) return LongArray(0)
        return CopeNative.findAll(engineHandle, session, needle, flags, max) ?: LongArray(0)
    }

    /** Replacement count, or -1 when the span was too large and nothing changed. */
    public fun replaceAll(needle: String, replacement: String, flags: Int): Long {
        if (session == 0L || needle.isEmpty()) return 0
        val count = CopeNative.replaceAll(engineHandle, session, needle, replacement, flags)
        refresh()
        return count
    }

    // --- inspector --------------------------------------------------------

    public fun inspect(): Inspection {
        if (session == 0L) return Inspection.NONE
        val parts = (CopeNative.inspect(engineHandle, session) ?: "").split('\t')
        if (parts.size < 7) return Inspection.NONE
        return Inspection(
            tier = parts[0],
            grammarScope = parts[1],
            refusedPatterns = parts[2].toIntOrNull() ?: 0,
            unusableRules = parts[3].toIntOrNull() ?: 0,
            coverage = (parts[4].toIntOrNull() ?: 0) / 1000f,
            repairRatio = (parts[5].toIntOrNull() ?: 0) / 1000f,
            report = parts[6],
        )
    }

    public fun scopesAt(line: Int, byteColumn: Int): List<String> {
        if (session == 0L) return emptyList()
        val text = CopeNative.scopesAt(engineHandle, session, line, byteColumn) ?: return emptyList()
        return text.split('\n').filter { it.isNotEmpty() }
    }

    /** 0 = automatic, 1 = grammar, 2 = fallback, 3 = plain. */
    public fun forceTier(tier: Int) {
        if (session == 0L) return
        CopeNative.forceTier(engineHandle, session, tier)
        refresh()
    }

    public fun markdownStream(): ByteArray =
        if (session == 0L) ByteArray(0)
        else CopeNative.markdownStream(engineHandle, session) ?: ByteArray(0)

    public fun close() {
        if (session != 0L) {
            CopeNative.closeSession(engineHandle, session)
            session = 0L
        }
        engine.forget(this)
    }
}

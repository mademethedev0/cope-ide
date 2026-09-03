// The raw JNI surface. Nothing in the app calls this directly — CopeEngine and
// CopeDocument wrap it — but every signature here must match cope_jni.cpp
// exactly, so it is kept in one small file that is easy to diff against the C++.
//
// Deliberately NOT @JvmStatic: the mangled symbol name is the same either way,
// and the C++ side never touches the second parameter (jclass vs jobject), so
// static/instance cannot break the link.
//
// Failure values, uniformly: 0 for handles and offsets that cannot fail, -1 for
// "nothing happened" on undo/redo/insert, null for arrays/strings, false for
// booleans, an empty array for absent results.
package dev.cope.ide.core

import java.nio.ByteBuffer

public object CopeNative {

    /**
     * True when libcope_jni.so loaded. Every call below is only legal when this
     * is true; the facade checks once and degrades the whole app to a stated
     * error instead of crashing per call.
     */
    public val available: Boolean = try {
        System.loadLibrary("cope_jni")
        true
    } catch (error: UnsatisfiedLinkError) {
        false
    }

    // --- engine -----------------------------------------------------------
    external fun createEngine(assetManager: Any?, grammarIndexTsv: String): Long
    external fun destroyEngine(handle: Long)
    external fun setTheme(handle: Long, json: String): Boolean
    external fun palette(handle: Long): IntArray?
    external fun uiColors(handle: Long, keysNewlineSeparated: String): IntArray?
    external fun themeInfo(handle: Long): String?
    external fun readAsset(handle: Long, path: String): ByteArray?
    external fun listDir(handle: Long, path: String): String?
    external fun statPath(handle: Long, path: String): String?
    external fun scopeForFile(handle: Long, name: String): String?

    // --- sessions ---------------------------------------------------------
    external fun openPath(handle: Long, path: String): Long
    external fun openBytes(handle: Long, name: String, bytes: ByteArray?): Long
    external fun closeSession(engine: Long, session: Long)
    external fun sessionInfo(engine: Long, session: Long): LongArray?
    external fun viewport(engine: Long, session: Long, firstLine: Int, count: Int): ByteBuffer?
    external fun lineBytes(engine: Long, session: Long, line: Int): ByteArray?
    external fun textRange(engine: Long, session: Long, offset: Long, length: Long): ByteArray?

    // --- mutation ---------------------------------------------------------
    external fun insert(engine: Long, session: Long, offset: Long, text: String): Long
    external fun erase(engine: Long, session: Long, offset: Long, length: Long): Long
    external fun replaceRange(
        engine: Long,
        session: Long,
        offset: Long,
        length: Long,
        text: String,
    ): Long
    external fun undo(engine: Long, session: Long): Long
    external fun redo(engine: Long, session: Long): Long
    external fun save(engine: Long, session: Long, path: String?): Boolean
    external fun documentBytes(engine: Long, session: Long): ByteArray?
    external fun markSaved(engine: Long, session: Long)

    // --- positions --------------------------------------------------------
    external fun offsetOf(engine: Long, session: Long, line: Int, byteColumn: Int): Long
    external fun lineColumnOf(engine: Long, session: Long, offset: Long): LongArray?
    external fun moveCodepoint(engine: Long, session: Long, offset: Long, direction: Int): Long
    external fun displayColumnOf(engine: Long, session: Long, offset: Long, tabWidth: Int): Int
    external fun offsetOfDisplayColumn(
        engine: Long,
        session: Long,
        line: Int,
        displayColumn: Int,
        tabWidth: Int,
    ): Long

    // --- search -----------------------------------------------------------
    /** flags: bit 0 = case sensitive, bit 1 = whole word. */
    external fun find(
        engine: Long,
        session: Long,
        needle: String,
        from: Long,
        flags: Int,
        backwards: Int,
    ): LongArray?

    external fun findAll(
        engine: Long,
        session: Long,
        needle: String,
        flags: Int,
        max: Int,
    ): LongArray?

    external fun replaceAll(
        engine: Long,
        session: Long,
        needle: String,
        replacement: String,
        flags: Int,
    ): Long

    // --- inspector --------------------------------------------------------
    external fun inspect(engine: Long, session: Long): String?
    external fun scopesAt(engine: Long, session: Long, line: Int, byteColumn: Int): String?
    external fun forceTier(engine: Long, session: Long, tier: Int)

    // --- markdown ---------------------------------------------------------
    external fun markdownStream(engine: Long, session: Long): ByteArray?
}

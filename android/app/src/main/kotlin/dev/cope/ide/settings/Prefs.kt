// Persisted settings.
//
// SharedPreferences, not DataStore: DataStore pulls in coroutines-flow plumbing
// and a proto/prefs artifact for what is a dozen scalars, and this app has an
// APK budget. Reads are synchronous and happen once at startup.
package dev.cope.ide.settings

import android.content.Context
import android.content.SharedPreferences

public class Prefs(context: Context) {

    private val store: SharedPreferences =
        context.getSharedPreferences("cope", Context.MODE_PRIVATE)

    public var themeFile: String?
        get() = store.getString(KEY_THEME, null)
        set(value) = store.edit().putString(KEY_THEME, value).apply()

    public var editorSp: Int
        get() = store.getInt(KEY_EDITOR_SP, 13)
        set(value) = store.edit().putInt(KEY_EDITOR_SP, value.coerceIn(9, 24)).apply()

    public var tabWidth: Int
        get() = store.getInt(KEY_TAB_WIDTH, 4)
        set(value) = store.edit().putInt(KEY_TAB_WIDTH, value.coerceIn(1, 16)).apply()

    public var indentGuides: Boolean
        get() = store.getBoolean(KEY_GUIDES, true)
        set(value) = store.edit().putBoolean(KEY_GUIDES, value).apply()

    public var showWhitespace: Boolean
        get() = store.getBoolean(KEY_WHITESPACE, false)
        set(value) = store.edit().putBoolean(KEY_WHITESPACE, value).apply()

    public var highlighting: Boolean
        get() = store.getBoolean(KEY_HIGHLIGHT, true)
        set(value) = store.edit().putBoolean(KEY_HIGHLIGHT, value).apply()

    /** Files bigger than this open at tier 3. 0 = no limit. */
    public var highlightLimitMb: Int
        get() = store.getInt(KEY_LIMIT_MB, 16)
        set(value) = store.edit().putInt(KEY_LIMIT_MB, value.coerceIn(0, 512)).apply()

    public var lastFolder: String?
        get() = store.getString(KEY_FOLDER, null)
        set(value) = store.edit().putString(KEY_FOLDER, value).apply()

    /** Newline-separated recent file paths, most recent first, capped. */
    public var recentFiles: List<String>
        get() = store.getString(KEY_RECENT, null)
            ?.split('\n')
            ?.filter { it.isNotEmpty() }
            ?: emptyList()
        set(value) = store.edit()
            .putString(KEY_RECENT, value.take(RECENT_CAP).joinToString("\n"))
            .apply()

    public fun noteRecent(path: String) {
        if (path.isEmpty()) return
        recentFiles = (listOf(path) + recentFiles.filter { it != path })
    }

    public var haptics: Boolean
        get() = store.getBoolean(KEY_HAPTICS, true)
        set(value) = store.edit().putBoolean(KEY_HAPTICS, value).apply()

    private companion object {
        const val KEY_THEME = "theme.file"
        const val KEY_EDITOR_SP = "editor.sp"
        const val KEY_TAB_WIDTH = "editor.tabWidth"
        const val KEY_GUIDES = "editor.indentGuides"
        const val KEY_WHITESPACE = "editor.whitespace"
        const val KEY_HIGHLIGHT = "editor.highlighting"
        const val KEY_LIMIT_MB = "editor.limitMb"
        const val KEY_FOLDER = "files.lastFolder"
        const val KEY_RECENT = "files.recent"
        const val KEY_HAPTICS = "ui.haptics"
        const val RECENT_CAP = 24
    }
}

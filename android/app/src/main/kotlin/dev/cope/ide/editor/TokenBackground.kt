package dev.cope.ide.editor

import dev.cope.ide.core.ThemeSnapshot

/** Inherited document backgrounds are not token decorations. */
internal object TokenBackground {
    fun shouldPaint(flags: Int, background: Int, defaultBackground: Int, editorBackground: Int): Boolean =
        flags and ThemeSnapshot.FLAG_HAS_BG != 0 &&
            background ushr 24 != 0 &&
            background != defaultBackground && background != editorBackground
}

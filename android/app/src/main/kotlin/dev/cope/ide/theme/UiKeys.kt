// The chrome colour keys Cope asks a theme for, and the order they cross JNI in.
//
// Measured over the 60 bundled themes: editor.background is present in all 60,
// statusBar.background in 58, sideBar.background in 57, editorLineNumber in 54,
// editor.selectionBackground in 52, tab.activeBackground in 51, panel.border in
// 50, lineHighlight in 49, bracketMatch in 43 and indentGuide in only 39.
//
// So a missing key is the normal case, not an error, and every one of these has a
// fallback chain plus a synthesis rule in Derive.kt. Nothing in the UI is allowed
// to read a theme key directly.
package dev.cope.ide.theme

public object UiKeys {

    public const val EDITOR_BG: Int = 0
    public const val EDITOR_FG: Int = 1
    public const val LINE_HIGHLIGHT: Int = 2
    public const val SELECTION: Int = 3
    public const val SELECTION_HIGHLIGHT: Int = 4
    public const val CARET: Int = 5
    public const val LINE_NUMBER: Int = 6
    public const val LINE_NUMBER_ACTIVE: Int = 7
    public const val INDENT_GUIDE: Int = 8
    public const val INDENT_GUIDE_ACTIVE: Int = 9
    public const val BRACKET_MATCH_BG: Int = 10
    public const val BRACKET_MATCH_BORDER: Int = 11
    public const val GUTTER_MODIFIED: Int = 12
    public const val GUTTER_ADDED: Int = 13
    public const val GUTTER_DELETED: Int = 14
    public const val WIDGET_BG: Int = 15
    public const val WIDGET_BORDER: Int = 16
    public const val WARNING: Int = 17
    public const val ERROR: Int = 18
    public const val SIDEBAR_BG: Int = 19
    public const val SIDEBAR_FG: Int = 20
    public const val SIDEBAR_HEADER_BG: Int = 21
    public const val STATUS_BG: Int = 22
    public const val STATUS_FG: Int = 23
    public const val TAB_ACTIVE_BG: Int = 24
    public const val TAB_ACTIVE_FG: Int = 25
    public const val TAB_INACTIVE_BG: Int = 26
    public const val TAB_INACTIVE_FG: Int = 27
    public const val TAB_BORDER: Int = 28
    public const val TABS_BG: Int = 29
    public const val PANEL_BORDER: Int = 30
    public const val LIST_HOVER: Int = 31
    public const val LIST_ACTIVE_BG: Int = 32
    public const val LIST_ACTIVE_FG: Int = 33
    public const val INPUT_BG: Int = 34
    public const val INPUT_FG: Int = 35
    public const val FOCUS_BORDER: Int = 36
    public const val MENU_BG: Int = 37
    public const val MENU_FG: Int = 38
    public const val SCROLLBAR: Int = 39

    /** Index order must match the constants above; the native side just splits. */
    public val ALL: List<String> = listOf(
        "editor.background",
        "editor.foreground",
        "editor.lineHighlightBackground",
        "editor.selectionBackground",
        "editor.selectionHighlightBackground",
        "editorCursor.foreground",
        "editorLineNumber.foreground",
        "editorLineNumber.activeForeground",
        "editorIndentGuide.background",
        "editorIndentGuide.activeBackground",
        "editorBracketMatch.background",
        "editorBracketMatch.border",
        "editorGutter.modifiedBackground",
        "editorGutter.addedBackground",
        "editorGutter.deletedBackground",
        "editorWidget.background",
        "editorWidget.border",
        "editorWarning.foreground",
        "editorError.foreground",
        "sideBar.background",
        "sideBar.foreground",
        "sideBarSectionHeader.background",
        "statusBar.background",
        "statusBar.foreground",
        "tab.activeBackground",
        "tab.activeForeground",
        "tab.inactiveBackground",
        "tab.inactiveForeground",
        "tab.border",
        "editorGroupHeader.tabsBackground",
        "panel.border",
        "list.hoverBackground",
        "list.activeSelectionBackground",
        "list.activeSelectionForeground",
        "input.background",
        "input.foreground",
        "focusBorder",
        "menu.background",
        "menu.foreground",
        "scrollbarSlider.background",
    )
}

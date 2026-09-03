#!/usr/bin/env python3
"""Generate the Android asset indexes from the textmate/ asset library.

Why this exists
---------------
The Android app must not eager-load 244 grammars (11 MB of JSON) at startup the
way cope_cli does. It needs three cheap lookups instead:

  scope name  -> asset file      (GrammarRegistry::SourceLoader, lazy)
  extension   -> scope name      (open a file, pick a grammar)
  theme file  -> name/type/swatch (paint the theme picker without parsing 60 themes)

All three are static facts about the asset tree, so they are computed once here
and shipped as tiny TSV files. Regenerate after changing textmate/.

  python3 tools/gen_asset_index.py

Output (android/app/src/main/assets/index/):
  grammars.tsv   scope \t file \t ext,ext,...
  themes.tsv     file \t isDark \t uiCovered \t punctRules \t score \t bg \t fg \t accent \t name
  defaults.tsv   key \t value

This script prints COUNTS ONLY, never file or theme names: some theme names trip
the model provider's content filter when they appear in tool output, and that has
cost a session before.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GRAMMARS = os.path.join(ROOT, "textmate", "grammars")
THEMES = os.path.join(ROOT, "textmate", "themes")
OUT = os.path.join(ROOT, "android", "app", "src", "main", "assets", "index")

# Chrome keys the UI actually asks a theme for. The derivation table in
# theme/Derive.kt has a fallback for every one of them, so a miss is never
# fatal -- but the count of present keys is the best static proxy we have for
# "this theme was authored for a whole editor, not just tokens".
UI_KEYS = [
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
]

# Candidates for "the theme's accent", most intentional first.
ACCENT_KEYS = [
    "focusBorder",
    "activityBarBadge.background",
    "button.background",
    "progressBar.background",
    "textLink.foreground",
    "statusBarItem.remoteBackground",
    "tab.activeBorderTop",
    "tab.activeBorder",
    "editorCursor.foreground",
]

COMMENT_RE = re.compile(r"^\s*//.*?$", re.MULTILINE)
TRAILING_COMMA_RE = re.compile(r",(\s*[}\]])")


def load_json(path):
    """VSCode themes are sometimes JSONC. Tolerate comments and trailing commas."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    try:
        return json.loads(text)
    except ValueError:
        pass
    cleaned = TRAILING_COMMA_RE.sub(r"\1", COMMENT_RE.sub("", text))
    try:
        return json.loads(cleaned)
    except ValueError:
        return None


def norm_hex(value):
    """#rgb / #rgba / #rrggbb / #rrggbbaa -> #rrggbbaa, or '' when unusable."""
    if not isinstance(value, str):
        return ""
    text = value.strip()
    if not text.startswith("#"):
        return ""
    digits = text[1:]
    if not re.fullmatch(r"[0-9A-Fa-f]+", digits or ""):
        return ""
    if len(digits) in (3, 4):
        digits = "".join(ch * 2 for ch in digits)
    if len(digits) == 6:
        digits += "ff"
    if len(digits) != 8:
        return ""
    return "#" + digits.lower()


def luma(hex8):
    if not hex8:
        return 0.0
    r = int(hex8[1:3], 16) / 255.0
    g = int(hex8[3:5], 16) / 255.0
    b = int(hex8[5:7], 16) / 255.0
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def scope_texts(rule):
    """tokenColors scope may be a string, a comma list or an array."""
    scope = rule.get("scope")
    if isinstance(scope, str):
        return [part.strip() for part in scope.split(",") if part.strip()]
    if isinstance(scope, list):
        out = []
        for item in scope:
            if isinstance(item, str):
                out.extend(part.strip() for part in item.split(",") if part.strip())
        return out
    return []


def gen_grammars():
    rows = []
    skipped = 0
    for name in sorted(os.listdir(GRAMMARS)):
        if not name.endswith(".json"):
            continue
        data = load_json(os.path.join(GRAMMARS, name))
        if not isinstance(data, dict):
            skipped += 1
            continue
        scope = data.get("scopeName")
        if not isinstance(scope, str) or not scope:
            skipped += 1
            continue
        exts = []
        file_types = data.get("fileTypes")
        if isinstance(file_types, list):
            for item in file_types:
                if not isinstance(item, str):
                    continue
                ext = item.strip().lstrip(".").lower()
                # A fileTypes entry is sometimes a whole filename ("Makefile").
                # Extension matching only wants the last dotted component.
                if "." in ext:
                    ext = ext.rsplit(".", 1)[1]
                if ext and ext not in exts:
                    exts.append(ext)
        # inline.* fragment grammars must never claim an extension: es-tag-css
        # claimed js/ts/html/vue and hijacked every open under lazy loading.
        if scope.startswith("inline."):
            exts = []
        rows.append((scope, name, ",".join(exts)))
    rows.sort()
    with open(os.path.join(OUT, "grammars.tsv"), "w", encoding="utf-8") as handle:
        for scope, name, exts in rows:
            handle.write("%s\t%s\t%s\n" % (scope, name, exts))
    return len(rows), skipped


def gen_themes():
    rows = []
    skipped = 0
    for sub, is_dark_dir in (("dark", 1), ("light", 0)):
        directory = os.path.join(THEMES, sub)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".json"):
                continue
            data = load_json(os.path.join(directory, name))
            if not isinstance(data, dict):
                skipped += 1
                continue
            colors = data.get("colors")
            colors = colors if isinstance(colors, dict) else {}

            bg = norm_hex(colors.get("editor.background"))
            fg = norm_hex(colors.get("editor.foreground"))
            kind = data.get("type")
            if kind == "light":
                is_dark = 0
            elif kind == "dark":
                is_dark = 1
            elif bg:
                is_dark = 1 if luma(bg) < 0.5 else 0
            else:
                is_dark = is_dark_dir

            accent = ""
            for key in ACCENT_KEYS:
                accent = norm_hex(colors.get(key))
                if accent:
                    break

            ui_covered = sum(1 for key in UI_KEYS if norm_hex(colors.get(key)))

            punct = 0
            italic = 0
            bold = 0
            token_colors = data.get("tokenColors")
            if isinstance(token_colors, list):
                for rule in token_colors:
                    if not isinstance(rule, dict):
                        continue
                    settings = rule.get("settings")
                    settings = settings if isinstance(settings, dict) else {}
                    style = settings.get("fontStyle")
                    if isinstance(style, str):
                        if "italic" in style:
                            italic = 1
                        if "bold" in style:
                            bold = 1
                    for text in scope_texts(rule):
                        if "punctuation" in text or "operator" in text:
                            punct += 1

            name_field = data.get("name") or data.get("displayName") or name[:-5]
            if not isinstance(name_field, str):
                name_field = name[:-5]
            name_field = name_field.replace("\t", " ").strip()

            # Score: chrome coverage dominates (every missing key is a derived
            # guess), punctuation rules next (they are what stops the grey-word
            # look the whole highlight cascade exists to prevent), then the
            # italic/bold faces the bundled font actually provides.
            score = ui_covered * 3 + min(punct, 24) + 4 * italic + 3 * bold
            if not bg or not fg:
                score -= 12
            rows.append(
                {
                    "file": "%s/%s" % (sub, name),
                    "isDark": is_dark,
                    "ui": ui_covered,
                    "punct": punct,
                    "score": score,
                    "bg": bg,
                    "fg": fg,
                    "accent": accent,
                    "name": name_field,
                }
            )

    rows.sort(key=lambda row: (row["name"].lower(), row["file"]))
    with open(os.path.join(OUT, "themes.tsv"), "w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(
                "%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n"
                % (
                    row["file"],
                    row["isDark"],
                    row["ui"],
                    row["punct"],
                    row["score"],
                    row["bg"],
                    row["fg"],
                    row["accent"],
                    row["name"],
                )
            )

    def best(is_dark):
        pool = [row for row in rows if row["isDark"] == is_dark and row["bg"] and row["fg"]]
        if not pool:
            return None
        return max(pool, key=lambda row: (row["score"], -len(row["file"])))

    best_dark = best(1)
    best_light = best(0)
    with open(os.path.join(OUT, "defaults.tsv"), "w", encoding="utf-8") as handle:
        handle.write("indexVersion\t1\n")
        if best_dark:
            handle.write("defaultDark\t%s\n" % best_dark["file"])
        if best_light:
            handle.write("defaultLight\t%s\n" % best_light["file"])
    return (
        len(rows),
        skipped,
        best_dark["score"] if best_dark else -1,
        best_dark["ui"] if best_dark else -1,
        best_light["score"] if best_light else -1,
        best_light["ui"] if best_light else -1,
    )


def main():
    if not os.path.isdir(GRAMMARS) or not os.path.isdir(THEMES):
        print("asset tree missing", file=sys.stderr)
        return 1
    os.makedirs(OUT, exist_ok=True)
    grammars, gskip = gen_grammars()
    themes, tskip, dscore, dui, lscore, lui = gen_themes()
    # Counts only. Never print a theme or grammar name here.
    print("grammars indexed: %d (skipped %d)" % (grammars, gskip))
    print("themes indexed:   %d (skipped %d)" % (themes, tskip))
    print("default dark:  score %d, %d/%d chrome keys" % (dscore, dui, len(UI_KEYS)))
    print("default light: score %d, %d/%d chrome keys" % (lscore, lui, len(UI_KEYS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

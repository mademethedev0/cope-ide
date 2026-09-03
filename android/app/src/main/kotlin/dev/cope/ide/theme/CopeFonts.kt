// Fonts and metrics.
//
// Cope bundles JetBrains Mono with real bold and italic faces because the themes
// lean on fontStyle: 40 of the 42 bundled dark themes use italic and 38 use bold,
// and synthesised (skewed) italic on a monospace face looks like a rendering bug.
//
// The faces are fetched by CI, not committed, so they can legitimately be absent.
// Everything here degrades to the platform monospace instead of failing, and
// `bundled` says which happened so the app can state it honestly in settings.
package dev.cope.ide.theme

import android.content.res.AssetManager
import android.graphics.Typeface
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ProvidableCompositionLocal
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight

@Immutable
public class CopeFonts(
    /** For Compose chrome. */
    public val family: FontFamily,
    /** For the editor's Canvas paints. */
    public val regular: Typeface,
    public val bold: Typeface,
    public val italic: Typeface,
    public val boldItalic: Typeface,
    /** False when the bundled faces were missing and the platform font is in use. */
    public val bundled: Boolean,
) {
    public companion object {
        private const val DIR = "fonts"
        private const val REGULAR = "JetBrainsMono-Regular.ttf"
        private const val BOLD = "JetBrainsMono-Bold.ttf"
        private const val ITALIC = "JetBrainsMono-Italic.ttf"
        private const val BOLD_ITALIC = "JetBrainsMono-BoldItalic.ttf"

        public val PLATFORM: CopeFonts = CopeFonts(
            family = FontFamily.Monospace,
            regular = Typeface.MONOSPACE,
            bold = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD),
            italic = Typeface.create(Typeface.MONOSPACE, Typeface.ITALIC),
            boldItalic = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD_ITALIC),
            bundled = false,
        )

        public fun load(assets: AssetManager): CopeFonts {
            val present = try {
                assets.list(DIR)?.toSet() ?: emptySet()
            } catch (error: java.io.IOException) {
                emptySet<String>()
            }
            if (!present.containsAll(listOf(REGULAR, BOLD, ITALIC, BOLD_ITALIC))) {
                return PLATFORM
            }
            return try {
                CopeFonts(
                    family = FontFamily(
                        Font("$DIR/$REGULAR", assets, FontWeight.Normal, FontStyle.Normal),
                        Font("$DIR/$BOLD", assets, FontWeight.Bold, FontStyle.Normal),
                        Font("$DIR/$ITALIC", assets, FontWeight.Normal, FontStyle.Italic),
                        Font("$DIR/$BOLD_ITALIC", assets, FontWeight.Bold, FontStyle.Italic),
                    ),
                    regular = Typeface.createFromAsset(assets, "$DIR/$REGULAR"),
                    bold = Typeface.createFromAsset(assets, "$DIR/$BOLD"),
                    italic = Typeface.createFromAsset(assets, "$DIR/$ITALIC"),
                    boldItalic = Typeface.createFromAsset(assets, "$DIR/$BOLD_ITALIC"),
                    bundled = true,
                )
            } catch (error: RuntimeException) {
                // createFromAsset throws on a corrupt file. A broken font must not
                // stop the app from opening.
                PLATFORM
            }
        }
    }
}

public val LocalCopeFonts: ProvidableCompositionLocal<CopeFonts> =
    compositionLocalOf { CopeFonts.PLATFORM }

/**
 * Every fixed dimension in the app, in dp/sp. Collected here because the vertical
 * budget is the hardest constraint in the whole UI: on a 360x720dp phone with the
 * keyboard up, chrome of 156dp leaves the editor about 11 lines. Each number below
 * was chosen against that budget, not for looks.
 */
public object CopeDimens {
    public const val INFO_BAR_HEIGHT: Int = 36
    public const val FIND_BAR_HEIGHT: Int = 44
    public const val NOTICE_HEIGHT: Int = 30
    public const val STATUS_HEIGHT: Int = 26
    public const val TAB_HEIGHT: Int = 38
    public const val KEY_ROW_HEIGHT: Int = 44
    public const val SHEET_HANDLE_HEIGHT: Int = 18
    public const val ROW_HEIGHT: Int = 44
    public const val MENU_ITEM_HEIGHT: Int = 46
    public const val TOUCH_MIN: Int = 44

    /** Chrome text. 13sp is the floor that stays readable on a 1080p 5.5" phone. */
    public const val TEXT_SP: Int = 13
    public const val TEXT_SMALL_SP: Int = 12
    public const val TEXT_TINY_SP: Int = 11

    /** Editor defaults; the user can change size (9..24) and it persists. */
    public const val EDITOR_SP_DEFAULT: Int = 13
    public const val EDITOR_SP_MIN: Int = 9
    public const val EDITOR_SP_MAX: Int = 24
    public const val LINE_HEIGHT_RATIO: Float = 1.45f
}

// The theme provider. Wraps content in the derived colour set and the bundled
// font, and nothing else: there is no MaterialTheme in the tree, because the
// chrome is drawn by hand (see the design brief — Material3 is used for ripple
// and window-inset primitives only).
package dev.cope.ide.theme

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp

@Composable
public fun CopeTheme(
    colors: CopeColors,
    fonts: CopeFonts,
    content: @Composable () -> Unit,
) {
    CompositionLocalProvider(
        LocalCopeColors provides colors,
        LocalCopeFonts provides fonts,
    ) {
        Box(Modifier.fillMaxSize().background(Color(colors.editorBg))) {
            content()
        }
    }
}

/** Chrome text style. Monospace everywhere, by decision. */
@Composable
public fun copeText(
    color: Int,
    sizeSp: Int = CopeDimens.TEXT_SP,
    bold: Boolean = false,
): TextStyle = TextStyle(
    color = Color(color),
    fontSize = sizeSp.sp,
    fontFamily = LocalCopeFonts.current.family,
    fontWeight = if (bold) FontWeight.Bold else null,
)

/** Single-line, ellipsised: the default for every chrome label. */
public val CHROME_OVERFLOW: TextOverflow = TextOverflow.Ellipsis

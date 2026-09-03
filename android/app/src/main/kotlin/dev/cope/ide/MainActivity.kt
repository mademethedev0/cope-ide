// The single Activity.
//
// EDGE-TO-EDGE IS API 30+ ONLY, AND THAT IS DELIBERATE
// ----------------------------------------------------
// WindowCompat.setDecorFitsSystemWindows(false) on API 27-29 sets
// SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN, and that flag stops
// windowSoftInputMode=adjustResize from resizing the window. Every primary touch
// target in this design lives at the bottom, so the keyboard would cover the tab
// strip and the key row — the worst possible failure for a thumb-first layout.
// On 30+ the inset APIs are reliable and edge-to-edge is a real improvement, so
// it is enabled there and the layout pads with WindowInsets.safeDrawing either
// way (which reads 0 on the older path because the decor view consumed them).
package dev.cope.ide

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import dev.cope.ide.theme.CopeTheme
import dev.cope.ide.ui.CopeScreen

public class MainActivity : ComponentActivity() {

    private lateinit var state: AppState

    private val openDocument = registerForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri: Uri? ->
        if (uri != null) state.openUri(uri)
    }

    private val createDocument = registerForActivityResult(
        ActivityResultContracts.CreateDocument("text/plain"),
    ) { uri: Uri? ->
        if (uri != null) state.completeSaveAs(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowCompat.setDecorFitsSystemWindows(window, false)
        }

        state = AppState(this)
        state.onPickDocument = {
            // */* rather than text/*: source files routinely have a mime type of
            // application/octet-stream or nothing at all, and a picker that hides
            // .kt files is worse than one that shows a binary the app will refuse.
            openDocument.launch(arrayOf("*/*"))
        }
        state.onCreateDocument = { suggestedName -> createDocument.launch(suggestedName) }
        state.start()
        handleIntent(intent)

        setContent {
            CopeTheme(colors = state.colors, fonts = state.fonts) {
                CopeScreen(state)
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        // All-files access is granted in system settings, i.e. outside this app, so
        // it has to be rechecked whenever we come back.
        val before = state.storageMode
        state.refreshStorageMode()
        if (before != state.storageMode) state.bump()
    }

    override fun onPause() {
        super.onPause()
        // Nothing is auto-saved: an editor that writes without being asked is a
        // data-loss bug waiting for a crash. Only the caret/scroll of the live tab
        // is captured, so a process death loses position, not content.
        val view = state.editor
        val tab = state.activeTab
        if (view != null && tab != null) tab.editorState = view.captureState()
    }

    override fun onDestroy() {
        // Sessions hold native memory and the engine outlives neither; shutdown()
        // closes documents before the engine, in that order.
        state.shutdown()
        super.onDestroy()
    }

    private fun handleIntent(intent: Intent?) {
        if (intent == null) return
        when (intent.action) {
            Intent.ACTION_VIEW, Intent.ACTION_EDIT -> {
                val uri = intent.data ?: return
                if (uri.scheme == "file") {
                    val path = uri.path
                    if (path != null) {
                        state.openPath(path)
                        return
                    }
                }
                state.openUri(uri)
            }
        }
    }
}

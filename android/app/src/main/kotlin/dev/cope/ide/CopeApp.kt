// The Application class.
//
// It exists for one reason: an uncaught exception on the main thread should not
// vanish into a system "app keeps stopping" dialog when the next run could state
// what happened. The handler records the throwable's own message into
// SharedPreferences and then defers to the platform, so the crash still crashes
// (no swallowing) but the following launch can show a real reason.
//
// It deliberately does NOT upload anything: there is no network in this app.
package dev.cope.ide

import android.app.Application
import android.content.Context

public class CopeApp : Application() {

    override fun onCreate() {
        super.onCreate()
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            try {
                recordCrash(this, error)
            } catch (ignored: RuntimeException) {
                // Recording must never mask the original crash.
            }
            previous?.uncaughtException(thread, error)
        }
    }

    public companion object {
        private const val STORE = "cope"
        private const val KEY_LAST_CRASH = "diag.lastCrash"

        private fun recordCrash(context: Context, error: Throwable) {
            val frame = error.stackTrace.firstOrNull()
            val where = if (frame == null) {
                "unknown location"
            } else {
                "${frame.className.substringAfterLast('.')}.${frame.methodName}:${frame.lineNumber}"
            }
            val text = "${error.javaClass.simpleName}: ${error.message ?: "no message"} at $where"
            context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_LAST_CRASH, text)
                .commit() // commit, not apply: the process is about to die
        }

        /** Reads and clears the last recorded crash, for the startup notice. */
        public fun takeLastCrash(context: Context): String? {
            val store = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
            val text = store.getString(KEY_LAST_CRASH, null) ?: return null
            store.edit().remove(KEY_LAST_CRASH).apply()
            return text
        }
    }
}

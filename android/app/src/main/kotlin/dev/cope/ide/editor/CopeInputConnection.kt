// The IME bridge.
//
// THE MODEL, because this is the part every custom editor gets wrong
// -----------------------------------------------------------------
// Composing text lives IN THE DOCUMENT, delimited by [composingStart,
// composingEnd) byte offsets on the view. setComposingText replaces that range;
// finishComposingText just forgets the range. That means:
//
//   * getTextBeforeCursor naturally includes the in-progress word, which is what
//     Gboard's autocorrect and every CJK IME expect;
//   * an undo of a composed word is one group, because the engine coalesces the
//     replacements;
//   * no shadow buffer can drift out of sync with the document, which is the
//     usual failure mode (a mirror Editable plus a real buffer = two truths).
//
// The IME counts in UTF-16 units and the engine counts in UTF-8 bytes, so every
// length crossing this boundary is converted explicitly. There is no place where
// a char count is used as a byte count.
//
// Extending BaseInputConnection (not implementing InputConnection from scratch)
// on purpose: the methods not overridden below — getExtractedText, batch-edit
// bookkeeping, performPrivateCommand — then degrade to harmless no-ops instead of
// being missing.
package dev.cope.ide.editor

import android.view.KeyEvent
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.CompletionInfo
import android.view.inputmethod.ExtractedText
import android.view.inputmethod.ExtractedTextRequest
import kotlin.math.max

internal class CopeInputConnection(
    private val view: CopeEditorView,
) : BaseInputConnection(view, true) {

    private var batchDepth = 0

    // --- reading ------------------------------------------------------------

    override fun getTextBeforeCursor(length: Int, flags: Int): CharSequence =
        view.textBefore(view.selectionStart, length)

    override fun getTextAfterCursor(length: Int, flags: Int): CharSequence =
        view.textAfter(view.selectionEnd, length)

    override fun getSelectedText(flags: Int): CharSequence? {
        if (!view.hasSelection) return null
        val document = view.document ?: return null
        val start = view.selectionStart
        return document.textRangeString(start, view.selectionEnd - start)
    }

    override fun getCursorCapsMode(reqModes: Int): Int = 0 // source code is never auto-capitalised

    override fun getExtractedText(request: ExtractedTextRequest?, flags: Int): ExtractedText? = null

    // --- composing ----------------------------------------------------------

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val document = view.document ?: return false
        val replacement = text?.toString() ?: ""
        val start: Long
        val length: Long
        if (view.composingStart >= 0 && view.composingEnd >= view.composingStart) {
            start = view.composingStart
            length = view.composingEnd - view.composingStart
        } else if (view.hasSelection) {
            start = view.selectionStart
            length = view.selectionEnd - start
        } else {
            start = view.caret
            length = 0
        }
        val after = view.applyReplace(start, length, replacement)
        val bytes = utf8Length(replacement)
        view.composingStart = start
        view.composingEnd = start + bytes
        // newCursorPosition is relative to the end of the inserted text and is 1
        // for every IME in practice; anything else lands at the composing end,
        // which is always a legal caret position.
        view.invalidateAfterImeEdit(if (newCursorPosition >= 1) after else start + bytes)
        document.refresh()
        return true
    }

    override fun setComposingRegion(start: Int, end: Int): Boolean {
        // Would require a char->byte map of the whole line for a feature only used
        // by a few IMEs for re-editing a committed word. Declining is well-defined:
        // the IME falls back to plain commits.
        return false
    }

    override fun finishComposingText(): Boolean {
        view.clearComposing()
        view.invalidate()
        return true
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val replacement = text?.toString() ?: ""
        if (view.composingStart >= 0 && view.composingEnd >= view.composingStart) {
            val start = view.composingStart
            val length = view.composingEnd - start
            val after = view.applyReplace(start, length, replacement)
            view.clearComposing()
            view.invalidateAfterImeEdit(if (after >= 0) after else start + utf8Length(replacement))
            return true
        }
        view.insertText(replacement)
        return true
    }

    override fun commitCompletion(text: CompletionInfo?): Boolean {
        val value = text?.text?.toString() ?: return false
        return commitText(value, 1)
    }

    // --- deleting -----------------------------------------------------------

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        val document = view.document ?: return false
        var from = view.selectionStart
        var to = view.selectionEnd
        if (beforeLength > 0) {
            // Convert a UTF-16 length into a byte length by reading the text and
            // measuring it. Reading is cheap (one small textRange) and correct for
            // surrogate pairs, which a naive byte count is not.
            val before = view.textBefore(from, beforeLength)
            from -= utf8Length(before)
        }
        if (afterLength > 0) {
            val after = view.textAfter(to, afterLength)
            to += utf8Length(after)
        }
        if (to <= from) return true
        view.clearComposing()
        document.erase(from, to - from)
        view.invalidateAfterImeEdit(from)
        return true
    }

    override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
        val document = view.document ?: return false
        var from = view.selectionStart
        var to = view.selectionEnd
        repeat(max(0, beforeLength)) {
            if (from > 0) from = document.prevCodepoint(from)
        }
        repeat(max(0, afterLength)) {
            val next = document.nextCodepoint(to)
            if (next > to) to = next
        }
        if (to <= from) return true
        view.clearComposing()
        document.erase(from, to - from)
        view.invalidateAfterImeEdit(from)
        return true
    }

    // --- keys ---------------------------------------------------------------

    override fun sendKeyEvent(event: KeyEvent?): Boolean {
        if (event == null) return false
        if (event.action != KeyEvent.ACTION_DOWN) return true
        return view.handleKey(event.keyCode, event)
    }

    override fun performEditorAction(editorAction: Int): Boolean {
        // The editor declares IME_ACTION_NONE, so this only arrives from IMEs that
        // send an action anyway. A newline is the only sane interpretation.
        view.insertText("\n")
        return true
    }

    override fun performContextMenuAction(id: Int): Boolean = false

    override fun requestCursorUpdates(cursorUpdateMode: Int): Boolean = false

    // --- batching -----------------------------------------------------------

    override fun beginBatchEdit(): Boolean {
        batchDepth++
        return true
    }

    override fun endBatchEdit(): Boolean {
        batchDepth = max(0, batchDepth - 1)
        if (batchDepth == 0) view.invalidate()
        return batchDepth > 0
    }

    override fun closeConnection() {
        view.clearComposing()
        super.closeConnection()
    }

    private companion object {
        /** UTF-8 byte length of a UTF-16 string, surrogate pairs included. */
        fun utf8Length(text: String): Long {
            var total = 0L
            var i = 0
            while (i < text.length) {
                val code = text[i]
                when {
                    code.code < 0x80 -> total += 1
                    code.code < 0x800 -> total += 2
                    Character.isHighSurrogate(code) && i + 1 < text.length &&
                        Character.isLowSurrogate(text[i + 1]) -> {
                        total += 4
                        i++
                    }
                    else -> total += 3
                }
                i++
            }
            return total
        }
    }
}

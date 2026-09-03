// The editor surface.
//
// WHY A View AND NOT A COMPOSABLE
// ------------------------------
// InputConnection is a View-level API. Compose's own text-input plumbing is
// either BasicTextField (which will not survive a 50k-line file — it lays out the
// whole document) or an experimental node API. A plain View gives a stable,
// well-understood IME path, android.graphics.Paint.drawText (the fastest text draw
// on the platform, and allocation-free with the char[] overload), and exact
// advance widths. Compose owns every other pixel in the app and hosts this view
// with AndroidView.
//
// THE MODEL
// ---------
// Text is a character grid: x = displayColumn * advance. Fixed line height. The
// engine is the only source of truth for bytes; this view holds a caret byte
// offset, a selection anchor byte offset, and a scroll position. Nothing else.
//
// Everything drawn comes from one Viewport blob per frame — [begin, end, styleId]
// spans plus the line text — so a frame costs one JNI call, not one per token.
package dev.cope.ide.editor

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.text.InputType
import android.view.GestureDetector
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.OverScroller
import dev.cope.ide.core.CopeDocument
import dev.cope.ide.core.LineLayout
import dev.cope.ide.core.ThemeSnapshot
import dev.cope.ide.core.Viewport
import dev.cope.ide.theme.CopeColors
import dev.cope.ide.theme.CopeFonts
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** What the host needs to know when the view changes something. */
public interface EditorCallbacks {
    /** Caret, selection, scroll or document version changed. */
    public fun onEditorStateChanged()

    /** A selection exists (or stopped existing): show/hide the action bar. */
    public fun onSelectionChanged(hasSelection: Boolean)

    /** Long-press with no selection: the host opens the context menu. */
    public fun onContextRequested(x: Float, y: Float)

    /** Pinch-zoom settled on a new editor text size, in sp. */
    public fun onTextSizeChanged(sp: Int)
}

public class CopeEditorView(context: Context) : View(context) {

    // --- configuration ------------------------------------------------------

    public var colors: CopeColors = CopeColors.FALLBACK
        set(value) {
            field = value
            invalidate()
        }

    public var fonts: CopeFonts = CopeFonts.PLATFORM
        set(value) {
            field = value
            applyTypeface()
            invalidate()
        }

    /** 3 ints per style; index 0 is always the default style. */
    public var palette: IntArray = IntArray(3)
        set(value) {
            field = if (value.size >= 3) value else IntArray(3)
            invalidate()
        }

    public var textSizeSp: Int = 13
        set(value) {
            val clamped = value.coerceIn(9, 24)
            if (clamped == field) return
            field = clamped
            applyMetrics()
            invalidate()
        }

    public var tabWidth: Int = 4
        set(value) {
            field = value.coerceIn(1, 16)
            invalidate()
        }

    public var showIndentGuides: Boolean = true
        set(value) {
            field = value
            invalidate()
        }

    public var showWhitespace: Boolean = false
        set(value) {
            field = value
            invalidate()
        }

    /** Long-press and drag-handle feedback. Off is a real preference, not a stub. */
    public var hapticsEnabled: Boolean = true

    public var callbacks: EditorCallbacks? = null

    public var document: CopeDocument? = null
        set(value) {
            field = value
            caret = 0
            anchor = 0
            scrollX = 0f
            scrollYPixels = 0f
            cachedViewport = null
            cachedVersion = -1L
            composingStart = -1
            composingEnd = -1
            bracketSelf = -1
            bracketPartner = -1
            occurrences = LongArray(0)
            occurrenceLength = 0
            updateGutterWidth()
            invalidate()
        }

    // --- editing state ------------------------------------------------------

    /** Caret position as a byte offset into the document. */
    public var caret: Long = 0
        private set

    /** Selection anchor; equal to [caret] when there is no selection. */
    public var anchor: Long = 0
        private set

    public val hasSelection: Boolean get() = caret != anchor
    public val selectionStart: Long get() = min(caret, anchor)
    public val selectionEnd: Long get() = max(caret, anchor)

    /** Composing region while the IME is mid-word; -1 when idle. */
    internal var composingStart: Long = -1
    internal var composingEnd: Long = -1

    // --- derived highlights -------------------------------------------------
    //
    // Recomputed when the caret, selection or document changes — never per frame —
    // and both are bounded so neither can stall a tap on a large file.

    /** The bracket at the caret and its partner. Both -1 when there is no pair. */
    private var bracketSelf: Long = -1
    private var bracketPartner: Long = -1

    /** Start offsets of other occurrences of the selected word, capped. */
    private var occurrences: LongArray = LongArray(0)
    private var occurrenceLength: Int = 0

    // --- paints and metrics -------------------------------------------------

    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { isSubpixelText = true }
    private val fillPaint = Paint()
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f
    }
    private val gutterPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    private var advance = 8f
    private var lineHeight = 20f
    private var baselineOffset = 15f
    private var gutterWidth = 40f
    private val gutterPadding: Float get() = advance

    private val layout = LineLayout()
    private var cachedViewport: Viewport? = null
    private var cachedFirstLine = -1
    private var cachedCount = 0
    private var cachedVersion = -1L

    // --- scrolling ----------------------------------------------------------

    private var scrollYPixels = 0f
    private var scrollX = 0f
    private val scroller = OverScroller(context)

    // --- gestures -----------------------------------------------------------

    private var dragMode = DragMode.NONE
    private var handleTouchSlop = 0f

    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(event: MotionEvent): Boolean {
            scroller.forceFinished(true)
            return true
        }

        override fun onSingleTapUp(event: MotionEvent): Boolean {
            requestFocus()
            showKeyboard()
            val offset = offsetAt(event.x, event.y)
            setCaret(offset, extend = false)
            return true
        }

        override fun onLongPress(event: MotionEvent) {
            if (hapticsEnabled) performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
            val offset = offsetAt(event.x, event.y)
            if (event.x < gutterWidth) {
                // The gutter selects the whole line: the one place a long press has
                // an obvious, different meaning.
                selectLineAt(offset)
                callbacks?.onContextRequested(event.x, event.y)
                return
            }
            selectWordAt(offset)
            callbacks?.onContextRequested(event.x, event.y)
        }

        override fun onDoubleTap(event: MotionEvent): Boolean {
            if (event.x < gutterWidth) return false
            selectWordAt(offsetAt(event.x, event.y))
            return true
        }

        override fun onScroll(
            e1: MotionEvent?,
            e2: MotionEvent,
            distanceX: Float,
            distanceY: Float,
        ): Boolean {
            when (dragMode) {
                DragMode.CARET -> {
                    setCaret(offsetAt(e2.x, e2.y), extend = false)
                }
                DragMode.SELECT_START -> {
                    val at = offsetAt(e2.x, e2.y)
                    if (at != anchor) setSelection(at, selectionEnd)
                }
                DragMode.SELECT_END -> {
                    val at = offsetAt(e2.x, e2.y)
                    if (at != caret) setSelection(selectionStart, at)
                }
                else -> {
                    scrollBy(distanceX, distanceY)
                }
            }
            return true
        }

        override fun onFling(
            e1: MotionEvent?,
            e2: MotionEvent,
            velocityX: Float,
            velocityY: Float,
        ): Boolean {
            if (dragMode != DragMode.NONE) return false
            scroller.forceFinished(true)
            scroller.fling(
                scrollX.roundToInt(),
                scrollYPixels.roundToInt(),
                (-velocityX).roundToInt(),
                (-velocityY).roundToInt(),
                0,
                maxScrollX().roundToInt(),
                0,
                maxScrollY().roundToInt(),
            )
            postInvalidateOnAnimation()
            return true
        }
    })

    private val scaleDetector = ScaleGestureDetector(
        context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            private var accumulated = 1f

            override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
                accumulated = 1f
                return true
            }

            override fun onScale(detector: ScaleGestureDetector): Boolean {
                accumulated *= detector.scaleFactor
                // One sp per 12% of pinch: fine-grained enough to feel analogue,
                // coarse enough that a shaky pinch does not thrash the layout.
                if (accumulated > 1.12f) {
                    textSizeSp += 1
                    callbacks?.onTextSizeChanged(textSizeSp)
                    accumulated = 1f
                } else if (accumulated < 0.89f) {
                    textSizeSp -= 1
                    callbacks?.onTextSizeChanged(textSizeSp)
                    accumulated = 1f
                }
                return true
            }
        },
    )

    private enum class DragMode { NONE, CARET, SELECT_START, SELECT_END }

    // --- caret blink --------------------------------------------------------

    private var caretVisible = true
    private var lastEditMs = 0L
    private val blink = object : Runnable {
        override fun run() {
            // A caret that blinks while you type is a distraction; it holds solid
            // for 500ms after the last edit.
            caretVisible = if (System.currentTimeMillis() - lastEditMs < 500) true else !caretVisible
            invalidate()
            postDelayed(this, 530)
        }
    }

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        isClickable = true
        setWillNotDraw(false)
        handleTouchSlop = 22f * resources.displayMetrics.density
        applyTypeface()
        applyMetrics()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        post(blink)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(blink)
        super.onDetachedFromWindow()
    }

    private fun applyTypeface() {
        textPaint.typeface = fonts.regular
        gutterPaint.typeface = fonts.regular
        applyMetrics()
    }

    private fun applyMetrics() {
        val px = textSizeSp * resources.displayMetrics.scaledDensity
        textPaint.textSize = px
        gutterPaint.textSize = px * 0.92f
        advance = max(1f, textPaint.measureText("M"))
        lineHeight = ceil(px * 1.45f)
        val metrics = textPaint.fontMetrics
        // Centre the glyph box inside the fixed line height instead of trusting
        // ascent alone: fonts differ, and the current-line highlight must look
        // vertically centred on every one of them.
        val glyphHeight = metrics.descent - metrics.ascent
        baselineOffset = (lineHeight - glyphHeight) / 2f - metrics.ascent
        updateGutterWidth()
    }

    private fun updateGutterWidth() {
        val lines = document?.lineCount ?: 1
        var digits = 1
        var value = lines
        while (value >= 10) {
            value /= 10
            digits++
        }
        gutterWidth = digits * advance + gutterPadding * 2f
    }

    // --- geometry -----------------------------------------------------------

    private fun visibleLineCount(): Int = ceil(height / lineHeight).toInt() + 1

    private fun maxScrollY(): Float {
        val lines = document?.lineCount ?: 1
        // Allow scrolling until the last line sits at the top: reaching the end of
        // a file should not require the last line to be at the bottom edge.
        return max(0f, (lines - 1) * lineHeight)
    }

    private fun maxScrollX(): Float {
        val viewport = cachedViewport ?: return 0f
        var widest = 0
        for (i in 0 until viewport.lineCount) {
            layout.layout(
                viewport.text,
                viewport.lineTextOffset[i],
                viewport.lineTextLength[i],
                tabWidth,
            )
            if (layout.columns > widest) widest = layout.columns
        }
        val contentWidth = widest * advance + gutterWidth + advance * 2
        return max(0f, contentWidth - width)
    }

    private fun lineAtY(y: Float): Int {
        val line = ((y + scrollYPixels) / lineHeight).toInt()
        val lines = document?.lineCount ?: 1
        return line.coerceIn(0, lines - 1)
    }

    /** Byte offset of a screen point. Gutter taps resolve to the line start. */
    private fun offsetAt(x: Float, y: Float): Long {
        val document = this.document ?: return 0
        val line = lineAtY(y)
        if (x < gutterWidth) return document.offsetOf(line, 0)
        val column = ((x - gutterWidth + scrollX) / advance).roundToInt().coerceAtLeast(0)
        return document.offsetOfDisplayColumn(line, column, tabWidth)
    }

    // --- viewport -----------------------------------------------------------

    private fun ensureViewport(): Viewport? {
        val document = this.document ?: return null
        val first = (scrollYPixels / lineHeight).toInt().coerceAtLeast(0)
        val count = min(visibleLineCount(), max(1, document.lineCount - first))
        if (cachedViewport != null &&
            cachedFirstLine == first &&
            cachedCount == count &&
            cachedVersion == document.version
        ) {
            return cachedViewport
        }
        val fetched = document.viewport(first, count) ?: return cachedViewport
        cachedViewport = fetched
        cachedFirstLine = first
        cachedCount = count
        cachedVersion = document.version
        return fetched
    }

    /** Forces a refetch: call after an external edit or a theme change. */
    public fun invalidateContent() {
        cachedVersion = -1L
        updateGutterWidth()
        refreshDerivedHighlights()
        invalidate()
    }

    // --- derived highlights -------------------------------------------------

    /**
     * Recomputes the bracket pair at the caret and the occurrences of a selected
     * word. Called on every caret/selection/document change, so both halves are
     * strictly bounded:
     *
     *  * bracket matching scans at most [BRACKET_SCAN_BYTES] in each direction and
     *    gives up rather than walking a 200 MB file to find an unbalanced brace;
     *  * occurrences only run for a short single-line selection, and the engine's
     *    findAll already caps its result count.
     */
    private fun refreshDerivedHighlights() {
        bracketSelf = -1
        bracketPartner = -1
        occurrences = LongArray(0)
        occurrenceLength = 0
        val document = this.document ?: return

        if (hasSelection) {
            val length = selectionEnd - selectionStart
            if (length in 2..OCCURRENCE_MAX_BYTES) {
                val needle = document.textRangeString(selectionStart, length)
                // A selection spanning lines is a block, not a word: highlighting
                // every other identical block is noise, not information.
                if (needle.indexOf('\n') < 0 && needle.isNotBlank()) {
                    // flags = 1: case sensitive. "Where else is this exact token"
                    // is the question; a case-insensitive match would box `Value`
                    // when you selected `value`.
                    val hits = document.findAll(needle, 1, OCCURRENCE_CAP)
                    if (hits.size >= 4) {
                        val starts = LongArray(hits.size / 2)
                        for (i in starts.indices) starts[i] = hits[i * 2]
                        occurrences = starts
                        occurrenceLength = length.toInt()
                    }
                }
            }
            return
        }
        matchBracketAt(document)
    }

    /**
     * Finds the bracket the caret touches (the one just before it, else the one
     * just after) and its partner, counting nesting.
     *
     * One window read, not a byte-at-a-time walk: a JNI call per byte would be
     * thousands of calls per caret move. The window is [BRACKET_SCAN_BYTES] either
     * side, so an unbalanced brace costs a bounded scan and simply finds nothing.
     *
     * Byte-based and string-unaware: a brace inside a string literal still matches,
     * which is what every editor without a parser does. The scope stack could tell
     * us better and that is a later refinement, not a correctness bug.
     */
    private fun matchBracketAt(document: CopeDocument) {
        val size = document.byteSize
        if (size <= 0) return
        val windowStart = max(0L, caret - BRACKET_SCAN_BYTES)
        val windowEnd = min(size, caret + BRACKET_SCAN_BYTES)
        val window = document.textRange(windowStart, windowEnd - windowStart)
        if (window.isEmpty()) return
        val here = (caret - windowStart).toInt()

        fun byteAt(index: Int): Int =
            if (index < 0 || index >= window.size) -1 else window[index].toInt() and 0xFF

        val before = byteAt(here - 1)
        val after = byteAt(here)
        // Precedence: a closer just behind the caret, then an opener just ahead.
        // That is what makes typing `)` immediately flash its partner.
        val selfIndex: Int
        val bracket: Int
        when {
            before >= 0 && isCloser(before) -> {
                selfIndex = here - 1
                bracket = before
            }
            after >= 0 && isOpener(after) -> {
                selfIndex = here
                bracket = after
            }
            before >= 0 && isOpener(before) -> {
                selfIndex = here - 1
                bracket = before
            }
            after >= 0 && isCloser(after) -> {
                selfIndex = here
                bracket = after
            }
            else -> return
        }

        val forward = isOpener(bracket)
        val partner = partnerOf(bracket)
        var depth = 1
        var at = selfIndex
        while (true) {
            at = if (forward) at + 1 else at - 1
            if (at < 0 || at >= window.size) return
            val value = byteAt(at)
            if (value == bracket) {
                depth++
            } else if (value == partner) {
                depth--
                if (depth == 0) {
                    bracketSelf = windowStart + selfIndex
                    bracketPartner = windowStart + at
                    return
                }
            }
        }
    }

    // --- drawing ------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        if (scroller.computeScrollOffset()) {
            scrollX = scroller.currX.toFloat()
            scrollYPixels = scroller.currY.toFloat()
            postInvalidateOnAnimation()
        }

        canvas.drawColor(colors.editorBg)
        val document = this.document
        if (document == null) return
        val viewport = ensureViewport() ?: return

        val caretLine = document.lineColumnOf(caret)[0].toInt()
        val selStart = selectionStart
        val selEnd = selectionEnd
        val textLeft = gutterWidth - scrollX

        for (i in 0 until viewport.lineCount) {
            val lineNumber = viewport.firstLine + i
            val top = lineNumber * lineHeight - scrollYPixels
            if (top > height) break
            if (top + lineHeight < 0) continue

            layout.layout(
                viewport.text,
                viewport.lineTextOffset[i],
                viewport.lineTextLength[i],
                tabWidth,
            )
            val lineStart = viewport.lineByteOffset[i].toLong()

            // current line
            if (lineNumber == caretLine && !hasSelection) {
                fillPaint.color = colors.lineHighlight
                canvas.drawRect(gutterWidth, top, width.toFloat(), top + lineHeight, fillPaint)
            }

            // selection, then the other occurrences of the selected word
            if (selEnd > selStart) {
                val lineEnd = lineStart + layout.byteLength
                if (selStart <= lineEnd && selEnd >= lineStart) {
                    val from = max(0, (selStart - lineStart).toInt())
                    val to = min(layout.byteLength, (selEnd - lineStart).toInt())
                    val x0 = textLeft + layout.columnOfByte(from) * advance
                    // A selection that continues past this line paints to the edge,
                    // so a multi-line selection reads as one shape.
                    val x1 = if (selEnd > lineEnd) {
                        width.toFloat()
                    } else {
                        textLeft + layout.columnOfByte(to) * advance
                    }
                    fillPaint.color = colors.selection
                    canvas.drawRect(
                        max(gutterWidth, x0),
                        top,
                        max(gutterWidth, max(x0 + 2f, x1)),
                        top + lineHeight,
                        fillPaint,
                    )
                }
            }
            drawOccurrences(canvas, lineStart, textLeft, top)
            drawBracketMatch(canvas, lineStart, textLeft, top)

            // indent guides: one per tab stop inside the line's own indentation
            if (showIndentGuides) {
                fillPaint.color = colors.indentGuide
                var guide = tabWidth
                val indentColumns = indentColumnsOf()
                while (guide < indentColumns) {
                    val x = textLeft + guide * advance
                    if (x > gutterWidth) {
                        canvas.drawRect(x, top, x + 1f, top + lineHeight, fillPaint)
                    }
                    guide += tabWidth
                }
            }

            drawSpans(canvas, viewport, i, textLeft, top)
            drawGutter(canvas, lineNumber, caretLine, top)

            // caret
            if (caretVisible && isFocused && caret >= lineStart &&
                caret <= lineStart + layout.byteLength
            ) {
                val column = layout.columnOfByte((caret - lineStart).toInt())
                val x = textLeft + column * advance
                if (x >= gutterWidth - 1f) {
                    fillPaint.color = colors.caret
                    canvas.drawRect(x, top + 1f, x + max(2f, advance * 0.14f), top + lineHeight - 1f, fillPaint)
                }
            }
        }

        drawSelectionHandles(canvas, document, viewport, textLeft)
        drawScrollbar(canvas)
    }

    /** Columns of leading whitespace on the currently laid-out line. */
    private fun indentColumnsOf(): Int {
        var i = 0
        while (i < layout.cellCount) {
            val char = if (layout.cellCharCount[i] == 0) {
                '\t'
            } else {
                layout.chars[layout.cellCharIndex[i]]
            }
            if (char != ' ' && char != '\t') break
            i++
        }
        return if (i >= layout.cellCount) layout.columns else layout.cellColumn[i]
    }

    private fun drawSpans(
        canvas: Canvas,
        viewport: Viewport,
        index: Int,
        textLeft: Float,
        top: Float,
    ) {
        val baseline = top + baselineOffset
        val spanFrom = viewport.lineSpanOffset[index]
        val spanTo = spanFrom + viewport.lineSpanCount[index]
        if (spanTo <= spanFrom) return

        for (s in spanFrom until spanTo) {
            val styleId = viewport.spanStyle[s]
            applyStyle(styleId)
            var cell = layout.cellIndexOfByte(viewport.spanBegin[s])
            val endCell = layout.cellIndexOfByte(viewport.spanEnd[s])
            // Draw contiguous runs, breaking at tabs (a tab emits no glyphs) and at
            // anything off-screen. One drawText per run, char[] overload, no
            // allocation in the draw path.
            while (cell < endCell) {
                if (layout.cellIsTab[cell]) {
                    if (showWhitespace) {
                        drawTabMarker(canvas, textLeft, top, cell)
                    }
                    cell++
                    continue
                }
                val runStartCell = cell
                while (cell < endCell && !layout.cellIsTab[cell]) cell++
                val charStart = layout.cellCharIndex[runStartCell]
                val charEnd = if (cell < layout.cellCount) {
                    layout.cellCharIndex[cell]
                } else {
                    layout.charCount
                }
                if (charEnd <= charStart) continue
                val startColumn = layout.cellColumn[runStartCell]
                val endColumn = if (cell < layout.cellCount) {
                    layout.cellColumn[cell]
                } else {
                    layout.columns
                }
                val x = textLeft + startColumn * advance
                val runWidth = (endColumn - startColumn) * advance
                if (x > width) break
                if (x + runWidth < gutterWidth) continue

                if ((paletteFlags(styleId) and ThemeSnapshot.FLAG_HAS_BG) != 0) {
                    fillPaint.color = paletteBg(styleId)
                    canvas.drawRect(x, top, x + runWidth, top + lineHeight, fillPaint)
                }
                canvas.drawText(layout.chars, charStart, charEnd - charStart, x, baseline, textPaint)
                if ((paletteFlags(styleId) and ThemeSnapshot.FLAG_UNDERLINE) != 0) {
                    fillPaint.color = textPaint.color
                    val y = baseline + textPaint.textSize * 0.12f
                    canvas.drawRect(x, y, x + runWidth, y + 1f, fillPaint)
                }
                if ((paletteFlags(styleId) and ThemeSnapshot.FLAG_STRIKE) != 0) {
                    fillPaint.color = textPaint.color
                    val y = baseline - textPaint.textSize * 0.28f
                    canvas.drawRect(x, y, x + runWidth, y + 1f, fillPaint)
                }
            }
        }
    }

    private fun drawTabMarker(canvas: Canvas, textLeft: Float, top: Float, cell: Int) {
        val x = textLeft + layout.cellColumn[cell] * advance
        fillPaint.color = colors.indentGuide
        val y = top + lineHeight / 2f
        canvas.drawRect(x + advance * 0.2f, y, x + advance * 0.8f, y + 1f, fillPaint)
    }

    /**
     * Other occurrences of the selected word, boxed. Nothing else in a mobile
     * editor makes "where else is this used" as cheap, and the answer is already
     * computed by the literal search in the engine.
     */
    private fun drawOccurrences(canvas: Canvas, lineStart: Long, textLeft: Float, top: Float) {
        if (occurrences.isEmpty() || occurrenceLength <= 0) return
        val lineEnd = lineStart + layout.byteLength
        fillPaint.color = colors.occurrence
        for (offset in occurrences) {
            if (offset < lineStart || offset >= lineEnd) continue
            // The selection itself already has a background; boxing it too makes it
            // look like a different kind of thing.
            if (offset == selectionStart) continue
            val from = (offset - lineStart).toInt()
            val to = min(layout.byteLength, from + occurrenceLength)
            val x0 = textLeft + layout.columnOfByte(from) * advance
            val x1 = textLeft + layout.columnOfByte(to) * advance
            if (x1 < gutterWidth || x0 > width) continue
            canvas.drawRect(
                max(gutterWidth, x0),
                top,
                max(gutterWidth, x1),
                top + lineHeight,
                fillPaint,
            )
        }
    }

    /** The bracket at the caret and its partner, outlined. */
    private fun drawBracketMatch(canvas: Canvas, lineStart: Long, textLeft: Float, top: Float) {
        if (bracketSelf < 0 || bracketPartner < 0) return
        val lineEnd = lineStart + layout.byteLength
        for (offset in longArrayOf(bracketSelf, bracketPartner)) {
            if (offset < lineStart || offset >= lineEnd) continue
            val column = layout.columnOfByte((offset - lineStart).toInt())
            val x = textLeft + column * advance
            if (x + advance < gutterWidth || x > width) continue
            fillPaint.color = colors.bracketMatchBg
            canvas.drawRect(x, top, x + advance, top + lineHeight, fillPaint)
            strokePaint.color = colors.bracketMatchBorder
            canvas.drawRect(x, top + 0.5f, x + advance, top + lineHeight - 0.5f, strokePaint)
        }
    }

    private fun drawGutter(canvas: Canvas, lineNumber: Int, caretLine: Int, top: Float) {
        fillPaint.color = colors.editorBg
        canvas.drawRect(0f, top, gutterWidth, top + lineHeight, fillPaint)
        val current = lineNumber == caretLine
        if (current) {
            fillPaint.color = colors.lineHighlight
            canvas.drawRect(0f, top, gutterWidth, top + lineHeight, fillPaint)
        }
        gutterPaint.color = if (current) colors.lineNumberActive else colors.lineNumber
        val label = (lineNumber + 1).toString()
        val labelWidth = gutterPaint.measureText(label)
        canvas.drawText(
            label,
            gutterWidth - gutterPadding - labelWidth,
            top + baselineOffset,
            gutterPaint,
        )
        strokePaint.color = colors.border
        canvas.drawLine(gutterWidth - 0.5f, top, gutterWidth - 0.5f, top + lineHeight, strokePaint)
    }

    private fun drawSelectionHandles(
        canvas: Canvas,
        document: CopeDocument,
        viewport: Viewport,
        textLeft: Float,
    ) {
        if (!hasSelection) return
        fillPaint.color = colors.accent
        for (offset in longArrayOf(selectionStart, selectionEnd)) {
            val position = document.lineColumnOf(offset)
            val line = position[0].toInt()
            if (!viewport.containsLine(line)) continue
            val index = line - viewport.firstLine
            layout.layout(
                viewport.text,
                viewport.lineTextOffset[index],
                viewport.lineTextLength[index],
                tabWidth,
            )
            val x = textLeft + layout.columnOfByte(position[1].toInt()) * advance
            val y = line * lineHeight - scrollYPixels + lineHeight
            canvas.drawCircle(x, y + handleRadius(), handleRadius(), fillPaint)
            canvas.drawRect(x - 1f, y - lineHeight, x + 1f, y, fillPaint)
        }
    }

    private fun handleRadius(): Float = 7f * resources.displayMetrics.density

    private fun drawScrollbar(canvas: Canvas) {
        val lines = document?.lineCount ?: return
        val maxScroll = maxScrollY()
        if (maxScroll <= 0f || lines <= 0) return
        val trackHeight = height.toFloat()
        val thumbHeight = max(
            24f * resources.displayMetrics.density,
            trackHeight * (trackHeight / (trackHeight + maxScroll)),
        )
        val progress = (scrollYPixels / maxScroll).coerceIn(0f, 1f)
        val top = (trackHeight - thumbHeight) * progress
        val barWidth = 3f * resources.displayMetrics.density
        fillPaint.color = colors.scrollbar
        canvas.drawRect(width - barWidth, top, width.toFloat(), top + thumbHeight, fillPaint)
    }

    private fun paletteFg(styleId: Int): Int {
        val base = styleId * 3
        return if (base + 2 < palette.size) palette[base] else colors.editorFg
    }

    private fun paletteBg(styleId: Int): Int {
        val base = styleId * 3
        return if (base + 2 < palette.size) palette[base + 1] else colors.editorBg
    }

    private fun paletteFlags(styleId: Int): Int {
        val base = styleId * 3
        return if (base + 2 < palette.size) palette[base + 2] else 0
    }

    private fun applyStyle(styleId: Int) {
        val flags = paletteFlags(styleId)
        textPaint.color = if ((flags and ThemeSnapshot.FLAG_HAS_FG) != 0) {
            paletteFg(styleId)
        } else {
            colors.editorFg
        }
        val bold = (flags and ThemeSnapshot.FLAG_BOLD) != 0
        val italic = (flags and ThemeSnapshot.FLAG_ITALIC) != 0
        textPaint.typeface = when {
            bold && italic -> fonts.boldItalic
            bold -> fonts.bold
            italic -> fonts.italic
            else -> fonts.regular
        }
    }

    // --- input --------------------------------------------------------------

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_MULTI_LINE or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        // IME_ACTION_NONE plus NO_ENTER_ACTION: Enter must insert a newline, never
        // "submit". NO_FULLSCREEN keeps the IME from taking the whole screen in
        // landscape, which would hide the editor entirely.
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or
            EditorInfo.IME_FLAG_NO_ENTER_ACTION or
            EditorInfo.IME_FLAG_NO_FULLSCREEN
        outAttrs.initialSelStart = 0
        outAttrs.initialSelEnd = 0
        return CopeInputConnection(this)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) {
            dragMode = DragMode.NONE
            return true
        }
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            dragMode = dragModeFor(event.x, event.y)
        } else if (event.actionMasked == MotionEvent.ACTION_UP ||
            event.actionMasked == MotionEvent.ACTION_CANCEL
        ) {
            dragMode = DragMode.NONE
        }
        return gestures.onTouchEvent(event) || super.onTouchEvent(event)
    }

    /**
     * Decides whether a drag scrolls, drags the caret, or drags a selection
     * handle. Touching within a handle's radius wins; touching within one cell of
     * the caret drags the caret (the "nudge" affordance — a 4px native handle is
     * not usable and this project does not rely on one).
     */
    private fun dragModeFor(x: Float, y: Float): DragMode {
        val document = this.document ?: return DragMode.NONE
        if (hasSelection) {
            if (nearOffset(document, selectionStart, x, y)) return DragMode.SELECT_START
            if (nearOffset(document, selectionEnd, x, y)) return DragMode.SELECT_END
            return DragMode.NONE
        }
        if (nearOffset(document, caret, x, y, radius = advance * 1.6f)) return DragMode.CARET
        return DragMode.NONE
    }

    private fun nearOffset(
        document: CopeDocument,
        offset: Long,
        x: Float,
        y: Float,
        radius: Float = handleTouchSlop,
    ): Boolean {
        val viewport = cachedViewport ?: return false
        val position = document.lineColumnOf(offset)
        val line = position[0].toInt()
        if (!viewport.containsLine(line)) return false
        val index = line - viewport.firstLine
        layout.layout(viewport.text, viewport.lineTextOffset[index], viewport.lineTextLength[index], tabWidth)
        val px = gutterWidth - scrollX + layout.columnOfByte(position[1].toInt()) * advance
        val py = line * lineHeight - scrollYPixels + lineHeight
        return abs(x - px) < radius && y > py - lineHeight - radius && y < py + radius * 2f
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (handleKey(keyCode, event)) return true
        return super.onKeyDown(keyCode, event)
    }

    /** Shared by the hardware keyboard and the smart key row. */
    public fun handleKey(keyCode: Int, event: KeyEvent?): Boolean {
        val document = this.document ?: return false
        when (keyCode) {
            KeyEvent.KEYCODE_DEL -> {
                deleteBackward()
                return true
            }
            KeyEvent.KEYCODE_FORWARD_DEL -> {
                if (hasSelection) {
                    deleteSelection()
                } else {
                    val to = document.nextCodepoint(caret)
                    if (to > caret) applyErase(caret, to - caret)
                }
                return true
            }
            KeyEvent.KEYCODE_ENTER -> {
                insertText("\n")
                return true
            }
            KeyEvent.KEYCODE_TAB -> {
                insertText(" ".repeat(tabWidth))
                return true
            }
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                moveCaret(-1, event?.isShiftPressed == true)
                return true
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                moveCaret(1, event?.isShiftPressed == true)
                return true
            }
            KeyEvent.KEYCODE_DPAD_UP -> {
                moveLine(-1, event?.isShiftPressed == true)
                return true
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                moveLine(1, event?.isShiftPressed == true)
                return true
            }
            KeyEvent.KEYCODE_MOVE_HOME -> {
                setCaret(document.offsetOf(currentLine(), 0), event?.isShiftPressed == true)
                return true
            }
            KeyEvent.KEYCODE_MOVE_END -> {
                setCaret(endOfLine(currentLine()), event?.isShiftPressed == true)
                return true
            }
        }
        val unicode = event?.unicodeChar ?: 0
        if (unicode != 0 && event != null && !event.isCtrlPressed) {
            insertText(String(Character.toChars(unicode)))
            return true
        }
        return false
    }

    // --- editing operations -------------------------------------------------

    public fun insertText(text: String) {
        val document = this.document ?: return
        if (text.isEmpty()) return
        clearComposing()
        if (hasSelection) {
            val start = selectionStart
            val length = selectionEnd - start
            val after = document.replace(start, length, text)
            afterEdit(if (after >= 0) after else start)
        } else {
            val after = document.insert(caret, text)
            afterEdit(if (after >= 0) after else caret)
        }
    }

    public fun deleteBackward() {
        val document = this.document ?: return
        if (hasSelection) {
            deleteSelection()
            return
        }
        if (caret <= 0) return
        val from = document.prevCodepoint(caret)
        applyErase(from, caret - from)
    }

    public fun deleteSelection() {
        val document = this.document ?: return
        if (!hasSelection) return
        val start = selectionStart
        applyErase(start, selectionEnd - start)
    }

    internal fun applyErase(offset: Long, length: Long) {
        val document = this.document ?: return
        if (length <= 0) return
        clearComposing()
        document.erase(offset, length)
        afterEdit(offset)
    }

    internal fun applyReplace(offset: Long, length: Long, text: String): Long {
        val document = this.document ?: return caret
        val after = document.replace(offset, length, text)
        return if (after >= 0) after else offset
    }

    internal fun afterEdit(newCaret: Long) {
        lastEditMs = System.currentTimeMillis()
        caretVisible = true
        caret = newCaret.coerceAtLeast(0)
        anchor = caret
        cachedVersion = -1L
        updateGutterWidth()
        refreshDerivedHighlights()
        ensureCaretVisible()
        callbacks?.onSelectionChanged(false)
        callbacks?.onEditorStateChanged()
        invalidate()
    }

    internal fun clearComposing() {
        composingStart = -1
        composingEnd = -1
    }

    public fun undo() {
        val document = this.document ?: return
        val at = document.undo()
        if (at >= 0) afterEdit(at)
    }

    public fun redo() {
        val document = this.document ?: return
        val at = document.redo()
        if (at >= 0) afterEdit(at)
    }

    // --- caret / selection --------------------------------------------------

    public fun setCaret(offset: Long, extend: Boolean) {
        val document = this.document ?: return
        val clamped = offset.coerceIn(0, document.byteSize)
        caret = clamped
        if (!extend) anchor = clamped
        caretVisible = true
        lastEditMs = System.currentTimeMillis()
        refreshDerivedHighlights()
        ensureCaretVisible()
        callbacks?.onSelectionChanged(hasSelection)
        callbacks?.onEditorStateChanged()
        invalidate()
    }

    public fun setSelection(start: Long, end: Long) {
        val document = this.document ?: return
        anchor = start.coerceIn(0, document.byteSize)
        caret = end.coerceIn(0, document.byteSize)
        refreshDerivedHighlights()
        callbacks?.onSelectionChanged(hasSelection)
        callbacks?.onEditorStateChanged()
        invalidate()
    }

    public fun selectAll() {
        val document = this.document ?: return
        setSelection(0, document.byteSize)
    }

    public fun currentLine(): Int = document?.lineColumnOf(caret)?.get(0)?.toInt() ?: 0

    private fun endOfLine(line: Int): Long {
        val document = this.document ?: return 0
        // A column past any possible line length clamps to the content end, which
        // is exactly "end of line" without needing the line's length here.
        return document.offsetOf(line, Int.MAX_VALUE)
    }

    public fun moveCaret(direction: Int, extend: Boolean) {
        val document = this.document ?: return
        val to = if (direction > 0) document.nextCodepoint(caret) else document.prevCodepoint(caret)
        setCaret(to, extend)
    }

    public fun moveLine(delta: Int, extend: Boolean) {
        val document = this.document ?: return
        val position = document.lineColumnOf(caret)
        val line = (position[0].toInt() + delta).coerceIn(0, document.lineCount - 1)
        val column = document.let {
            // Keep the visual column, not the byte column: moving down a line with
            // a tab in it must not jump sideways.
            val current = position[0].toInt()
            val bytes = it.lineBytes(current)
            layout.layout(bytes, 0, bytes.size, tabWidth)
            layout.columnOfByte(position[1].toInt())
        }
        setCaret(document.offsetOfDisplayColumn(line, column, tabWidth), extend)
    }

    public fun selectWordAt(offset: Long) {
        val document = this.document ?: return
        val position = document.lineColumnOf(offset)
        val line = position[0].toInt()
        val bytes = document.lineBytes(line)
        val lineStart = document.offsetOf(line, 0)
        var start = position[1].toInt().coerceIn(0, bytes.size)
        var end = start
        fun isWord(b: Byte): Boolean {
            val v = b.toInt() and 0xFF
            return (v >= '0'.code && v <= '9'.code) ||
                (v >= 'A'.code && v <= 'Z'.code) ||
                (v >= 'a'.code && v <= 'z'.code) ||
                v == '_'.code || v >= 0x80
        }
        while (start > 0 && isWord(bytes[start - 1])) start--
        while (end < bytes.size && isWord(bytes[end])) end++
        if (start == end) {
            setCaret(offset, extend = false)
            return
        }
        setSelection(lineStart + start, lineStart + end)
    }

    /** Selects a whole line including its terminator, the gutter long-press action. */
    public fun selectLineAt(offset: Long) {
        val document = this.document ?: return
        val line = document.lineColumnOf(offset)[0].toInt()
        val start = document.offsetOf(line, 0)
        val end = if (line + 1 < document.lineCount) {
            document.offsetOf(line + 1, 0)
        } else {
            document.byteSize
        }
        setSelection(start, end)
    }

    public fun ensureCaretVisible() {
        val document = this.document ?: return
        val position = document.lineColumnOf(caret)
        val line = position[0].toInt()
        val top = line * lineHeight
        val bottom = top + lineHeight
        if (top < scrollYPixels) {
            scrollYPixels = top
        } else if (bottom > scrollYPixels + height) {
            scrollYPixels = bottom - height
        }
        scrollYPixels = scrollYPixels.coerceIn(0f, maxScrollY())
    }

    /** Per-tab state, saved on switch: caret, anchor, vertical and horizontal scroll. */
    public fun captureState(): LongArray =
        longArrayOf(caret, anchor, scrollYPixels.toLong(), scrollX.toLong())

    public fun restoreState(state: LongArray) {
        if (state.size < 4) return
        val document = this.document ?: return
        caret = state[0].coerceIn(0, document.byteSize)
        anchor = state[1].coerceIn(0, document.byteSize)
        scrollYPixels = state[2].toFloat().coerceAtLeast(0f)
        scrollX = state[3].toFloat().coerceAtLeast(0f)
        cachedVersion = -1L
        refreshDerivedHighlights()
        invalidate()
    }

    /** Line the viewport starts at; the status strip and the sheet show it. */
    public fun firstVisibleLine(): Int = (scrollYPixels / lineHeight).toInt()

    public fun scrollToLine(line: Int) {
        val document = this.document ?: return
        val target = line.coerceIn(0, document.lineCount - 1) * lineHeight
        scrollYPixels = target.coerceIn(0f, maxScrollY())
        invalidate()
    }

    private fun scrollBy(dx: Float, dy: Float) {
        scrollYPixels = (scrollYPixels + dy).coerceIn(0f, maxScrollY())
        scrollX = (scrollX + dx).coerceIn(0f, maxScrollX())
        callbacks?.onEditorStateChanged()
        invalidate()
    }

    // --- IME helpers used by CopeInputConnection -----------------------------

    internal fun showKeyboard() {
        val manager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        manager?.showSoftInput(this, 0)
    }

    internal fun hideKeyboard() {
        val manager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        manager?.hideSoftInputFromWindow(windowToken, 0)
    }

    internal fun textBefore(offset: Long, maxChars: Int): String {
        val document = this.document ?: return ""
        if (offset <= 0 || maxChars <= 0) return ""
        // Read up to 4 bytes per requested char, then trim: UTF-8 is at most 4
        // bytes per codepoint, so this can never read too little.
        val want = min(offset, maxChars.toLong() * 4L)
        val text = document.textRangeString(offset - want, want)
        return if (text.length <= maxChars) text else text.substring(text.length - maxChars)
    }

    internal fun textAfter(offset: Long, maxChars: Int): String {
        val document = this.document ?: return ""
        if (maxChars <= 0) return ""
        val want = min(document.byteSize - offset, maxChars.toLong() * 4L)
        if (want <= 0) return ""
        val text = document.textRangeString(offset, want)
        return if (text.length <= maxChars) text else text.substring(0, maxChars)
    }

    internal fun invalidateAfterImeEdit(newCaret: Long) {
        lastEditMs = System.currentTimeMillis()
        caretVisible = true
        caret = newCaret.coerceAtLeast(0)
        anchor = caret
        cachedVersion = -1L
        updateGutterWidth()
        refreshDerivedHighlights()
        ensureCaretVisible()
        callbacks?.onEditorStateChanged()
        invalidate()
    }

    private companion object {
        /** Bracket pairs, index-matched. Angle brackets are excluded on purpose:
         *  `a < b` in every C-family language would match a `>` pages away. */
        const val OPENERS = "([{"
        const val CLOSERS = ")]}"

        /**
         * How far a bracket search looks in each direction. 64 KiB is far past any
         * real function body and small enough that the window read is imperceptible.
         */
        const val BRACKET_SCAN_BYTES = 64L * 1024L

        /** A selection longer than this is a block, not a word to match. */
        const val OCCURRENCE_MAX_BYTES = 96L

        /** More boxes than this on screen is noise, and findAll caps anyway. */
        const val OCCURRENCE_CAP = 500

        fun isOpener(byte: Int): Boolean = OPENERS.indexOf(byte.toChar()) >= 0

        fun isCloser(byte: Int): Boolean = CLOSERS.indexOf(byte.toChar()) >= 0

        /** The matching bracket byte. Only called for a known bracket. */
        fun partnerOf(byte: Int): Int {
            val open = OPENERS.indexOf(byte.toChar())
            if (open >= 0) return CLOSERS[open].code
            val close = CLOSERS.indexOf(byte.toChar())
            return if (close >= 0) OPENERS[close].code else -1
        }
    }
}

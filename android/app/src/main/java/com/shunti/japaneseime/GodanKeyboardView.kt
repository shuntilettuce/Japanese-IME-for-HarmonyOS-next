package com.shunti.japaneseime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs

/**
 * GODAN のキーボード (ユーザー 2026-10-24。Gboard の GODAN と同じ考え方): 母音 5 つと子音を並べ、ローマ字で打つ。
 * 打った字は QWERTY と同じくローマ字として読みに入る (英字の打ち分け・ローマ字の打ち間違いの直しも同じ)。
 * 5 列 × 5 段、Gboard の GODAN と同じ並び: [↶ ◀ 記 あa1] [A I U E O] [K S T N ゛゜] [H M Y R W] [⌫ ▶ 空白 確定]。
 * 子音の上下左右に Q・G・数字・記号 (K: 上 Q 右 G 下 2 など)。settings.godanMirror で左右を逆に
 */
@SuppressLint("ViewConstructor")
class GodanKeyboardView(
    context: Context,
    private val h: InputHandler,
    private val tables: Tables,
    private val haptic: () -> Unit,
) : View(context) {

    var theme: Map<String, Int> = tables.LIGHT_THEME
        set(v) { field = v; invalidate() }
    var totalH = 0f
    var mirror = false
        set(v) { field = v; invalidate() }

    private val dp = resources.displayMetrics.density
    private val keyPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val big = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val hint = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val rect = RectF()
    private val timers = Handler(Looper.getMainLooper())
    private var repeating: Runnable? = null
    private val threshold get() = 22 * dp

    /** キー 1 つ: 表示・[タップ, 左, 上, 右, 下] の動作 (無い向きはタップ)・縦に何段ぶんか */
    private class Key(val label: String, val acts: List<String?>, val side: Boolean = false, val span: Int = 1, val accent: Boolean = false) {
        fun at(d: Int) = acts.getOrNull(d) ?: acts[0]!!
    }

    /** 字のキー: タップ・左・上・右・下 (r: ローマ字のキー、p: そのまま入れる字) */
    private fun k(label: String, tap: String, l: String? = null, u: String? = null, r: String? = null, d: String? = null) =
        Key(label, listOf(tap, l, u, r, d))
    private fun s(label: String, act: String, span: Int = 1, accent: Boolean = false) = Key(label, listOf(act), side = true, span = span, accent = accent)

    /** 列 (左から、反転前)。Gboard の GODAN と同じ並び (ユーザー 2026-10-24 の画面): 子音の上下左右に Q・G・数字などを置く */
    private fun columns(): List<List<Key>> {
        val enter = h.enterLabel()
        return listOf(
            listOf(s("↶", "undo"), s("◀", "left"), s("記", "symbol"), s("あa1", "mode", span = 2)),
            listOf(k("A", "r:a", d = "p:1"), k("I", "r:i", d = "p:4"), k("U", "r:u", d = "p:7"), k("E", "r:e"), k("O", "r:o")),
            listOf(k("K", "r:k", u = "r:q", r = "r:g", d = "p:2"), k("S", "r:s", u = "r:j", r = "r:z", d = "p:5"),
                k("T", "r:t", u = "r:c", r = "r:d", d = "p:8"), k("N", "r:n", l = "p::", r = "p:・", d = "p:0"),
                Key("゛゜\n大⇔小", listOf("variant"))),
            listOf(k("H", "r:h", l = "r:p", u = "r:f", r = "r:b", d = "p:3"), k("M", "r:m", l = "p:/", u = "r:l", r = "r:-", d = "p:6"),
                k("Y", "r:y", l = "p:（", u = "r:x", r = "p:）", d = "p:9"), k("R", "r:r", l = "p:。", u = "p:？", r = "p:！", d = "p:、"),
                k("W", "r:w", l = "p:「", u = "r:v", r = "p:」")),
            listOf(s("⌫", "bs"), s("▶", "right"), s(if (h.inputState == InputHandler.InputState.IDLE) "␣" else "変換", "space"),
                s(if (enter == "改行") "→" else enter, "enter", span = 2, accent = true)),
        )
    }

    // 左右反転は字の 3 列だけ (母音を右に)。両端の機能の列 (↶ ◀ 記 あa1 / ⌫ ▶ 空白 確定) はそのまま (ユーザー 2026-10-24)
    private fun layout(): List<List<Key>> = columns().let { if (mirror) listOf(it[0]) + it.subList(1, 4).reversed() + listOf(it[4]) else it }

    /** 列 c の段 r にあるキー (縦に何段ぶんかを数える)。返り値 (キー, 上の段, 段数) */
    private fun keyAt(col: List<Key>, row: Int): Triple<Key, Int, Int>? {
        var r0 = 0
        for (key in col) {
            if (row < r0 + key.span) return Triple(key, r0, key.span)
            r0 += key.span
        }
        return null
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), totalH.toInt())
    }

    private fun colW() = width / 5f
    private fun rowH() = (height - 4 * dp) / 5f

    private var downX = 0f
    private var downY = 0f
    private var activeKey: Key? = null
    private var dirNow = 0

    override fun onDraw(canvas: Canvas) {
        keyPaint.color = theme.getValue("panelBg")
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), keyPaint)
        big.color = theme.getValue("textPrimary")
        hint.color = theme.getValue("textPrimary").let { (it and 0x00FFFFFF) or 0x99000000.toInt() }
        big.textSize = 20 * dp
        hint.textSize = 10 * dp
        val cols = layout()
        val w = colW()
        val rh = rowH()
        for ((c, col) in cols.withIndex()) {
            var r = 0
            for (key in col) {
                rect.set(c * w + 3 * dp, 2 * dp + r * rh + 3 * dp, (c + 1) * w - 3 * dp, 2 * dp + (r + key.span) * rh - 3 * dp)
                val active = activeKey === key
                keyPaint.color = theme.getValue(when {
                    active -> if (key.side) "funcBg" else "accentSoft"
                    key.accent && h.inputState != InputHandler.InputState.IDLE -> "accent"
                    key.side -> "specialBg"
                    else -> "keyBg"
                })
                canvas.drawRoundRect(rect, 7 * dp, 7 * dp, keyPaint)
                val text = if (active && dirNow != 0) label(key.at(dirNow)) else key.label
                val lines = text.split("\n")
                big.textSize = (if (lines[0].length > 2) 13 else 21) * dp
                if (lines.size == 1) canvas.drawText(text, rect.centerX(), rect.centerY() - (big.descent() + big.ascent()) / 2, big)
                else {
                    canvas.drawText(lines[0], rect.centerX(), rect.centerY() - 2 * dp, big)
                    canvas.drawText(lines[1], rect.centerX(), rect.centerY() + 12 * dp, hint)
                }
                // 上下左右の字 (小さく)
                key.acts.getOrNull(1)?.let { canvas.drawText(label(it), rect.left + 12 * dp, rect.centerY() + 4 * dp, hint) }
                key.acts.getOrNull(2)?.let { canvas.drawText(label(it), rect.centerX(), rect.top + 11 * dp, hint) }
                key.acts.getOrNull(3)?.let { canvas.drawText(label(it), rect.right - 12 * dp, rect.centerY() + 4 * dp, hint) }
                key.acts.getOrNull(4)?.let { canvas.drawText(label(it), rect.centerX(), rect.bottom - 3 * dp, hint) }
                r += key.span
            }
        }
    }

    private fun label(act: String) = when {
        act == "r:nn" -> "ん"
        act == "r:-" -> "ー"
        act.startsWith("r:") -> act.substring(2).uppercase()
        act.startsWith("p:") -> act.substring(2)
        else -> act
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val c = (e.x / colW()).toInt().coerceIn(0, 4)
                val r = ((e.y - 2 * dp) / rowH()).toInt().coerceIn(0, 4)
                activeKey = keyAt(layout()[c], r)?.first
                downX = e.x
                downY = e.y
                dirNow = 0
                haptic()
                when (activeKey?.acts?.get(0)) {   // 押し続けると繰り返す
                    "bs" -> startRepeat { h.handleBackspace() }
                    "left" -> startRepeat { h.handleCursorLeft() }
                    "right" -> startRepeat { h.handleCursorRight() }
                }
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val d = dir(e.x - downX, e.y - downY)
                if (d != dirNow) { dirNow = d; invalidate() }
            }
            MotionEvent.ACTION_UP -> {
                stopRepeat()
                val key = activeKey
                activeKey = null
                if (key != null && key.acts[0] !in listOf("bs", "left", "right")) fire(key.at(dir(e.x - downX, e.y - downY)))
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> {
                stopRepeat()
                activeKey = null
                invalidate()
            }
        }
        return true
    }

    private fun fire(act: String) {
        when {
            act.startsWith("r:") -> act.substring(2).forEach { h.handleKeyPress(it.toString()) }
            act.startsWith("p:") -> {
                val p = act.substring(2)
                if (h.inputState == InputHandler.InputState.IDLE) h.insertText(p) else h.handleDirectKana(p)
            }
            act == "space" -> h.handleSpaceKey()
            act == "enter" -> h.handleEnterKey()
            act == "mode" -> h.cycleInputMode()
            act == "undo" -> h.undoLastDeletion()
            act == "variant" -> h.handleVariantCycle()
            act == "symbol" -> {
                h.commitTopCandidate()
                h.subMode = InputHandler.SubMode.SYMBOL
                h.updateView()
            }
        }
    }

    private fun dir(dx: Float, dy: Float): Int {
        if (abs(dx) < threshold && abs(dy) < threshold) return 0
        return if (abs(dx) >= abs(dy)) (if (dx > 0) 3 else 1) else (if (dy < 0) 2 else 4)
    }

    private fun startRepeat(action: () -> Unit) {
        action()
        val r = object : Runnable {
            override fun run() {
                action()
                timers.postDelayed(this, 90)
            }
        }
        repeating = r
        timers.postDelayed(r, 450)
    }

    private fun stopRepeat() {
        repeating?.let { timers.removeCallbacks(it) }
        repeating = null
    }
}

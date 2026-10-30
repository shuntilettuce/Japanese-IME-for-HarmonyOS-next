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

/**
 * QWERTY のキーボード (HarmonyOS 版 components/KeyboardView.ets)。かなモードではローマ字で打つ。
 * 5 段: 数字 / q〜p / a〜l (かなでは最後に ー) / ⇧ z〜m ⌫ / [あA1] [記号] [、] [空白] [。] [確定]。
 * 配置の表は Tables (HarmonyOS 版から生成)。描画も当たり判定も自前。
 */
@SuppressLint("ViewConstructor")
class QwertyKeyboardView(
    context: Context,
    private val h: InputHandler,
    private val tables: Tables,
    private val haptic: () -> Unit,
    private val special: (String) -> Unit,   // 長押しの操作 ("onehand-left" / "onehand-right")
) : View(context) {

    var theme: Map<String, Int> = tables.LIGHT_THEME
        set(v) { field = v; invalidate() }
    var totalH = 0f

    private val dp = resources.displayMetrics.density
    private val keyPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val rect = RectF()

    /** キー 1 つ: 表示・動作・幅 (1 = 普通のキー) */
    private class Key(val label: String, val action: String, val w: Float = 1f)

    private var shift = 0   // 0 なし, 1 次の 1 字だけ大文字, 2 ずっと大文字
    private var pressed: Key? = null
    private var laidOut: List<Pair<Key, RectF>> = emptyList()
    private val timers = Handler(Looper.getMainLooper())
    private var repeating: Runnable? = null

    private val kana get() = h.inputMode == InputHandler.InputMode.HIRAGANA

    private fun rows(): List<List<Key>> {
        val r = tables.QWERTY_ROWS
        val row1 = if (kana) r[1] else tables.QWERTY_ROW1_ALPHANUMERIC
        fun letters(l: List<String>) = l.map { Key(it, "char:$it") }
        val modeLabel = if (kana) "あA" else "Aあ"
        val space = when {
            !kana -> "space"
            h.inputState == InputHandler.InputState.IDLE -> "空白"
            else -> "変換"
        }
        return listOf(
            tables.QWERTY_NUMBER_ROW.map { Key(it, "digit:$it") },
            letters(r[0]),
            letters(row1),
            listOf(Key(if (shift == 2) "⇪" else "⇧", "shift", 1.5f)) + letters(r[2]) + Key("⌫", "del", 1.5f),
            // ◄/► はスペースの右に並べる (長押しで続けて動く)。幅の合計は 10
            listOf(
                Key(modeLabel, "mode", 1.3f), Key("記号", "symbol", 1.1f), Key(if (kana) "、" else ",", "punct:" + if (kana) "、" else ",", 0.9f),
                Key(space, "space", 2.6f), Key("◀", "left", 0.9f), Key("▶", "right", 0.9f),
                Key(if (kana) "。" else ".", "punct:" + if (kana) "。" else ".", 0.9f),
                Key(h.enterLabel(), "enter", 1.4f),
            ),
        )
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), totalH.toInt())
    }

    override fun onDraw(canvas: Canvas) {
        keyPaint.color = theme.getValue("panelBg")
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), keyPaint)
        val rs = rows()
        val rowH = (height - 8 * dp) / rs.size
        val unit = width / 10f
        val out = ArrayList<Pair<Key, RectF>>()
        for ((ri, row) in rs.withIndex()) {
            val total = row.sumOf { it.w.toDouble() }.toFloat()
            var x = (width - total * unit) / 2   // 9 字の段は真ん中に寄せる
            val top = 4 * dp + ri * rowH
            for (k in row) {
                val r = RectF(x, top, x + k.w * unit, top + rowH)
                out.add(k to r)
                rect.set(r.left + 2.5f * dp, r.top + 3 * dp, r.right - 2.5f * dp, r.bottom - 3 * dp)
                val special = !k.action.startsWith("char:") && !k.action.startsWith("digit:") && k.action != "space"
                keyPaint.color = theme.getValue(
                    when {
                        k === pressed -> if (special) "funcBg" else "accentSoft"
                        k.action == "enter" && h.inputState != InputHandler.InputState.IDLE -> "accent"
                        k.action == "shift" && shift > 0 -> "accent"
                        special -> "specialBg"
                        else -> "keyBg"
                    },
                )
                canvas.drawRoundRect(rect, 7 * dp, 7 * dp, keyPaint)
                val onAccent = (k.action == "enter" && h.inputState != InputHandler.InputState.IDLE) || (k.action == "shift" && shift > 0)
                text.color = theme.getValue(if (onAccent) "onAccent" else "textPrimary")
                val label = if (k.action.startsWith("char:") && shift > 0) k.label.uppercase() else k.label
                text.textSize = (if (label.length > 1) 13 else if (ri == 0) 16 else 19) * dp
                canvas.drawText(label, rect.centerX(), rect.centerY() - (text.descent() + text.ascent()) / 2, text)
                x += k.w * unit
            }
        }
        laidOut = out
    }

    private fun keyAt(x: Float, y: Float) = laidOut.firstOrNull { it.second.contains(x, y) }?.first

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressed = keyAt(e.x, e.y)
                haptic()
                longFired = false
                when (pressed?.action) {
                    "del" -> startRepeat { h.handleBackspace() }
                    "left" -> startRepeat { h.handleCursorLeft() }
                    "right" -> startRepeat { h.handleCursorRight() }
                    "symbol" -> armLong("onehand-left")    // 長押しで片手 (左)。もう一度で元に戻る
                    "space" -> armLong("onehand-right")    // 長押しで片手 (右)
                }
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                // 押したまま隣のキーへずらしたら、離した所のキーにする (打ち間違いを直せる)
                val k = keyAt(e.x, e.y)
                if (k != null && k !== pressed && pressed?.action !in setOf("del", "left", "right")) {
                    cancelLong()
                    pressed = k
                    invalidate()
                }
            }
            MotionEvent.ACTION_UP -> {
                stopRepeat()
                cancelLong()
                if (!longFired) pressed?.let { press(it) }
                pressed = null
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> {
                stopRepeat()
                cancelLong()
                pressed = null
                invalidate()
            }
        }
        return true
    }

    private fun press(k: Key) {
        val a = k.action
        when {
            a.startsWith("char:") -> {
                val c = a.substring(5)
                h.handleKeyPress(if (shift > 0) c.uppercase() else c)   // かなでも Shift で大文字 (そこから英字のまま、PC と同じ)
                if (shift == 1) shift = 0
            }
            a.startsWith("digit:") || a.startsWith("punct:") -> {
                val c = a.substringAfter(':')
                when {
                    !kana -> h.handleKeyPress(c)
                    h.inputState == InputHandler.InputState.IDLE -> h.insertText(c)
                    else -> h.handleDirectKana(c)
                }
            }
            a == "shift" -> shift = (shift + 1) % 3
            a == "space" -> h.handleSpaceKey()
            a == "enter" -> h.handleEnterKey()
            a == "mode" -> h.cycleInputMode()
            a == "symbol" -> {
                h.commitTopCandidate()
                h.subMode = InputHandler.SubMode.SYMBOL
                h.updateView()
            }
        }
    }

    private var longFired = false
    private var longPress: Runnable? = null

    private fun armLong(action: String) {
        val r = Runnable {
            longFired = true
            haptic()
            special(action)
        }
        longPress = r
        timers.postDelayed(r, KeyboardView.LONG_MS)
    }

    private fun cancelLong() {
        longPress?.let { timers.removeCallbacks(it) }
        longPress = null
    }

    private fun startRepeat(action: () -> Unit) {
        action()
        val r = object : Runnable {
            override fun run() {
                action()
                timers.postDelayed(this, 100)
            }
        }
        repeating = r
        timers.postDelayed(r, 500)
    }

    private fun stopRepeat() {
        repeating?.let { timers.removeCallbacks(it) }
        repeating = null
    }
}

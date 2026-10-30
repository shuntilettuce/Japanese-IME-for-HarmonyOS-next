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
 * フリックのキーボード (HarmonyOS 版の FlickKeyboardView / AlphabetFlickKeyboardView / NumericPadView を 1 つに)。
 * 5 列 × 4 段: 左の列 [取消, ◀, 記号, あA]、中央の 3 列はフリックのキー (表は Tables)、右の列 [⌫, ▶, 空白, 確定]。
 * 描画も当たり判定も自前 (部品を使わない。軽さ優先)。
 */
@SuppressLint("ViewConstructor")
class KeyboardView(
    context: Context,
    private val h: InputHandler,
    private val tables: Tables,
    private val haptic: () -> Unit,
    private val special: (String) -> Unit,   // 長押しの操作 ("onehand-left" / "onehand-right")
) : View(context) {

    var theme: Map<String, Int> = tables.LIGHT_THEME
        set(v) { field = v; applyTheme(); invalidate() }

    private val dp = resources.displayMetrics.density
    private val threshold = 20 * dp
    var rowH = 56 * dp
    var hints = true   // フリックの上下左右の小さい字を出すか (設定)

    private val bg = Paint()
    private val keyPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val big = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val small = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val hint = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = JpFont.get(context) }
    private val rect = RectF()

    // 押しているキー (行, 列)。列は 0..4 (0 と 4 が左右の機能キー)
    private var activeRow = -1
    private var activeCol = -1
    private var startX = 0f
    private var startY = 0f
    private var flickDir = 0   // 0 タップ, 1 左, 2 上, 3 右, 4 下
    private var lastAlpha = ""  // 英字: 最後に打った小文字 (a/A キーで大文字にする)

    private val timers = Handler(Looper.getMainLooper())
    private var repeating: Runnable? = null

    init {
        applyTheme()
    }

    private fun applyTheme() {
        bg.color = theme.getValue("panelBg")
        big.color = theme.getValue("textPrimary")
        small.color = theme.getValue("textPrimary")
        big.textSize = 22 * dp
        small.textSize = 13 * dp
        hint.textSize = 10 * dp
    }

    private fun grid(): List<List<Tables.FlickKey>> = when {
        h.subMode == InputHandler.SubMode.NUMERIC -> tables.NUM_FLICK_GRID
        h.inputMode == InputHandler.InputMode.ALPHANUMERIC -> tables.ALPHA_GRID
        else -> tables.FLICK_GRID
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), (rowH * 4 + 8 * dp).toInt())
    }

    private fun colW() = width / 5f

    // ---------------------------------------------------------------- 表示
    private fun sideLabel(row: Int, col: Int): String = if (col == 0) when (row) {
        0 -> "取消"
        1 -> "◀"
        2 -> "記号"
        else -> when {
            // 数字パッドに回さない設定なら 1 は書かない (ユーザー 2026-10-14)
            h.subMode == InputHandler.SubMode.NUMERIC -> "1あA"
            h.inputMode == InputHandler.InputMode.ALPHANUMERIC -> if (h.settings.numericPad) "A1あ" else "Aあ"
            else -> if (h.settings.numericPad) "あA1" else "あA"
        }
    } else when (row) {
        0 -> "⌫"
        1 -> "▶"
        2 -> if (h.inputState == InputHandler.InputState.IDLE) "空白" else "変換"
        else -> h.enterLabel()
    }

    /** 英数モードの句読点キーは半角 (HarmonyOS 版 jpToAsciiPunct) */
    private fun punct(s: String) = if (h.inputMode == InputHandler.InputMode.ALPHANUMERIC) ASCII_PUNCT[s] ?: s else s

    private fun keyText(key: Tables.FlickKey, dir: Int): String = when (key.special) {
        "punct" -> punct(key.at(dir))
        "case" -> "a/A"
        else -> key.at(dir)
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bg)
        val g = grid()
        val w = colW()
        for (r in 0 until 4) for (c in 0 until 5) {
            rect.set(c * w + 3 * dp, 4 * dp + r * rowH + 3 * dp, (c + 1) * w - 3 * dp, 4 * dp + (r + 1) * rowH - 3 * dp)
            val active = r == activeRow && c == activeCol
            val side = c == 0 || c == 4
            val key = if (side) null else g.getOrNull(r)?.getOrNull(c - 1)
            val special = side || key?.special == "dakuten" || key?.special == "case"
            keyPaint.color = theme.getValue(
                when {
                    active && !side -> "accentSoft"
                    active -> "funcBg"
                    c == 4 && r == 3 && h.inputState != InputHandler.InputState.IDLE -> "accent"
                    special -> "specialBg"
                    else -> "keyBg"
                },
            )
            canvas.drawRoundRect(rect, 8 * dp, 8 * dp, keyPaint)
            val cx = rect.centerX()
            val cy = rect.centerY()
            if (key == null) {
                val s = sideLabel(r, c)
                val p = if (s.length > 1 && s != "◀" && s != "▶") small else big
                p.color = theme.getValue(if (c == 4 && r == 3 && h.inputState != InputHandler.InputState.IDLE) "onAccent" else "textPrimary")
                canvas.drawText(s, cx, cy - (p.descent() + p.ascent()) / 2, p)
                continue
            }
            // 中央の字 (押している間はフリックの向きの字)
            val center = keyText(key, if (active) flickDir else 0)
            val p = if (center.length > 1) small else big
            p.color = theme.getValue("textPrimary")
            canvas.drawText(center, cx, cy - (p.descent() + p.ascent()) / 2, p)
            // 上下左右の小さいヒント
            if (key.special == "case" || (!hints && !active)) continue
            val dx = rect.width() * 0.34f
            val dy = rect.height() * 0.32f
            for (d in 1..4) {
                val s = when (d) {
                    1 -> key.left
                    2 -> key.up
                    3 -> key.right
                    else -> key.down
                } ?: continue
                hint.color = theme.getValue(if (active && flickDir == d) "accentStrong" else "textSecondary")
                val x = cx + when (d) { 1 -> -dx; 3 -> dx; else -> 0f }
                val y = cy + when (d) { 2 -> -dy; 4 -> dy; else -> 0f }
                canvas.drawText(if (key.special == "punct") punct(s) else s, x, y - (hint.descent() + hint.ascent()) / 2, hint)
            }
        }
    }

    // ---------------------------------------------------------------- 操作
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                activeRow = ((e.y - 4 * dp) / rowH).toInt().coerceIn(0, 3)
                activeCol = (e.x / colW()).toInt().coerceIn(0, 4)
                startX = e.x
                startY = e.y
                flickDir = 0
                haptic()
                onDown(activeRow, activeCol)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val d = dir(e.x - startX, e.y - startY)
                if (d != 0) cancelLong()   // 指をずらしたら長押しではない
                if (d != flickDir) {
                    flickDir = d
                    invalidate()
                }
            }
            MotionEvent.ACTION_UP -> {
                stopRepeat()
                cancelLong()
                if (activeRow >= 0 && !longFired) onUp(activeRow, activeCol, dir(e.x - startX, e.y - startY))
                activeRow = -1
                activeCol = -1
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> {
                stopRepeat()
                cancelLong()
                activeRow = -1
                activeCol = -1
                invalidate()
            }
        }
        return true
    }

    // 長押し: 記号 → 片手 (左)、空白 → 片手 (右)。もう一度で元に戻る (HarmonyOS 版と同じキー)
    private var longFired = false
    private var longPress: Runnable? = null

    private fun armLong(action: String) {
        val r = Runnable {
            longFired = true
            haptic()
            special(action)
        }
        longPress = r
        timers.postDelayed(r, LONG_MS)
    }

    private fun cancelLong() {
        longPress?.let { timers.removeCallbacks(it) }
        longPress = null
    }

    /** 押した瞬間に動くキー: ⌫ と ◀ ▶ (押し続けると繰り返す)。記号・空白は長押しを待つ */
    private fun onDown(r: Int, c: Int) {
        longFired = false
        when {
            c == 0 && r == 2 -> armLong("onehand-left")
            c == 4 && r == 2 -> armLong("onehand-right")
        }
        when {
            c == 4 && r == 0 -> startRepeat(500, 100, 100) { h.handleBackspace() }
            c == 0 && r == 1 -> startRepeat(400, 110, 40) { h.handleCursorLeft() }
            c == 4 && r == 1 -> startRepeat(400, 110, 40) { h.handleCursorRight() }
        }
    }

    private fun onUp(r: Int, c: Int, d: Int) {
        if (c == 0) {
            when (r) {
                0 -> h.undoLastDeletion()
                2 -> {
                    h.commitTopCandidate()
                    h.subMode = InputHandler.SubMode.SYMBOL
                    h.updateView()
                }
                3 -> h.cycleInputMode()
            }
            lastAlpha = ""
            return
        }
        if (c == 4) {
            when (r) {
                2 -> h.handleSpaceKey()
                3 -> h.handleEnterKey()
            }
            if (r != 0) lastAlpha = ""
            return
        }
        val key = grid().getOrNull(r)?.getOrNull(c - 1) ?: return
        when {
            key.special == "dakuten" -> h.handleVariantCycle()
            key.special == "case" -> {
                // 直前に打った小文字を大文字に打ち直す
                if (lastAlpha.isNotEmpty()) {
                    h.handleBackspace()
                    h.insertText(lastAlpha.uppercase())
                    lastAlpha = ""
                }
            }
            key.special == "punct" -> {
                val p = punct(key.at(d))
                if (h.inputState == InputHandler.InputState.IDLE) h.insertText(p) else h.handleDirectKana(p)
            }
            h.subMode == InputHandler.SubMode.NUMERIC -> {
                val s = key.at(d)
                if (h.inputState == InputHandler.InputState.IDLE) h.insertText(s) else h.handleDirectKana(s)
            }
            h.inputMode == InputHandler.InputMode.ALPHANUMERIC -> {
                val s = key.at(d).let { if (it.length == 1 && it[0].isLetter()) it.lowercase() else it }
                if (h.inputState == InputHandler.InputState.IDLE) h.insertText(s) else h.handleKeyPress(s)
                lastAlpha = if (s.length == 1 && s[0] in 'a'..'z') s else ""
            }
            else -> h.handleDirectKana(key.at(d))
        }
    }

    private fun startRepeat(delay: Long, interval: Long, fast: Long, action: () -> Unit) {
        action()
        val started = System.currentTimeMillis()
        val r = object : Runnable {
            override fun run() {
                action()
                timers.postDelayed(this, if (System.currentTimeMillis() - started > delay + 600) fast else interval)
            }
        }
        repeating = r
        timers.postDelayed(r, delay)
    }

    private fun stopRepeat() {
        repeating?.let { timers.removeCallbacks(it) }
        repeating = null
    }

    private fun dir(dx: Float, dy: Float): Int {
        if (abs(dx) < threshold && abs(dy) < threshold) return 0
        return if (abs(dx) >= abs(dy)) (if (dx > 0) 3 else 1) else (if (dy < 0) 2 else 4)
    }

    companion object {
        const val LONG_MS = 450L
        private val ASCII_PUNCT = mapOf("、" to ",", "。" to ".", "！" to "!", "？" to "?", "…" to "...")
    }
}

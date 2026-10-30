package com.shunti.japaneseime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.text.TextUtils
import android.text.TextPaint
import android.view.MotionEvent
import android.view.View

/**
 * キーボードの上の ⚙ から開くメニュー (キーボードの場所に出す)。
 * 上: 片手 (左・右)・フローティング・テーマ・辞書に登録・アプリの設定・キーボードの切替・閉じる
 * 下: クリップボードの履歴 (タップで貼り付け)。描画も当たり判定も自前。
 */
@SuppressLint("ViewConstructor")
class MenuView(context: Context, private val host: Host, private val haptic: () -> Unit) : View(context) {

    interface Host {
        val oneHanded: String        // "off" / "left" / "right"
        val floating: Boolean
        val themeLabel: String
        val clipHistory: List<String>
        fun toggleOneHanded(side: String)
        fun toggleFloating()
        fun cycleTheme()
        fun openApp(section: String?)
        fun showImePicker()
        fun paste(text: String)
        fun closeMenu()
    }

    var theme: Map<String, Int> = emptyMap()
        set(v) { field = v; invalidate() }
    var totalH = 0f

    private val dp = resources.displayMetrics.density
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { typeface = JpFont.get(context) }
    private val rect = RectF()
    private var hits: List<Pair<RectF, () -> Unit>> = emptyList()
    private var pressed: RectF? = null

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), totalH.toInt())
    }

    override fun onDraw(canvas: Canvas) {
        paint.color = theme.getValue("panelBg")
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        val out = ArrayList<Pair<RectF, () -> Unit>>()
        val tiles = listOf(
            Triple("片手 (左)", host.oneHanded == "left") { host.toggleOneHanded("left") },
            Triple("片手 (右)", host.oneHanded == "right") { host.toggleOneHanded("right") },
            Triple("フローティング", host.floating) { host.toggleFloating() },
            Triple("テーマ: ${host.themeLabel}", false) { host.cycleTheme() },
            Triple("辞書に登録", false) { host.openApp("dict") },
            Triple("アプリの設定", false) { host.openApp(null) },
            Triple("キーボード切替", false) { host.showImePicker() },
            Triple("閉じる", false) { host.closeMenu() },
        )
        val cols = 4
        val tw = (width - 8 * dp) / cols
        val th = 46 * dp
        for ((i, t) in tiles.withIndex()) {
            val l = 4 * dp + (i % cols) * tw
            val top = 6 * dp + (i / cols) * th
            val r = RectF(l + 3 * dp, top + 3 * dp, l + tw - 3 * dp, top + th - 3 * dp)
            paint.color = theme.getValue(if (r == pressed) "funcBg" else if (t.second) "accentSoft" else "specialBg")
            canvas.drawRoundRect(r, 8 * dp, 8 * dp, paint)
            label(canvas, t.first, r, 13f, if (t.second) "accentStrong" else "textPrimary", center = true)
            out.add(r to t.third)
        }
        // クリップボード
        var y = 6 * dp + 2 * th + 10 * dp
        text.textSize = 12 * dp
        text.color = theme.getValue("textSecondary")
        text.textAlign = Paint.Align.LEFT
        canvas.drawText("クリップボード (タップで貼り付け。保存はしません)", 10 * dp, y + 12 * dp, text)
        y += 20 * dp
        val clips = host.clipHistory
        if (clips.isEmpty()) {
            canvas.drawText("コピーした文がここに出ます", 10 * dp, y + 18 * dp, text)
        }
        val rowH = 38 * dp
        for (c in clips) {
            if (y + rowH > height - 4 * dp) break
            val r = RectF(6 * dp, y + 2 * dp, width - 6 * dp, y + rowH - 2 * dp)
            paint.color = theme.getValue(if (r == pressed) "funcBg" else "keyBg")
            canvas.drawRoundRect(r, 8 * dp, 8 * dp, paint)
            label(canvas, c.replace('\n', ' '), RectF(r.left + 10 * dp, r.top, r.right - 10 * dp, r.bottom), 14f, "textPrimary", center = false)
            out.add(r to { host.paste(c) })
            y += rowH
        }
        hits = out
    }

    private fun label(canvas: Canvas, s: String, r: RectF, sp: Float, color: String, center: Boolean) {
        text.textSize = sp * dp
        text.color = theme.getValue(color)
        text.textAlign = if (center) Paint.Align.CENTER else Paint.Align.LEFT
        // ボタンの字は、入り切るまで小さくする (それでも入らなければ … で切る)
        if (center) while (text.measureText(s) > r.width() - 8 * dp && text.textSize > 9 * dp) text.textSize -= dp
        val t = TextUtils.ellipsize(s, text, r.width() - 8 * dp, TextUtils.TruncateAt.END).toString()
        canvas.drawText(t, if (center) r.centerX() else r.left, r.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressed = hits.firstOrNull { it.first.contains(e.x, e.y) }?.first
                if (pressed != null) haptic()
                invalidate()
            }
            MotionEvent.ACTION_UP -> {
                val hit = hits.firstOrNull { it.first.contains(e.x, e.y) }
                if (hit != null && hit.first == pressed) hit.second()
                pressed = null
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> {
                pressed = null
                invalidate()
            }
        }
        return true
    }
}

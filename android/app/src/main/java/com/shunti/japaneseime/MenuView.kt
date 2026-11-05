package com.shunti.japaneseime

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.text.Layout
import android.text.StaticLayout
import android.text.TextUtils
import android.text.TextPaint
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration

/**
 * キーボードの上の ⚙ から開くメニュー (キーボードの場所に出す)。
 * 上: 片手 (左・右)・フローティング・テーマ・辞書に登録・アプリの設定・キーボードの切替・閉じる
 * 下: クリップボードの履歴 (文・画像・動画。タップで貼り付け・長押しで大きく見る。上下に動かせる)。描画も当たり判定も自前。
 */
@SuppressLint("ViewConstructor")
class MenuView(context: Context, private val host: Host, private val haptic: () -> Unit) : View(context) {

    interface Host {
        val oneHanded: String        // "off" / "left" / "right"
        val floating: Boolean
        val themeLabel: String
        val clipHistory: List<ClipStore.Clip>
        fun toggleOneHanded(side: String)
        fun toggleFloating()
        fun cycleTheme()
        fun openApp(section: String?)
        fun showImePicker()
        fun pasteClip(c: ClipStore.Clip)
        fun clipThumb(c: ClipStore.Clip): Bitmap?
        fun clipPreview(c: ClipStore.Clip): Bitmap?
        fun toggleClipPin(c: ClipStore.Clip)
        fun removeClip(c: ClipStore.Clip)
        fun clearClips()
        fun closeMenu()
    }

    var theme: Map<String, Int> = emptyMap()
        set(v) { field = v; invalidate() }
    var totalH = 0f

    private val dp = resources.displayMetrics.density
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { typeface = JpFont.get(context) }
    private var hits: List<Pair<RectF, () -> Unit>> = emptyList()
    private var longHits: List<Pair<RectF, () -> Unit>> = emptyList()   // 長押しで動くもの (履歴の行)
    private var pressed: RectF? = null

    // 履歴の欄を上下に動かす
    private var scrollY0 = 0f
    private var maxScroll = 0f
    private var listTop = 0f
    private var downX = 0f
    private var downY = 0f
    private var lastY = 0f
    private var dragging = false
    private var longFired = false
    private val slop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
    private val longPress = Runnable {
        val hit = longHits.firstOrNull { it.first == pressed }
        if (hit != null && !dragging) {
            longFired = true
            haptic()
            pressed = null
            hit.second()
        }
    }

    /** 長押しで開いた大きな見本 (null なら履歴の一覧) */
    private var preview: ClipStore.Clip? = null
    private var previewBmp: Bitmap? = null

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), totalH.toInt())
    }

    override fun onDraw(canvas: Canvas) {
        paint.color = theme.getValue("panelBg")
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        val out = ArrayList<Pair<RectF, () -> Unit>>()
        val outLong = ArrayList<Pair<RectF, () -> Unit>>()
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
        // クリップボード: 見出しと「固定以外を消す」
        var y = 6 * dp + 2 * th + 6 * dp
        val clips = host.clipHistory
        text.textSize = 12 * dp
        text.color = theme.getValue("textSecondary")
        text.textAlign = Paint.Align.LEFT
        canvas.drawText("クリップボード (タップで貼り付け・長押しで見る)", 10 * dp, y + 16 * dp, text)
        if (clips.any { !it.pinned }) {
            val r = RectF(width - 96 * dp, y + 1 * dp, width - 6 * dp, y + 25 * dp)
            paint.color = theme.getValue(if (r == pressed) "funcBg" else "specialBg")
            canvas.drawRoundRect(r, 8 * dp, 8 * dp, paint)
            label(canvas, "固定以外を消す", r, 11f, "textSecondary", center = true)
            out.add(r to { host.clearClips() })
        }
        y += 28 * dp
        listTop = y
        if (clips.isEmpty()) {
            text.textSize = 12 * dp
            text.color = theme.getValue("textSecondary")
            text.textAlign = Paint.Align.LEFT
            canvas.drawText("コピーした文・画像・動画がここに出ます", 10 * dp, y + 18 * dp, text)
        }
        val rowH = 52 * dp
        maxScroll = maxOf(0f, clips.size * rowH + 4 * dp - (height - listTop))
        scrollY0 = scrollY0.coerceIn(0f, maxScroll)
        canvas.save()
        canvas.clipRect(0f, listTop, width.toFloat(), height.toFloat())
        var ry = listTop - scrollY0
        for (c in clips) {
            if (ry + rowH < listTop) { ry += rowH; continue }
            if (ry > height) break
            val r = RectF(6 * dp, ry + 2 * dp, width - 6 * dp, ry + rowH - 2 * dp)
            paint.color = theme.getValue(if (r == pressed) "funcBg" else "keyBg")
            canvas.drawRoundRect(r, 8 * dp, 8 * dp, paint)
            var left = r.left + 10 * dp
            val bmp = host.clipThumb(c)
            if (c.kind != 0) {   // 見本 (正方形に切り抜く)。無ければ種類の字
                val s = r.height() - 8 * dp
                val box = RectF(r.left + 4 * dp, r.top + 4 * dp, r.left + 4 * dp + s, r.top + 4 * dp + s)
                if (bmp != null) drawCover(canvas, bmp, box, 6 * dp) else {
                    paint.color = theme.getValue("specialBg")
                    canvas.drawRoundRect(box, 6 * dp, 6 * dp, paint)
                }
                if (c.kind == 2) playMark(canvas, box)
                left = box.right + 10 * dp
            }
            val right = r.right - (if (c.pinned) 44 * dp else 10 * dp)
            label(canvas, rowLabel(c), RectF(left, r.top, right, r.bottom), 14f, "textPrimary", center = false)
            if (c.pinned) label(canvas, "固定", RectF(right, r.top, r.right - 6 * dp, r.bottom), 11f, "accentStrong", center = true)
            // 欄の外に出た行は押せない
            val vis = RectF(r.left, maxOf(r.top, listTop), r.right, r.bottom)
            if (vis.height() > 8 * dp) {
                out.add(r to { host.pasteClip(c) })
                outLong.add(r to { openPreview(c, r.centerY()) })
            }
            ry += rowH
        }
        canvas.restore()
        preview?.let {   // 長押しのポップアップ: 一覧の上に重ねる (画面は替えない)。外を押すと閉じる
            out.clear()
            outLong.clear()
            drawPopup(canvas, it, out)
        }
        hits = out
        longHits = outLong
    }

    private fun rowLabel(c: ClipStore.Clip): String = when (c.kind) {
        0 -> c.text.replace('\n', ' ')
        1 -> "画像" + sizeText(c)
        2 -> "動画  " + dur(c.durMs) + sizeText(c)
        else -> c.name.ifEmpty { "ファイル" } + sizeText(c)
    }

    private fun sizeText(c: ClipStore.Clip): String {
        val kb = c.size / 1024.0
        return "  " + if (kb < 1024) "${kb.toInt()} KB" else String.format("%.1f MB", kb / 1024)
    }

    private fun dur(ms: Long): String {
        val s = ms / 1000
        return "%d:%02d".format(s / 60, s % 60)
    }

    private var popupAt = 0f   // 長押しした行の縦の真ん中 (ポップアップをその近くに出す)

    private fun openPreview(c: ClipStore.Clip, at: Float) {
        popupAt = at
        preview = c
        previewBmp = host.clipPreview(c)
        invalidate()
    }

    /**
     * 長押しのポップアップ: 行の近くに、見本 (画像・動画は絵、文は数行) と 貼り付け・固定・消去 を出す。
     * 後ろを暗くして、外を押したら閉じる (ユーザー 2026-11-05: 画面を替えずにポップアップで)
     */
    private fun drawPopup(canvas: Canvas, c: ClipStore.Clip, out: ArrayList<Pair<RectF, () -> Unit>>) {
        paint.color = 0x66000000
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        out.add(RectF(0f, 0f, width.toFloat(), height.toFloat()) to { closePreview() })   // 外 (後ろに足したカードの当たりが先に効く)
        val btnH = 40 * dp
        val pad = 8 * dp
        val bmp = previewBmp
        val maxBody = height * 0.55f
        val cw = minOf(width - 24 * dp, 360 * dp)
        // 見本の高さ
        val bodyH: Float
        var lay: StaticLayout? = null
        if (c.kind != 0 && bmp != null) {
            bodyH = minOf(maxBody, (cw - 2 * pad) * bmp.height / bmp.width)
        } else {
            text.textSize = 14 * dp
            text.textAlign = Paint.Align.LEFT
            val body = if (c.kind == 0) c.text else rowLabel(c)
            val l = StaticLayout.Builder.obtain(body, 0, body.length, text, (cw - 2 * pad).toInt())
                .setAlignment(Layout.Alignment.ALIGN_NORMAL).setMaxLines(maxOf(1, (maxBody / (19 * dp)).toInt()))
                .setEllipsize(TextUtils.TruncateAt.END).build()
            lay = l
            bodyH = l.height.toFloat()
        }
        val infoH = if (c.kind != 0) 16 * dp else 0f
        val ch = pad + bodyH + infoH + pad + btnH + pad
        val left = (width - cw) / 2
        val top = (popupAt - ch / 2).coerceIn(6 * dp, maxOf(6 * dp, height - ch - 6 * dp))
        val card = RectF(left, top, left + cw, top + ch)
        paint.color = theme.getValue("panelBg")
        canvas.drawRoundRect(card, 12 * dp, 12 * dp, paint)
        val area = RectF(card.left + pad, card.top + pad, card.right - pad, card.top + pad + bodyH)
        if (lay != null) {
            text.color = theme.getValue("textPrimary")
            canvas.save()
            canvas.translate(area.left, area.top)
            lay.draw(canvas)
            canvas.restore()
        } else if (bmp != null) {
            val r = minOf(area.width() / bmp.width, area.height() / bmp.height)
            val w = bmp.width * r
            val h = bmp.height * r
            val dst = RectF(area.centerX() - w / 2, area.top, area.centerX() + w / 2, area.top + h)
            canvas.drawBitmap(bmp, null, dst, paint)
            if (c.kind == 2) playMark(canvas, dst)
        }
        if (c.kind != 0) label(canvas, rowLabel(c), RectF(area.left, area.bottom, area.right, area.bottom + infoH), 11f, "textSecondary", center = true)
        val btns = listOf(
            "貼り付け" to { closePreview(); host.pasteClip(c) },
            "固定" to { host.toggleClipPin(c); closePreview() },   // 固定しているときは色が付く (押すと外れる)
            "消去" to { host.removeClip(c); closePreview() },
        )
        val by = card.bottom - pad - btnH
        val bw = (cw - 2 * pad) / btns.size
        for ((i, b) in btns.withIndex()) {
            val r = RectF(card.left + pad + i * bw + 3 * dp, by, card.left + pad + (i + 1) * bw - 3 * dp, by + btnH)
            val on = i == 0 || (i == 1 && c.pinned)
            paint.color = theme.getValue(if (r == pressed) "funcBg" else if (on) "accentSoft" else "specialBg")
            canvas.drawRoundRect(r, 8 * dp, 8 * dp, paint)
            label(canvas, b.first, r, 13f, if (on) "accentStrong" else "textPrimary", center = true)
            out.add(0, r to b.second)   // ボタンの当たりを外より先に
        }
        out.add(btns.size, card to {})   // カードの中の空いた所を押しても閉じない
    }

    private fun closePreview() {
        preview = null
        previewBmp = null
        invalidate()
    }

    /** 見本を box に隙間なく (はみ出しは切る) 角を丸めて描く */
    private fun drawCover(canvas: Canvas, b: Bitmap, box: RectF, radius: Float) {
        val r = maxOf(box.width() / b.width, box.height() / b.height)
        val sw = (box.width() / r).toInt()
        val sh = (box.height() / r).toInt()
        val src = Rect((b.width - sw) / 2, (b.height - sh) / 2, (b.width + sw) / 2, (b.height + sh) / 2)
        canvas.save()
        canvas.clipPath(Path().apply { addRoundRect(box, radius, radius, Path.Direction.CW) })
        canvas.drawBitmap(b, src, box, paint)
        canvas.restore()
    }

    /** 動画の印 (▶ を丸の中に) */
    private fun playMark(canvas: Canvas, box: RectF) {
        val rad = minOf(box.width(), box.height()) * 0.18f
        paint.color = 0x99000000.toInt()
        canvas.drawCircle(box.centerX(), box.centerY(), rad, paint)
        paint.color = 0xFFFFFFFF.toInt()
        val p = Path().apply {
            moveTo(box.centerX() - rad * 0.35f, box.centerY() - rad * 0.5f)
            lineTo(box.centerX() + rad * 0.55f, box.centerY())
            lineTo(box.centerX() - rad * 0.35f, box.centerY() + rad * 0.5f)
            close()
        }
        canvas.drawPath(p, paint)
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
                downX = e.x; downY = e.y; lastY = e.y
                dragging = false
                longFired = false
                pressed = hits.firstOrNull { it.first.contains(e.x, e.y) }?.first
                if (pressed != null) haptic()
                if (pressed != null && longHits.any { it.first == pressed }) postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                // 履歴の欄の中で縦に動かしたら、押したのをやめて欄を動かす
                if (!dragging && preview == null && downY >= listTop && kotlin.math.abs(e.y - downY) > slop && maxScroll > 0) {
                    dragging = true
                    pressed = null
                    removeCallbacks(longPress)
                }
                if (dragging) {
                    scrollY0 = (scrollY0 - (e.y - lastY)).coerceIn(0f, maxScroll)
                    invalidate()
                }
                lastY = e.y
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longPress)
                if (!dragging && !longFired) {
                    val hit = hits.firstOrNull { it.first.contains(e.x, e.y) }
                    if (hit != null && hit.first == pressed) hit.second()
                }
                pressed = null
                dragging = false
                invalidate()
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPress)
                pressed = null
                dragging = false
                invalidate()
            }
        }
        return true
    }

    /** メニューを開き直したら一覧の頭から */
    fun reset() {
        preview = null
        previewBmp = null
        scrollY0 = 0f
    }
}

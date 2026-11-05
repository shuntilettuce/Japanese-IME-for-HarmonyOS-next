package com.shunti.japaneseime

import android.content.ContentResolver
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.zip.CRC32

/**
 * クリップボードの履歴 (2026-11-05 ユーザー: 画像・動画も入れて見本を出す・保存する)。
 * 文と、画像・動画・ファイルの写しを、アプリの専用領域 (filesDir/clips) に置く。写しは ClipProvider で貼り付け先に渡す。
 * 固定 (pinned) したものは上限に数えず、消さない。
 */
class ClipStore private constructor(context: Context) {

    /** kind: 0 文・1 画像・2 動画・3 ほかのファイル。file・thumb は dir の中の名前 */
    class Clip(
        val id: Long, val kind: Int, val text: String, val mime: String, val file: String?, val thumb: String?,
        val size: Long, val time: Long, var pinned: Boolean, val name: String, val durMs: Long, val sig: String,
    )

    val dir = File(context.filesDir, "clips").apply { mkdirs() }
    private val index = File(dir, "clips.json")
    private val items = ArrayList<Clip>()   // 新しい順 (固定したものも同じ並びの中。表示で前に出す)

    init {
        load()
        prune()
    }

    /** 表示の順: 固定したもの → 新しい順 */
    @Synchronized fun list(): List<Clip> = items.filter { it.pinned } + items.filter { !it.pinned }

    @Synchronized fun addText(t: String): Clip {
        val sig = "t:" + t.hashCode() + ":" + t.length
        val old = items.firstOrNull { it.sig == sig && it.text == t }
        if (old != null) items.remove(old)
        val c = Clip(now(), 0, t, "text/plain", null, null, t.length.toLong(), System.currentTimeMillis(), old?.pinned == true, "", 0, sig)
        items.add(0, c)
        prune()
        save()
        return c
    }

    /**
     * 画像・動画・ファイルの写しを取って履歴に入れる (重いので裏のスレッドで)。読めない・大きすぎるときは null。
     * 同じ中身 (大きさと CRC32 が同じ) なら、前のものを一番上に動かす
     */
    fun addMedia(cr: ContentResolver, uri: Uri, mimeIn: String?): Clip? {
        val mime = (mimeIn?.takeIf { it.isNotEmpty() && it != "*/*" } ?: cr.getType(uri) ?: "application/octet-stream").lowercase()
        val kind = when {
            mime.startsWith("image/") -> 1
            mime.startsWith("video/") -> 2
            else -> 3
        }
        val name = runCatching {
            cr.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { if (it.moveToFirst()) it.getString(0) else null }
        }.getOrNull() ?: ""
        val id = now()
        val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime) ?: name.substringAfterLast('.', "bin").take(8)
        val f = File(dir, "$id.$ext")
        val crc = CRC32()
        var size = 0L
        val ok = runCatching {
            cr.openInputStream(uri)?.use { inp ->
                f.outputStream().use { out ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = inp.read(buf)
                        if (n < 0) break
                        size += n
                        if (size > MAX_FILE) return@runCatching false
                        crc.update(buf, 0, n)
                        out.write(buf, 0, n)
                    }
                }
                true
            } ?: false
        }.getOrDefault(false)
        if (!ok || size == 0L) {
            f.delete()
            return null
        }
        val sig = "m:$size:${crc.value}"
        synchronized(this) {
            val old = items.firstOrNull { it.sig == sig }
            if (old != null) {   // 同じものをもう一度コピーした: 写しは前のものを使う
                f.delete()
                items.remove(old)
                val c = Clip(id, old.kind, "", old.mime, old.file, old.thumb, old.size, System.currentTimeMillis(), old.pinned, old.name, old.durMs, sig)
                items.add(0, c)
                save()
                return c
            }
        }
        var dur = 0L
        val thumb: Bitmap? = when (kind) {
            1 -> decodeSampled(f, THUMB_PX)
            2 -> runCatching {
                MediaMetadataRetriever().run {
                    try {
                        setDataSource(f.absolutePath)
                        dur = extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L
                        getFrameAtTime(minOf(1_000_000L, dur * 500), MediaMetadataRetriever.OPTION_CLOSEST_SYNC)?.let { scale(it, THUMB_PX) }
                    } finally {
                        release()
                    }
                }
            }.getOrNull()
            else -> null
        }
        val tName = thumb?.let { b ->
            val tf = File(dir, "$id.thumb.jpg")
            runCatching { tf.outputStream().use { b.compress(Bitmap.CompressFormat.JPEG, 85, it) } }.getOrNull()?.let { tf.name }
        }
        val c = Clip(id, kind, "", mime, f.name, tName, size, System.currentTimeMillis(), false, name, dur, sig)
        synchronized(this) {
            items.add(0, c)
            prune()
            save()
        }
        return c
    }

    @Synchronized fun togglePin(id: Long) {
        items.firstOrNull { it.id == id }?.let { it.pinned = !it.pinned }
        prune()
        save()
    }

    @Synchronized fun remove(id: Long) {
        items.firstOrNull { it.id == id }?.let { drop(it) }
        save()
    }

    /** 固定以外を消す。all = true なら固定したものも全部 (設定画面から) */
    @Synchronized fun clear(all: Boolean) {
        for (c in items.filter { all || !it.pinned }) drop(c)
        save()
    }

    fun fileOf(c: Clip): File? = c.file?.let { File(dir, it) }?.takeIf { it.exists() }
    fun thumbOf(c: Clip): File? = c.thumb?.let { File(dir, it) }?.takeIf { it.exists() }

    // ---------------------------------------------------------------- 中
    private var lastId = 0L
    private fun now(): Long = maxOf(System.currentTimeMillis(), lastId + 1).also { lastId = it }

    private fun drop(c: Clip) {
        items.remove(c)
        if (items.none { it.file == c.file && c.file != null }) c.file?.let { File(dir, it).delete() }
        if (items.none { it.thumb == c.thumb && c.thumb != null }) c.thumb?.let { File(dir, it).delete() }
    }

    /** 上限: 固定以外は MAX_ITEMS 件・MAX_DAYS 日・写しの合計 MAX_MEDIA まで (古いものから消す) */
    private fun prune() {
        val old = System.currentTimeMillis() - MAX_DAYS * 86_400_000L
        for (c in items.filter { !it.pinned && it.time < old }) drop(c)
        while (items.count { !it.pinned } > MAX_ITEMS) drop(items.last { !it.pinned })
        while (items.filter { !it.pinned && it.file != null }.sumOf { it.size } > MAX_MEDIA) drop(items.last { !it.pinned && it.file != null })
        // 履歴に無い写し (前に落ちたときの残り) を片付ける
        val keep = items.flatMap { listOfNotNull(it.file, it.thumb) }.toSet() + index.name
        dir.listFiles()?.forEach { if (it.name !in keep && !it.name.endsWith(".tmp")) it.delete() }
    }

    private fun load() {
        val a = runCatching { JSONArray(index.readText()) }.getOrNull() ?: return
        for (i in 0 until a.length()) {
            val o = a.optJSONObject(i) ?: continue
            val c = Clip(o.optLong("id"), o.optInt("kind"), o.optString("text"), o.optString("mime"), o.optString("file").ifEmpty { null },
                o.optString("thumb").ifEmpty { null }, o.optLong("size"), o.optLong("time"), o.optBoolean("pinned"), o.optString("name"),
                o.optLong("dur"), o.optString("sig"))
            if (c.kind != 0 && fileOf(c) == null) continue   // 写しが消えている
            items.add(c)
        }
    }

    private fun save() {
        val a = JSONArray()
        for (c in items) a.put(JSONObject().apply {
            put("id", c.id); put("kind", c.kind); put("text", c.text); put("mime", c.mime); put("file", c.file ?: "")
            put("thumb", c.thumb ?: ""); put("size", c.size); put("time", c.time); put("pinned", c.pinned); put("name", c.name)
            put("dur", c.durMs); put("sig", c.sig)
        })
        runCatching {
            val tmp = File(dir, "clips.json.tmp")
            tmp.writeText(a.toString())
            if (!tmp.renameTo(index)) { index.delete(); tmp.renameTo(index) }
        }
    }

    companion object {
        const val MAX_ITEMS = 50
        const val MAX_DAYS = 30
        const val MAX_FILE = 100L * 1024 * 1024    // これより大きい動画・ファイルは写さない
        const val MAX_MEDIA = 300L * 1024 * 1024   // 写しの合計 (固定以外)
        const val THUMB_PX = 320

        @Volatile private var inst: ClipStore? = null
        fun get(context: Context): ClipStore = inst ?: synchronized(this) { inst ?: ClipStore(context.applicationContext).also { inst = it } }

        fun decodeSampled(f: File, px: Int): Bitmap? = runCatching {
            val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(f.absolutePath, o)
            var s = 1
            while (o.outWidth / (s * 2) >= px && o.outHeight / (s * 2) >= px) s *= 2
            BitmapFactory.decodeFile(f.absolutePath, BitmapFactory.Options().apply { inSampleSize = s })?.let { scale(it, px) }
        }.getOrNull()

        private fun scale(b: Bitmap, px: Int): Bitmap {
            val m = maxOf(b.width, b.height)
            if (m <= px) return b
            val r = px.toFloat() / m
            return Bitmap.createScaledBitmap(b, maxOf(1, (b.width * r).toInt()), maxOf(1, (b.height * r).toInt()), true)
        }
    }
}

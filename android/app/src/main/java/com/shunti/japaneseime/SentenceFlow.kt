package com.shunti.japaneseime

import org.json.JSONObject
import java.io.File

/**
 * 文の流れの予測 (HarmonyOS 版 SentenceFlow と同じ。ユーザー 2026-10-13: 「お疲れ様です」を出したら「。」、続けて「本日の進捗報告です。」)。
 * 入力欄に出た文字を句読点・改行・送信で区切った節の並びとして覚え、節 -> 句読点・節 -> 次の節 の流れのうち 2 回以上あったものを、
 * 確定の後の帯に出す。端末の中 (filesDir/flow.json) だけに保存する。
 */
object SentenceFlow {
    private const val BOUNDARY = "。．！？!?、，\n"
    private const val MAX_CLAUSE = 40
    private const val MAX_KEYS = 10000   // 以前は 1500 (2026-10-24)
    private const val MIN_COUNT = 2

    private val table = HashMap<String, HashMap<String, Int>>()   // "P" + 節 -> 句読点、"N" + 節 -> 次の節
    private var buf = StringBuilder()
    private var prev = ""
    private var file: File? = null
    private var dirty = false

    @Synchronized
    fun init(context: android.content.Context) {
        if (file != null) return
        val f = File(context.filesDir, "flow.json")
        file = f
        if (f.exists()) runCatching {
            val o = JSONObject(f.readText())
            for (k in o.keys()) {
                val m = HashMap<String, Int>()
                val v = o.getJSONObject(k)
                for (x in v.keys()) m[x] = v.getInt(x)
                table[k] = m
            }
        }.onFailure { table.clear(); keepBroken(f) }
    }

    fun save() {
        if (!dirty) return
        val f = file ?: return
        val o = JSONObject()
        for ((k, m) in table) o.put(k, JSONObject().apply { for ((x, n) in m) put(x, n) })
        dirty = !writeAtomic(f, o.toString())   // 書けなければ次の機会にもう一度
    }

    fun clear() {
        table.clear()
        dirty = true
        save()
        reset()
    }

    /** 別の欄に移った・カーソルを動かした: 途中の節はつながらない */
    fun reset() {
        buf = StringBuilder()
        prev = ""
    }

    /** 送信・改行: 文が終わった (句読点は無い) */
    fun boundary() {
        close("")
        prev = ""
    }

    /** 確定した文字を 1 字消した */
    fun backspace() {
        if (buf.isNotEmpty()) buf.setLength(buf.length - 1) else reset()
    }

    /** 入力欄に文字が出た (どの経路の確定でも) */
    fun feed(text: String) {
        for (ch in text) {
            if (ch in BOUNDARY) {
                close(if (ch == '\n') "" else ch.toString())
            } else {
                buf.append(ch)
                if (buf.length > MAX_CLAUSE * 2) buf = StringBuilder(buf.takeLast(MAX_CLAUSE * 2))
            }
        }
    }

    private fun close(punct: String) {
        val c = buf.toString().trim()
        buf = StringBuilder()
        if (c.isEmpty() || c.length > MAX_CLAUSE) {
            if (c.length > MAX_CLAUSE) prev = ""
            return
        }
        if (punct.isNotEmpty()) bump("P$c", punct)
        if (prev.isNotEmpty()) bump("N$prev", c)
        prev = c
        dirty = true
    }

    private fun bump(key: String, v: String) {
        val m = table.getOrPut(key) {
            if (table.size >= MAX_KEYS) table.minByOrNull { e -> e.value.values.sum() }?.key?.let { table.remove(it) }
            HashMap()
        }
        m[v] = (m[v] ?: 0) + 1
    }

    private fun top(key: String, n: Int): List<String> =
        table[key]?.entries?.filter { it.value >= MIN_COUNT }?.sortedByDescending { it.value }?.take(n)?.map { it.key } ?: emptyList()

    /** 確定の後の帯に出す続き (先頭ほど確か) */
    fun predict(): List<String> {
        val cur = buf.toString().trim()
        if (cur.isNotEmpty()) return top("P$cur", 2)
        if (prev.isEmpty()) return emptyList()
        return top("N$prev", 3).map { nx -> nx + (top("P$nx", 1).firstOrNull() ?: "") }
    }
}

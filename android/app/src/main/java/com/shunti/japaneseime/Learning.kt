package com.shunti.japaneseime

import org.json.JSONObject
import java.io.File

/**
 * 変換の学習 (HarmonyOS 版 KanaKanjiConverter の recordChoice / applyLearnedOrder と同じ決まり)。
 * 読み → {選んだ表記: 回数, "@day": 最後に選んだ日}。読みは 2 万個まで (あふれたら、回数が少なく長く使っていない読みから捨てる。
 * 以前は 500 個で、普段使いだとすぐ押し出されていた。2026-10-24)、
 * 1 つの読みに表記は 5 個まで。候補の列は、選んだ回数の多い表記を前に寄せる。
 * 端末の中のファイル (filesDir/learned.json) にだけ保存する。
 */
class Learning(private val file: File) {
    private var learned = LinkedHashMap<String, LinkedHashMap<String, Int>>()
    private val days = HashMap<String, Int>()   // 読み -> 最後に選んだ日 (1970-01-01 からの日数。M5 の「使う人の印」の時期に使う)
    private var dirty = false

    init {
        runCatching {
            val o = JSONObject(file.readText())
            for (r in o.keys()) {
                val e = o.getJSONObject(r)
                learned[r] = LinkedHashMap<String, Int>().apply {
                    for (s in e.keys()) if (s == DAY) days[r] = e.getInt(s) else put(s, e.getInt(s))
                }
            }
        }
    }

    fun recordChoice(kana: String, candidate: String) {
        if (kana.isEmpty() || candidate.isEmpty() || candidate == kana) return
        val entry = learned[kana] ?: run {
            if (learned.size >= MAX_READINGS) evictWeakestLearnedReading()
            LinkedHashMap<String, Int>().also { learned[kana] = it }
        }
        if (entry.containsKey(candidate) || entry.size < MAX_SURFACES) {
            entry[candidate] = (entry[candidate] ?: 0) + 1
            dirty = true
        }
        days[kana] = today()
    }

    /** 候補を消したときなど: その読みでその表記を選んだ記録を消す */
    fun forgetChoice(kana: String, candidate: String) {
        val entry = learned[kana] ?: return
        if (entry.remove(candidate) == null) return
        if (entry.isEmpty()) { learned.remove(kana); days.remove(kana) }
        dirty = true
    }

    private fun evictWeakestLearnedReading() {
        // 回数が少なく長く使っていない読みから (回数 1 は 30 日ぶん。20 回以上は同じ扱い。PC 版と同じ)
        val t = today()
        val weakest = learned.minByOrNull { (r, e) -> minOf(e.values.maxOrNull() ?: 0, 20) * 30 - (days[r]?.let { t - it } ?: 365) }?.key ?: return
        learned.remove(weakest)
        days.remove(weakest)
    }

    /** 出来上がった候補の列のうち、選んだことのある表記を回数の多い順に前へ (他の並びはそのまま) */
    fun applyLearnedOrder(kana: String, cands: List<String>): List<String> {
        val counts = learned[kana]
        if (counts.isNullOrEmpty()) return cands
        val boosted = cands.filter { (counts[it] ?: 0) > 0 }.sortedByDescending { counts[it] ?: 0 }
        return capEnglish(kana, boosted + cands.filter { (counts[it] ?: 0) == 0 })
    }

    /** 英カタカナ辞書の英単語 (全部小文字: いんじぇくしょん → injection) は、学習で上がっても 2 位まで (1 位はカタカナのまま) */
    private fun capEnglish(kana: String, list: List<String>): List<String> {
        if (list.size < 2 || !ENGLISH.matches(list[0]) || kana.all { it.code < 0x80 }) return list
        return listOf(list[1], list[0]) + list.drop(2)
    }

    /** 予測: 読みが prefix で始まる、より長い読みで選んだ表記 (選んだ回数の多い順) */
    fun completions(prefix: String, max: Int): List<String> {
        if (prefix.isEmpty()) return emptyList()
        // 2 回以上選んだものだけ (HarmonyOS 版 LEARNED_CONFIDENT_MIN と同じ。1 回きりの長い文が続きに出ていた。ユーザー 2026-10-14)
        return learned.entries.asSequence()
            .filter { it.key.length > prefix.length && it.key.startsWith(prefix) }
            .flatMap { e -> e.value.entries.filter { it.value >= 2 && !redundant(prefix, e.key, it.key) }.map { it.key to it.value } }
            .sortedByDescending { it.second }
            .map { it.first }.distinct().take(max).toList()
    }

    /**
     * 続きの候補として冗長か (HarmonyOS 版 PredictiveConversion.redundantCompletion と同じ。ユーザー 2026-10-09: 冗長な候補を学習しやすく、
     * 1 熟語で変換したいのに余分を消す手間が出る)。
     * 続きが表記の終わりのかなを足しただけ (しゃしん + を -> 写真を)、または助詞を挟んで別の語に続く (写真を撮った) もの
     */
    private fun redundant(prefix: String, reading: String, surface: String): Boolean {
        val ext = reading.substring(prefix.length)
        if (ext.isNotEmpty() && surface.endsWith(ext)) return true
        return PARTICLE_GAP.containsMatchIn(surface)
    }

    fun clear() {
        learned.clear()
        days.clear()
        dirty = true
    }

    /** 変わっていれば保存する (書き込みを減らすため、入力欄を離れたときなどにまとめて) */
    fun save() {
        if (!dirty) return
        dirty = false
        val o = JSONObject()
        for ((r, e) in learned) o.put(r, JSONObject().apply { for ((s, n) in e) put(s, n); days[r]?.let { put(DAY, it) } })
        runCatching {
            val tmp = File(file.path + ".tmp")
            tmp.writeText(o.toString())
            tmp.renameTo(file)
        }
    }

    companion object {
        private val ENGLISH = Regex("[a-z][a-z-]+")
        /** 語の切れ目の助詞 (の・送り仮名は 1 語の中にもあるので見ない: 柿の種・お疲れ様) */
        private val PARTICLE_GAP = Regex("[一-鿿々ァ-ヺ][をにがはでとへもや][一-鿿々ァ-ヺ]")
        private var instance: Learning? = null

        /** キーボードと設定画面で同じものを使う (設定画面で消した学習を、キーボードが古い中身で書き戻さないように) */
        @Synchronized
        fun get(context: android.content.Context): Learning =
            instance ?: Learning(java.io.File(context.filesDir, "learned.json")).also { instance = it }

        private const val MAX_READINGS = 20000
        private const val DAY = "@day"
        fun today(): Int = (System.currentTimeMillis() / 86_400_000L).toInt()
        private const val MAX_SURFACES = 5
    }
}

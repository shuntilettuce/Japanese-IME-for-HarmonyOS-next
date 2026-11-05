package com.shunti.japaneseime

import org.json.JSONObject
import java.io.File

/** 読めなかった (壊れた) ファイルを、上書きで消さないように別名で残す (learned.json.broken-<秒>。2026-10-24 の監査) */
internal fun keepBroken(file: File) {
    if (file.exists()) runCatching { file.renameTo(File(file.path + ".broken-" + System.currentTimeMillis() / 1000)) }
}

/** 一時ファイルに書いてから置き換える。うまくいったら true (失敗を黙って「保存済み」にしない) */
internal fun writeAtomic(file: File, text: String): Boolean = runCatching {
    val tmp = File(file.path + ".tmp")
    tmp.writeText(text)
    tmp.renameTo(file) || run { file.delete(); tmp.renameTo(file) }
}.getOrDefault(false)

/**
 * 変換の学習 (HarmonyOS 版 KanaKanjiConverter の recordChoice / applyLearnedOrder と同じ決まり)。
 * 読み → {選んだ表記: 回数, "@day": 最後に選んだ日, "@c": {左の文脈: {表記: 回数}}}。読みは 2 万個まで (あふれたら、回数が少なく長く使っていない読みから捨てる。
 * 以前は 500 個で、普段使いだとすぐ押し出されていた。2026-10-24)、
 * 1 つの読みに表記は 5 個まで。候補の列は、選んだ回数の多い表記を前に寄せる。
 * 端末の中のファイル (filesDir/learned.json) にだけ保存する。
 */
class Learning(private val file: File) {
    private var learned = LinkedHashMap<String, LinkedHashMap<String, Int>>()
    private val days = HashMap<String, Int>()   // 読み -> 最後に選んだ日 (1970-01-01 からの日数。M5 の「使う人の印」の時期に使う)
    private val contexts = HashMap<String, LinkedHashMap<String, LinkedHashMap<String, Int>>>()   // 読み -> 左の文脈 -> 表記 -> 回数 (古い文脈が前)
    private var dirty = false

    init {
        if (file.exists()) runCatching {
            val o = JSONObject(file.readText())
            for (r in o.keys()) {
                val e = o.getJSONObject(r)
                learned[r] = LinkedHashMap<String, Int>().apply {
                    for (s in e.keys()) when (s) {
                        DAY -> days[r] = e.getInt(s)
                        CTX -> contexts[r] = readContexts(e.getJSONObject(s))
                        else -> put(s, e.getInt(s))
                    }
                }
            }
        }.onFailure { learned.clear(); days.clear(); contexts.clear(); keepBroken(file) }
    }

    private fun readContexts(o: JSONObject) = LinkedHashMap<String, LinkedHashMap<String, Int>>().apply {
        for (left in o.keys()) {
            val c = o.getJSONObject(left)
            put(left, LinkedHashMap<String, Int>().also { m -> for (s in c.keys()) m[s] = c.getInt(s) })
        }
    }

    /** 確定で足す前の値 (確定を取り消したときに戻す。count 0 = その表記は無かった) */
    class Undo(
        val kana: String, val candidate: String, val count: Int, val day: Int?, val left: String?, val more: List<Undo> = emptyList(),
        val evicted: String? = null, val evictedCount: Int = 0,   // 枠から押し出した表記
        val generation: Int = 0,                                  // clear の前の控えは戻さない
    )

    /** strongForms の中身が変わるたびに増える */
    var version = 0
        private set

    private var generation = 0

    /** left = 語の左の文。分からなければ null (文脈の表には足さない) */
    fun recordChoice(kana: String, candidate: String, left: String? = null): Undo? {
        // かなのままの表記も数える。数えないと、かなで打ち続けても 1 回選んだ漢字に勝てない (ユーザー 2026-10-31)
        if (kana.isEmpty() || candidate.isEmpty()) return null
        val key = left?.let { leftKey(it) }
        val count0 = learned[kana]?.get(candidate) ?: 0
        val day0 = days[kana]
        val entry = learned[kana] ?: run {
            if (learned.size >= MAX_READINGS) evictWeakestLearnedReading()
            LinkedHashMap<String, Int>().also { learned[kana] = it }
        }
        // 表記は 5 つまで。埋まっていたら、いちばん少なく選んだ表記と入れ替える (以前は 6 つ目を黙って覚えなかった)
        var evicted: String? = null
        var evictedCount = 0
        if (!entry.containsKey(candidate) && entry.size >= MAX_SURFACES) entry.minByOrNull { it.value }?.let { w ->
            evicted = w.key
            evictedCount = w.value
            entry.remove(w.key)
            dropVotes(kana, w.key)
        }
        val undo = Undo(kana, candidate, count0, day0, key, emptyList(), evicted, evictedCount, generation)
        val count = (entry[candidate] ?: 0) + 1
        entry[candidate] = count
        if (count == 2) version++
        dirty = true
        days[kana] = today()
        if (key != null) note(kana, key, candidate, 1)
        return undo
    }

    /** 文脈の表に足す (add が負なら引く)。使った文脈を後ろへ。あふれたら古い文脈を捨てる (PC 版と同じ) */
    private fun note(kana: String, key: String, surface: String, add: Int) {
        val table = contexts[kana] ?: if (add > 0) LinkedHashMap<String, LinkedHashMap<String, Int>>().also { contexts[kana] = it } else return
        val votes = table.remove(key) ?: if (add > 0) LinkedHashMap<String, Int>() else return
        val n = (votes[surface] ?: 0) + add
        if (n > 0) votes[surface] = n else votes.remove(surface)
        if (votes.isNotEmpty()) table[key] = votes
        if (table.size > MAX_CONTEXTS) table.remove(table.keys.first())
        if (table.isEmpty()) contexts.remove(kana)
    }

    /**
     * 文中の語の読みの表記を置き換える学習 (PC 版 Learning::top と同じ)。同じ左の文脈で選んだ回数から別の表記の票を引いて
     * SAME_CTX_MIN 以上、または ANY_CTX_MIN 回選んでいてこの文脈に別の表記の票が無い表記のうち、いちばん多く選んだもの
     */
    fun top(kana: String, left: String): String? {
        val e = learned[kana] ?: return null
        val here = contexts[kana]?.get(leftKey(left))
        val all = here?.values?.sum() ?: 0
        var best: String? = null
        var most = 0
        for ((s, n) in e) {
            if (n <= most) continue
            val mine = here?.get(s) ?: 0
            val against = all - mine
            if (mine - against >= SAME_CTX_MIN || (n >= ANY_CTX_MIN && against == 0)) {
                most = n
                best = s
            }
        }
        return best
    }

    /** 語ごとの記録と読み全体の記録をまとめて、1 回の取り消しで戻せる形に */
    fun undoOf(list: List<Undo>): Undo? = list.lastOrNull()?.let { Undo(it.kana, it.candidate, it.count, it.day, it.left, list.dropLast(1), it.evicted, it.evictedCount, it.generation) }

    /**
     * 2 回以上選んだ短い語を、変換の網に入れる形で返す。文の途中でも AI が文脈で選べるように (PC 版と同じ)。
     * 後ろの同じかな (なにを → 何を の を) は外す。代表語はその語そのもの (辞書に無い語は入らない)
     */
    fun strongForms(): List<UserDict.Form> {
        val found = ArrayList<Triple<Int, String, String>>()
        for ((r, e) in learned) {
            if (r.length > MAX_NET_READING) continue
            for ((s, n) in e) {
                if (n < 2) continue
                var k = 0
                while (k < r.length && k < s.length && r[r.length - 1 - k] == s[s.length - 1 - k] && r[r.length - 1 - k] in 'ぁ'..'ゖ') k++
                val rr = r.dropLast(k)
                val ss = s.dropLast(k)
                if (rr.isNotEmpty() && ss.isNotEmpty() && rr != ss) found.add(Triple(n, rr, ss))
            }
        }
        // 上位を取ったあとは読みの順に (数の順のままだと、回数が 1 増えるたびに並びが変わって網を入れ直すことになる)
        return found.sortedByDescending { it.first }.take(MAX_NET_WORDS).sortedWith(compareBy({ it.second }, { it.third }))
            .map { UserDict.Form(it.second, it.third, it.second, it.third) }
    }

    /** かなのまま Enter で確定した: この読みに学習があるときだけ数える (PC の confirm と同じ。全部数えると読みの枠を食う) */
    fun confirmKana(kana: String, left: String): Undo? = confirm(kana, kana, left)

    /** 選び直さずに確定した表記: この読みに学習があるときだけ数える (PC の confirm と同じ) */
    fun confirm(kana: String, surface: String, left: String): Undo? = if (learned.containsKey(kana)) recordChoice(kana, surface, left) else null

    /** 確定の取り消し: 足す前の値に戻す */
    fun restore(u: Undo) {
        if (u.generation != generation) return   // 学習をリセットした後の取り消しで、消した学習を作り直さない
        restoreOne(u)
        u.more.asReversed().forEach { restoreOne(it) }
        version++
    }

    private fun restoreOne(u: Undo) {
        if (u.left != null) note(u.kana, u.left, u.candidate, -1)
        if (u.count == 0 && !learned.containsKey(u.kana)) return
        val entry = learned.getOrPut(u.kana) { LinkedHashMap() }
        if (u.count > 0) entry[u.candidate] = u.count else entry.remove(u.candidate)
        u.evicted?.let { entry[it] = u.evictedCount }
        if (entry.isEmpty()) {
            learned.remove(u.kana)
            days.remove(u.kana)
            contexts.remove(u.kana)
        } else if (u.day != null) {
            days[u.kana] = u.day
        } else {
            days.remove(u.kana)
        }
        dirty = true
    }

    /** 候補を消したときなど: その読みでその表記を選んだ記録を消す */
    fun forgetChoice(kana: String, candidate: String) {
        val entry = learned[kana] ?: return
        if (entry.remove(candidate) == null) return
        dropVotes(kana, candidate)
        if (entry.isEmpty()) { learned.remove(kana); days.remove(kana); contexts.remove(kana) }
        dirty = true
        version++
    }

    /** その表記の文脈の票を消す。残すと、もう数えない表記が「別の表記の票」として top を止め続ける */
    private fun dropVotes(kana: String, surface: String) {
        val table = contexts[kana] ?: return
        table.values.forEach { it.remove(surface) }
        table.values.removeAll { it.isEmpty() }
        if (table.isEmpty()) contexts.remove(kana)
    }

    private fun evictWeakestLearnedReading() {
        // 回数が少なく長く使っていない読みから (回数 1 は 30 日ぶん。20 回以上は同じ扱い。PC 版と同じ)
        val t = today()
        val weakest = learned.minByOrNull { (r, e) -> minOf(e.values.maxOrNull() ?: 0, 20) * 30 - (days[r]?.let { t - it } ?: 365) }?.key ?: return
        learned.remove(weakest)
        days.remove(weakest)
        contexts.remove(weakest)
    }

    /** 出来上がった候補の列のうち、選んだことのある表記を回数の多い順に前へ (他の並びはそのまま) */
    fun applyLearnedOrder(kana: String, cands: List<String>): List<String> {
        val counts = learned[kana]
        if (counts.isNullOrEmpty()) return cands
        // 2 回以上選んだ表記は先頭へ (回数の多い順)。1 回だけのものはモデルの 1 位を覆さず 2 位へ (PC・HarmonyOS と同じ。
        // 1 回の誤選択がずっと 1 位になり続けた。2026-10-24 の監査で Android だけ古いままだった)
        val boosted = cands.filter { (counts[it] ?: 0) >= 2 }.sortedByDescending { counts[it] ?: 0 }.toMutableList()
        val once = cands.filter { counts[it] == 1 }
        val rest = cands.filter { (counts[it] ?: 0) == 0 }.toMutableList()
        if (once.isNotEmpty() && boosted.isEmpty() && rest.isNotEmpty()) boosted.add(rest.removeAt(0))
        return capEnglish(kana, boosted + once + rest)
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
        contexts.clear()
        dirty = true
        version++
        generation++
    }

    /** 変わっていれば保存する (書き込みを減らすため、入力欄を離れたときなどにまとめて) */
    fun save() {
        if (!dirty) return
        val o = JSONObject()
        for ((r, e) in learned) o.put(r, JSONObject().apply {
            for ((s, n) in e) put(s, n)
            days[r]?.let { put(DAY, it) }
            contexts[r]?.let { t -> put(CTX, JSONObject().also { c -> for ((left, v) in t) c.put(left, JSONObject(v as Map<*, *>)) }) }
        })
        dirty = !writeAtomic(file, o.toString())   // 書けなければ次の機会にもう一度
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
        private const val MAX_NET_READING = 8   // これより長い読みは文 (語ではない)
        private const val MAX_NET_WORDS = 500
        private const val DAY = "@day"
        private const val CTX = "@c"
        private const val SAME_CTX_MIN = 1      // 手で 1 度
        private const val ANY_CTX_MIN = 3       // 手で 3 度
        private const val CTX_CHARS = 2         // 文脈として覚える左の字数
        private const val MAX_CONTEXTS = 8      // 読みごとに覚える文脈の数

        /** 文脈の鍵: 左の CTX_CHARS 字 (サロゲートの片方で切らない) */
        private fun leftKey(left: String): String {
            if (left.length <= CTX_CHARS) return left
            var st = left.length - CTX_CHARS
            if (left[st].isLowSurrogate()) st--
            return left.substring(st)
        }
        fun today(): Int = (System.currentTimeMillis() / 86_400_000L).toInt()
        private const val MAX_SURFACES = 5
    }
}

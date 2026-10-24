package com.shunti.japaneseime

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * ユーザー辞書: 読み → 単語と品詞 (名詞・人名・地名・動詞 (五段・一段)・形容詞。HarmonyOS 版と同じ分け方)。
 *
 * AI 変換に合わせた使い方: 登録した語を辞書の語と同じく変換の網に入れ、モデルが文脈で採点する
 * (Engine.setUserWords)。読みが文の途中に出てきても候補になる (しゅんてぃはすごい → shuntiはすごい)。
 * 品詞は、辞書にある同じ品詞の代表語 (猫・山田・東京・書く・食べる・高い など) から品詞 ID とコストを写して決める。
 * 動詞と形容詞は、代表語の活用形 (書か・書き・書け・書い…) と同じ語尾の形も作って入れる (ググる → ググれば・ググった)。
 * 読みがちょうど一致したときは、候補の先頭にも出す (InputHandler)。端末の中のファイル (filesDir/userdict.json) にだけ保存する。
 */
class UserDict private constructor(private val file: File) {
    class Entry(val reading: String, val word: String, val pos: String = "noun", val group: String = "")

    /** 網に入れる形: 読み・表記と、品詞を写す代表語の同じ形 */
    class Form(val reading: String, val surface: String, val tmplReading: String, val tmplSurface: String)

    private val entries = ArrayList<Entry>()

    /** 変わるたびに増える (キーボードが変換の網に入れ直すかを決めるのに使う) */
    @Volatile var version = 0
        private set

    init {
        if (file.exists()) runCatching {
            val a = JSONArray(file.readText())
            for (i in 0 until a.length()) {
                val o = a.getJSONObject(i)
                entries.add(Entry(o.getString("r"), o.getString("w"), o.optString("p", "noun"), o.optString("g", "")))
            }
        }.onFailure { entries.clear(); keepBroken(file) }
    }

    @Synchronized
    fun list(): List<Entry> = entries.toList()

    /** 読み (ひらがな) にちょうど一致する登録語 (登録した順) */
    @Synchronized
    fun lookup(reading: String): List<String> = entries.filter { it.reading == reading }.map { it.word }

    /** 登録。読みはひらがなにそろえる。うまく登録できなければ理由 (できたら null) */
    @Synchronized
    fun add(reading: String, word: String, pos: String, group: String): String? {
        val r = toHiragana(reading.trim())
        val w = word.trim()
        if (r.isEmpty() || w.isEmpty()) return "読みと単語を入れてください"
        if (r.any { it !in 'ぁ'..'ゖ' && it != 'ー' }) return "読みはひらがなで入れてください"
        checkForm(r, w, pos, group)?.let { return it }
        if (entries.any { it.reading == r && it.word == w }) return "もう登録してあります"
        entries.add(Entry(r, w, pos, if (pos == "verb") group else ""))
        save()
        return null
    }

    @Synchronized
    fun remove(e: Entry) {
        entries.removeAll { it.reading == e.reading && it.word == e.word }
        save()
    }

    /** 変換の網に入れる形の一覧 */
    @Synchronized
    fun engineForms(): List<Form> = entries.flatMap { forms(it) }

    private fun save() {
        version++
        val a = JSONArray()
        for (e in entries) a.put(JSONObject().put("r", e.reading).put("w", e.word).put("p", e.pos).put("g", e.group))
        writeAtomic(file, a.toString())
    }

    companion object {
        private var instance: UserDict? = null

        @Synchronized
        fun get(context: Context): UserDict = instance ?: UserDict(File(context.filesDir, "userdict.json")).also { instance = it }

        fun toHiragana(s: String) = buildString { for (c in s) append(if (c in 'ァ'..'ヶ') c - 0x60 else c) }

        /** 品詞の表示名 */
        val POS_LABELS = linkedMapOf("noun" to "名詞", "person" to "人名", "place" to "地名", "verb" to "動詞", "adjective" to "形容詞")

        // 品詞を写す代表語 (どれも辞書にあることを確かめてある)
        private val NOUN_TMPL = mapOf("noun" to ("ねこ" to "猫"), "person" to ("やまだ" to "山田"), "place" to ("とうきょう" to "東京"))

        /** 五段: 終止形の最後のかな → 代表語と、活用の語尾 (未然・連用・終止・仮定・意志・音便) */
        private val GODAN = mapOf(
            'う' to Triple("かう", "買う", "わいうえおっ"), 'く' to Triple("かく", "書く", "かきくけこい"),
            'ぐ' to Triple("およぐ", "泳ぐ", "がぎぐげごい"), 'す' to Triple("はなす", "話す", "さしすせそ"),
            'つ' to Triple("まつ", "待つ", "たちつてとっ"), 'ぬ' to Triple("しぬ", "死ぬ", "なにぬねのん"),
            'ぶ' to Triple("あそぶ", "遊ぶ", "ばびぶべぼん"), 'む' to Triple("よむ", "読む", "まみむめもん"),
            'る' to Triple("はしる", "走る", "らりるれろっ"),
        )
        private val ICHIDAN = listOf("", "る", "れ", "ろ", "よ")        // 代表語 食べる
        private val ADJ = listOf("い", "く", "かっ", "けれ", "かろ", "き", "さ", "そう")   // 代表語 高い

        /** 動詞・形容詞は、読みと表記の終わりが活用の形に合っているか */
        private fun checkForm(r: String, w: String, pos: String, group: String): String? = when (pos) {
            "verb" -> when {
                group == "ichidan" && !(r.endsWith("る") && w.endsWith("る")) -> "一段の動詞は「る」で終わる形で入れてください (例: たべる / 食べる)"
                group != "ichidan" && (r.last() !in GODAN || w.last() != r.last()) -> "動詞は終止形で、読みと単語の最後を同じかなにしてください (例: ぐぐる / ググる)"
                else -> null
            }
            "adjective" -> if (r.endsWith("い") && w.endsWith("い")) null else "形容詞は「い」で終わる形で入れてください (例: えもい / エモい)"
            else -> null
        }

        private fun forms(e: Entry): List<Form> {
            val r = e.reading
            val w = e.word
            fun stemForms(tr: String, ts: String, cut: Int, sufs: List<String>) =
                sufs.map { Form(r.dropLast(cut) + it, w.dropLast(cut) + it, tr.dropLast(cut) + it, ts.dropLast(cut) + it) }
            return when (e.pos) {
                "verb" -> if (e.group == "ichidan") stemForms("たべる", "食べる", 1, ICHIDAN)
                else GODAN[r.last()]?.let { (tr, ts, sufs) -> stemForms(tr, ts, 1, sufs.map { it.toString() }) } ?: emptyList()
                "adjective" -> stemForms("たかい", "高い", 1, ADJ)
                else -> NOUN_TMPL[e.pos]?.let { (tr, ts) -> listOf(Form(r, w, tr, ts)) } ?: emptyList()
            }
        }
    }
}

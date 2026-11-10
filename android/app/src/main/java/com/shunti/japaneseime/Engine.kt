package com.shunti.japaneseime

import android.content.res.AssetManager

/** 変換エンジン (kkc/engine/engine.cpp) の窓口。辞書とモデルは APK の資産をそのままメモリに写して使う。 */
class Engine private constructor(private var handle: Long, private val slurs: Regex?) {
    /** 設定「不適切な語も候補に出す」(既定オン)。オンなら slurs で弾かない */
    var allowSlurs = true


    /** 変換の結果: 候補と、1 位の候補の語の区切り (ends[i] = i 語目の読みの終わり、lens[i] = 表記の長さ) */
    class Result(val cands: List<String>, val ends: IntArray, val lens: IntArray,
                 /** 提案 (種類, 表記)。種類は engine.h の KKC_SUGGEST_* (1 打ち間違いの直し 2 日付 3 住所 4 英単語 6 気分 7 記号) */
                 val sugg: List<Pair<Int, String>> = emptyList(),
                 /** 上位の経路 (表示しない。確定した語を学習するのに使う)。1 本ごとに "読みの終わり\u0001表記\u0001…" */
                 val paths: List<String> = emptyList(),
                 /** 1 位の経路 (paths[0]) の文節ごとの読みの終わり (PC 版と同じ決まり。学習の置き換えの単位)。分からなければ空 */
                 val phraseEnds: IntArray = IntArray(0),
                 /** 1 位の候補の語ごとの品詞の左 ID (ends と同じ並び。PC 版の Conversion の words の lid)。分からなければ空 */
                 val lids: IntArray = IntArray(0))

    /** 文の中の語 (読み・表記・品詞の左 ID) */
    class PosWord(val reading: String, val surface: String, val lid: Int)

    /** 上位の候補。ctx = 確定済みの左の文脈、kana = 読み。times に段階ごとのミリ秒が入る (5 個) */
    fun convert(ctx: String, kana: String, max: Int = 10, model: Boolean = true, times: DoubleArray? = null): List<String> =
        convertWithSegments(ctx, kana, max, model, times).cands

    /** keys = PC と同じくローマ字で打ったキーの並びと、読みの字ごとの「その字までのキーの数」(読みと同じ長さ)。次の変換 1 回だけに効く */
    fun convertWithSegments(ctx: String, kana: String, max: Int = 10, model: Boolean = true, times: DoubleArray? = null,
                            keys: Pair<String, IntArray>? = null): Result {
        if (handle == 0L || kana.isEmpty()) return Result(emptyList(), IntArray(0), IntArray(0))
        if (keys != null && keys.second.size == kana.length) nativeSetKeys(handle, keys.first, keys.second)
        val raw = nativeConvert(handle, ctx, kana, max * 2, model, times)
        val seg = nativeSegments(handle)
        val segPos = nativeSegmentPos(handle)
        val paths = nativePaths(handle).toList()
        val phraseEnds = if (raw.isNotEmpty()) nativePhraseEnds(handle, kana, raw[0]) else IntArray(0)
        val sugg =nativeLastSuggest(handle).mapNotNull { s ->
            val i = s.indexOf('\u0001')
            if (i <= 0) null else (s.substring(0, i).toIntOrNull() ?: 0) to s.substring(i + 1)
        }.filter { allowSlurs || slurs == null || !slurs.containsMatchIn(it.second) }
        val cands = raw.asSequence().filter { allowSlurs || slurs == null || !slurs.containsMatchIn(it) }.take(max).toList()
        // 差別語で 1 位が落ちたときは区切りを使わない (区切りは元の 1 位のもの)
        val ok = raw.isNotEmpty() && cands.isNotEmpty() && cands[0] == raw[0]
        val n = if (ok) seg.size / 2 else 0
        return Result(cands, IntArray(n) { seg[2 * it] }, IntArray(n) { seg[2 * it + 1] }, sugg, paths, phraseEnds,
            if (segPos.size == n) segPos else IntArray(0))
    }

    /**
     * 確定した文 (読み, 表記) になる経路のうちコストのいちばん低いものの語 (PC 版 Converter::path_pos)。経路が無ければ空。
     * 品詞ごとの学習で、選んだ文の中での品詞を決める (2026-11-09)。convert と同じスレッドで呼ぶ
     */
    fun pathPos(reading: String, text: String): List<PosWord> {
        if (handle == 0L || reading.isEmpty() || text.isEmpty()) return emptyList()
        val a = nativePathPos(handle, reading, text)
        val out = ArrayList<PosWord>()
        var r0 = 0
        var s0 = 0
        for (i in 0 until a.size / 4) {
            val r1 = a[4 * i]
            val s1 = s0 + a[4 * i + 1]
            if (r1 > reading.length || s1 > text.length || r1 < r0) return emptyList()
            out.add(PosWord(reading.substring(r0, r1), text.substring(s0, s1), a[4 * i + 2]))
            r0 = r1
            s0 = s1
        }
        return out
    }

    /**
     * ユーザー辞書の語を変換の網に入れ直す (前の分は消す)。forms は UserDict.engineForms() の、読み・表記と代表語の組。
     * 代表語から品詞とコストを写し、そこから bonus だけ選ばれやすくする。足せた数を返す。convert と同じスレッドで呼ぶ
     */
    fun setUserWords(forms: List<UserDict.Form>, bonus: Int = 800): Int {
        if (handle == 0L) return 0
        nativeUserClear(handle)
        var n = 0
        for (f in forms) {
            // 学習した語で品詞が分かるもの: 選んだときの品詞の仲間の項目として (PC 版 set_user_words と同じ。2026-11-09)
            if (f.lid >= 0 && nativeUserAddPos(handle, f.reading, f.surface, bonus, 0, f.lid) != 0) {
                n++
                continue
            }
            if (f.lid >= 0 && f.reading.length < 2) continue   // 1 字の語は品詞を決めて置けるときだけ (どの文脈でも安い 1 字の辺にしない)
            var ok = nativeUserAddLike(handle, f.reading, f.surface, f.tmplReading, f.tmplSurface, bonus)
            // 代表語がその語そのもの (学習で選んだ語) で辞書に無いときは、名詞の代表語で入れる (PC 版と同じ)
            if (ok == 0 && f.tmplReading == f.reading && f.tmplSurface == f.surface) ok = nativeUserAddLike(handle, f.reading, f.surface, "ねこ", "猫", bonus)
            n += ok
        }
        return n
    }

    /** 予測: 読みが prefix で始まり、あと maxExtra 字までの辞書の語を、よく使う順に最大 max 個 (辞書を読むだけ。どのスレッドからでもよい) */
    fun complete(prefix: String, max: Int = 3, maxExtra: Int = 6): List<String> =
        if (handle == 0L || prefix.isEmpty()) emptyList() else nativeComplete(handle, prefix, maxExtra, max).toList()

    /**
     * 提案を候補に混ぜず分けて出すか。豆腐になる字の除外はエンジンでは切る (端末で Paint.hasGlyph で調べる:
     * Android 15 から AOSP に Noto Serif Hentaigana が入り、変体仮名も出る端末がある)
     */
    fun setSuggest(on: Boolean) {
        if (handle != 0L) nativeSetSuggest(handle, on, 0)
    }

    /** AI学習 (設定、既定オフ) のオン・オフ。path = 育てた中身の保存先 (null ならオフ)。同じ保存先なら、リセットで消されたかだけを見る。convert と同じスレッドで呼ぶ */
    fun setAiLearn(path: String?) {
        if (handle != 0L) nativeAiSet(handle, path ?: "")
    }

    /** 確定した文 (左の文脈・読み・表記) から 1 歩学んで保存する。返り値 = 学ぶ前の損失 (学ばなかったら負)。convert と同じスレッドで呼ぶ */
    fun aiLearn(ctx: String, reading: String, text: String): Float =
        if (handle == 0L) -1f else nativeAiLearn(handle, ctx, reading, text)

    fun setThreads(n: Int) {
        if (handle != 0L) nativeSetThreads(handle, n)
    }

    fun close() {
        if (handle != 0L) nativeClose(handle)
        handle = 0L
    }

    companion object {
        init {
            System.loadLibrary("kkc")
        }

        /** slurs = 語をつないで差別語になった候補を弾く正規表現 (HarmonyOS 版 AiConverter.SLURS と同じ。Tables から) */
        /** model = モデルの資産の名前 (無ければ kkc_model.bin) */
        fun open(am: AssetManager, slurs: Regex?, threads: Int = 4, model: String = "kkc_model.bin"): Engine? {
            val h = nativeOpen(am, threads, model)
            if (h == 0L) return null
            return Engine(h, slurs)
        }

        @JvmStatic private external fun nativeOpen(am: AssetManager, threads: Int, model: String): Long
        @JvmStatic private external fun nativeClose(h: Long)
        @JvmStatic private external fun nativeConvert(h: Long, ctx: String, kana: String, max: Int, model: Boolean, times: DoubleArray?): Array<String>
        @JvmStatic private external fun nativeSetThreads(h: Long, n: Int)
        @JvmStatic private external fun nativeSetKeys(h: Long, keys: String, spanEnd: IntArray)
        @JvmStatic private external fun nativeSetSuggest(h: Long, on: Boolean, charFilter: Int)
        @JvmStatic private external fun nativeLastSuggest(h: Long): Array<String>
        @JvmStatic private external fun nativeSegments(h: Long): IntArray
        @JvmStatic private external fun nativePaths(h: Long): Array<String>
        @JvmStatic private external fun nativePhraseEnds(h: Long, kana: String, top: String): IntArray
        @JvmStatic private external fun nativeUserClear(h: Long)
        @JvmStatic private external fun nativeComplete(h: Long, prefix: String, maxExtra: Int, maxOut: Int): Array<String>
        @JvmStatic private external fun nativeAiSet(h: Long, path: String)
        @JvmStatic private external fun nativeAiLearn(h: Long, ctx: String, reading: String, text: String): Float
        /** 品詞の表 (Pos が使う) */
        internal fun posTables(): Array<String> = nativePosTables()

        @JvmStatic private external fun nativeSegmentPos(h: Long): IntArray
        @JvmStatic private external fun nativePathPos(h: Long, reading: String, text: String): IntArray
        @JvmStatic private external fun nativePosTables(): Array<String>
        @JvmStatic private external fun nativeUserAddPos(h: Long, r: String, s: String, bonus: Int, flags: Int, lid: Int): Int
        @JvmStatic private external fun nativeUserAddLike(h: Long, r: String, s: String, tr: String, ts: String, bonus: Int): Int
    }
}

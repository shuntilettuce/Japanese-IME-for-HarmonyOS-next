package com.shunti.japaneseime

import android.content.res.AssetManager

/** 変換エンジン (kkc/engine/engine.cpp) の窓口。辞書とモデルは APK の資産をそのままメモリに写して使う。 */
class Engine private constructor(private var handle: Long, private val slurs: Regex?) {
    /** 設定「不適切な語も候補に出す」(既定オン)。オンなら slurs で弾かない */
    var allowSlurs = true


    /** 変換の結果: 候補と、1 位の候補の語の区切り (ends[i] = i 語目の読みの終わり、lens[i] = 表記の長さ) */
    class Result(val cands: List<String>, val ends: IntArray, val lens: IntArray,
                 /** 提案 (種類, 表記)。種類は engine.h の KKC_SUGGEST_* (1 打ち間違いの直し 2 日付 3 住所 4 英単語 6 気分 7 記号) */
                 val sugg: List<Pair<Int, String>> = emptyList())

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
        val sugg = nativeLastSuggest(handle).mapNotNull { s ->
            val i = s.indexOf('\u0001')
            if (i <= 0) null else (s.substring(0, i).toIntOrNull() ?: 0) to s.substring(i + 1)
        }.filter { allowSlurs || slurs == null || !slurs.containsMatchIn(it.second) }
        val cands = raw.asSequence().filter { allowSlurs || slurs == null || !slurs.containsMatchIn(it) }.take(max).toList()
        // 差別語で 1 位が落ちたときは区切りを使わない (区切りは元の 1 位のもの)
        val ok = raw.isNotEmpty() && cands.isNotEmpty() && cands[0] == raw[0]
        val n = if (ok) seg.size / 2 else 0
        return Result(cands, IntArray(n) { seg[2 * it] }, IntArray(n) { seg[2 * it + 1] }, sugg)
    }

    /**
     * ユーザー辞書の語を変換の網に入れ直す (前の分は消す)。forms は UserDict.engineForms() の、読み・表記と代表語の組。
     * 代表語から品詞とコストを写し、そこから bonus だけ選ばれやすくする。足せた数を返す。convert と同じスレッドで呼ぶ
     */
    fun setUserWords(forms: List<UserDict.Form>, bonus: Int = 800): Int {
        if (handle == 0L) return 0
        nativeUserClear(handle)
        var n = 0
        for (f in forms) n += nativeUserAddLike(handle, f.reading, f.surface, f.tmplReading, f.tmplSurface, bonus)
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
        @JvmStatic private external fun nativeUserClear(h: Long)
        @JvmStatic private external fun nativeComplete(h: Long, prefix: String, maxExtra: Int, maxOut: Int): Array<String>
        @JvmStatic private external fun nativeUserAddLike(h: Long, r: String, s: String, tr: String, ts: String, bonus: Int): Int
    }
}

package com.shunti.japaneseime

import android.os.Handler
import java.util.concurrent.ExecutorService

/**
 * AI 変換 (HarmonyOS 版 ime/AiConverter.ets の移植。関数名と決まりは向こうと同じ)。
 * 変換は worker のスレッドで行い、結果は main のスレッドで受け取る。このクラスの状態は main のスレッドだけで触る。
 */
class AiConverter(private val engine: Engine, private val worker: ExecutorService, private val main: Handler) {

    var lastMs = 0.0
        private set

    /** 読み -> 打ったキーの並び (InputHandler が QWERTY・GODAN で打った分を覚えている。無ければ null) */
    var keysFor: (String) -> Pair<String, IntArray>? = { null }

    // ---- 長い入力: 前の方を固定して、後ろだけを変換する ----
    // 変換 1 回が BUDGET_MS を超えて遅れを感じる長さになったら、1 位の候補の語の区切りのうち、最後の KEEP_WORDS 語より前にあって、
    // 2 回続けて同じ変換になったところ (または「、」「。」の後ろ) までを固定する。それより短いうちは文全体に AI をかけ続ける。
    // 固定した部分は文脈としてモデルに渡すので、後ろの変換の質は落ちない。
    // 固定した読みまで消して戻ったら・確定して文脈が変わったら、固定を解く。
    //
    // 既定 (保留) では、固定した部分は入力欄に確定しない。固定した時点でその部分だけを 1 回変換して上位 ALT 個の候補を持たせ、
    // 以後は AI にかけない。候補の一覧には「前の方を持たせた候補に替えた文」も並べ、そこから確定できる。
    // 設定「オート確定」(liveCommit) がオンなら、以前のように末尾 LIVE_KEEP 字より前の固まった所を固定し、呼ぶ側が入力欄に確定する。
    // (HarmonyOS 版 AiConverter.ets と同じ決まり)
    /** 長い入力で固定した前の方の 1 か所 (読みと、選び直せる候補。cands[0] が今の表記) */
    private class FrozenChunk(val kana: String, var cands: List<String>)

    var liveCommit = false
    private var chunks = ArrayList<FrozenChunk>()
    private var fCtx = ""      // 固定を始めたときの文脈
    private var fKana = ""     // 固定した読み (chunks の読みをつないだもの)
    private var fSurf = ""     // 固定した表記 (chunks の cands[0] をつないだもの)
    private var prevSegs = HashMap<Int, String>()   // 前回の結果の (区切りの位置 -> そこまでの表記)
    private var lastTarget = ""                     // 最後に出した読みと、その候補 (結果を待つ間の表示に使う)
    private var lastCands: List<String> = emptyList()
    /** いまの読みの提案 (種類, 表記。表記は固定した前の方込み)。candidates() が更新する */
    var suggestions: List<Pair<Int, String>> = emptyList()
        private set

    /**
     * 読みの全体 target の候補 (固定した部分を前に付けた文)。変換がまだ届いていなければ、裏で変換を頼み、
     * 前回の候補 + 新しく打ったかな を仮に返す (届いたら onDone で出し直す)
     */
    fun candidates(context: String, target: String, onDone: () -> Unit): List<String> {
        if (fCtx != context || !target.startsWith(fKana)) {
            fCtx = context
            fKana = ""
            fSurf = ""
            chunks = ArrayList()
            prevSegs.clear()
        }
        val tail = target.substring(fKana.length)
        if (tail.isEmpty()) return listOf(fSurf)
        val ctx2 = (context + fSurf).takeLast(40)
        val hit = cache[key(ctx2, tail)]
        if (hit != null && hit.cands.isNotEmpty()) {
            val out = withFrozenAlternatives(hit.cands.map { fSurf + it }, hit.cands[0])
            lastTarget = target
            lastCands = out
            // 1 位に上げた直し (種類 8) は 1 位と同じ表記なので外さない (候補の帯で “ ” で囲む印に使う。2026-10-14)
            suggestions = hit.sugg.map { it.first to (if (it.first == 9) it.second else fSurf + it.second) }.filter { it.first == 8 || it.first == 9 || it.second != out.firstOrNull() }
            maybeFreeze(tail, hit)
            return out
        }
        request(ctx2, tail, onDone)
        // 結果が届くまでは、前回の候補に新しく打ったかなをつないで出す (かなに戻ったり、候補の帯が縮んだりしない)
        if (lastTarget.isNotEmpty() && target.startsWith(lastTarget) && lastCands.isNotEmpty()) {
            val extra = target.substring(lastTarget.length)
            return lastCands.map { it + extra }
        }
        return listOf(fSurf + tail)
    }

    /**
     * 候補の一覧: AI の上位 3 つ (前の方は固定したまま) → 前の方の 1 か所を持たせた候補に替えた文 (後ろは AI の 1 位)
     * → AI の残り。前の方を替えた文は、後ろの方の固定ほど先に (直したいのは、たいてい直前に固定された所)
     */
    private fun withFrozenAlternatives(full: List<String>, tailTop: String): List<String> {
        if (chunks.isEmpty()) return full
        val variants = ArrayList<String>()
        for (i in chunks.indices.reversed()) {
            if (variants.size >= 8) break
            for (j in 1 until chunks[i].cands.size) {
                variants.add(chunks.mapIndexed { idx, c -> if (idx == i) c.cands[j] else c.cands[0] }.joinToString("") + tailTop)
            }
        }
        return (full.take(3) + variants + full.drop(3)).distinct()
    }

    /** オート確定のとき: 固定した読みと表記 (呼ぶ側がこれを入力欄に確定して、残りだけを入力中に残す) */
    fun frozenKana() = fKana
    fun frozenSurf() = fSurf

    /**
     * 固定した部分 (読み fk、表記 fs) を確定したあと。前回の候補も、確定した分だけ前を削って残す
     * (消してしまうと、次の結果が届くまで残りがかなで出てちらつく)
     */
    fun clearFrozen(fk: String = "", fs: String = "") {
        fKana = ""
        fSurf = ""
        chunks = ArrayList()
        prevSegs.clear()
        if (fk.isNotEmpty() && lastTarget.startsWith(fk)) {
            lastTarget = lastTarget.substring(fk.length)
            lastCands = lastCands.filter { it.startsWith(fs) }.map { it.substring(fs.length) }
        } else {
            lastTarget = ""
            lastCands = emptyList()
        }
    }

    private fun maybeFreeze(tail: String, r: Engine.Result) {
        val top = r.cands[0]
        var cum = 0
        var freezeEnd = 0
        var freezeLen = 0
        val segs = HashMap<Int, String>()
        for (i in 0 until r.ends.size - 1) {   // 最後の語は固定しない
            cum += r.lens[i]
            val end = r.ends[i]
            val surf = top.substring(0, minOf(cum, top.length))
            segs[end] = surf
            val punct = "、。！？!?".indexOf(tail[end - 1]) >= 0
            val stable = prevSegs[end] == surf
            val wordsAfter = r.ends.size - 1 - i   // この語より後ろに残る語の数
            // 速くても、読みが長すぎるとモデルの入力の長さの上限に届いて変換できなくなるので、MAX_TAIL 字を超えたら固定する
            val slow = lastMs > BUDGET_MS || tail.length > MAX_TAIL
            val ready = if (liveCommit) end <= tail.length - LIVE_KEEP else slow && wordsAfter >= KEEP_WORDS
            if (ready && (stable || punct)) {
                freezeEnd = end
                freezeLen = cum
            }
        }
        prevSegs = segs
        if (freezeEnd > 0) {
            val kana = tail.substring(0, freezeEnd)
            // 固定する前の方にも、挨拶のかな書きを当てる (候補の並べ替えだけでは、ここで「今日は」が固定されてしまう)
            val surf = greetingFix(kana, top.substring(0, minOf(freezeLen, top.length)))
            val chunk = FrozenChunk(kana, listOf(surf))
            val ctx = (fCtx + fSurf).takeLast(40)
            chunks.add(chunk)
            fKana += kana
            fSurf += surf
            prevSegs.clear()
            if (liveCommit) return   // すぐ入力欄に確定されるので、選び直しの候補は要らない
            // 固定した部分だけを 1 回変換して、選び直せる候補を持たせる (以後この部分は AI にかけない)
            worker.execute {
                val alts = runCatching { engine.convert(ctx, kana, 8, true) }.getOrElse { emptyList() }
                main.post { chunk.cands = listOf(surf) + alts.filter { it != surf }.take(ALT - 1) }
            }
        }
    }

    /** 予測 (読みの続く辞書の語) */
    fun complete(prefix: String, max: Int) = engine.complete(prefix, max)

    /** 変換の結果の控えを捨てる (ユーザー辞書が変わって、同じ読みでも結果が変わるとき) */
    fun clearCache() {
        cache.clear()
    }

    // ---- 裏のスレッドでの変換 ----
    private val cache = HashMap<String, Engine.Result>()
    private var inflight = false
    private var wantKey = ""
    private var wantCtx = ""
    private var wantKana = ""
    private var wantCb: () -> Unit = {}

    private fun key(context: String, kana: String) = "$context|$kana|${keysFor(kana)?.first ?: ""}"

    /** 変換中にまた頼まれたら、終わったあとで最新の 1 つだけを変換する (古い読みの変換を溜めない) */
    private fun request(context: String, kana: String, onDone: () -> Unit) {
        wantKey = key(context, kana)
        wantCtx = context
        wantKana = kana
        wantCb = onDone
        if (!inflight) run()
    }

    private fun run() {
        inflight = true
        val key = wantKey
        val ctx = wantCtx
        val kana = wantKana
        val cb = wantCb
        val keys = keysFor(kana)
        worker.execute {
            val t = DoubleArray(5)
            val r = runCatching { engine.convertWithSegments(ctx, kana, CANDIDATES, true, t, keys) }
                .getOrElse { Engine.Result(emptyList(), IntArray(0), IntArray(0)) }
            main.post {
                lastMs = t.sum()
                if (cache.size > 256) cache.clear()
                cache[key] = r
                inflight = false
                if (wantKey != key) run()      // 変換中に読みが変わった: 最新の読みを変換する
                else cb()
            }
        }
    }

    companion object {
        private const val KEEP_WORDS = 4      // 固定しても、最後のこの語数は必ず AI に残す
        private const val BUDGET_MS = 80.0    // 変換 1 回がこれより速いうちは固定しない (遅れを感じない限界)
        private const val MAX_TAIL = 60
        private const val LIVE_KEEP = 8       // オート確定のときに、末尾に残す字数 (以前の決まり)
        private const val CANDIDATES = 30     // 10 では短い読み (き・かん) の単漢字が足りない
        private const val ALT = 4             // 固定した部分に持たせる候補の数

        /** 読み reading とその表記 surf の組で、挨拶の「今日は」をかな書きに直す (きょうは を打っていないときだけ) */
        fun greetingFix(reading: String, surf: String): String =
            if ("こんにちは" in reading && "きょうは" !in reading) surf.replace("今日は", "こんにちは") else surf
    }
}

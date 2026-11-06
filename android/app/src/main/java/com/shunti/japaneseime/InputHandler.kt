package com.shunti.japaneseime

import android.graphics.Color
import android.os.SystemClock
import android.text.SpannableString
import android.text.Spanned
import android.text.style.BackgroundColorSpan
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.ExtractedTextRequest
import android.view.inputmethod.InputConnection
import java.text.BreakIterator
import java.util.Calendar

/**
 * 入力の状態と操作 (HarmonyOS 版 ime/KeyboardController.ets の InputHandler の、AI 変換で使う部分の移植)。
 * 関数名と状態の決まりは向こうと同じ。向こうとの違い:
 *  - 辞書を使う機能 (ハイブリッド変換・予測変換・文節の編集) は持たない。変換は AI 変換だけ (+ ユーザー辞書)
 *  - 文脈は確定した文のかわりに、入力欄のカーソルの左をそのまま読む (Android では読めるので)
 *  - SELECTING (空白で候補を選んでいる) の間は、選んでいる候補を入力欄に出す
 */
class InputHandler(
    private val host: Host,
    private val tables: Tables,
    private val learning: Learning,
    val settings: Settings,
    private val userDict: UserDict,
) {

    interface Host {
        val ic: InputConnection?
        val editorInfo: EditorInfo?
        val ai: AiConverter?
        fun render()          // 候補の帯とキーの表示を出し直す
        fun post(r: () -> Unit)   // いまの処理が終わってから r を呼ぶ
        fun postDelayed(ms: Long, r: () -> Unit)   // ms ミリ秒の後に r を呼ぶ
        fun learningChanged()   // 学習が変わった: 2 回以上選んだ語を変換の網に入れ直す
    }

    /** BUNSETSU = 文節ごとに選ぶ (HarmonyOS 版と同じ。読み全体を AI 変換した 1 位の語の区切りで分ける) */
    enum class InputState { IDLE, COMPOSING, SELECTING, BUNSETSU }
    enum class InputMode { HIRAGANA, ALPHANUMERIC }
    enum class SubMode { KANA, NUMERIC, SYMBOL, EMOJI, MENU }

    var inputState = InputState.IDLE
        private set
    var inputMode = InputMode.HIRAGANA
        private set
    var subMode = SubMode.KANA
    var composingText = ""
        private set
    var candidates: List<String> = emptyList()
        private set
    var selectedCandidateIndex = 0
        private set
    var typoTop = ""   // 1 位にした打ち間違いの直し (候補の帯で “ ” で囲む)
    private var afterEmoji: List<String> = emptyList()   // 1 位の文の語に当たる絵文字・顔文字 (確定の後の帯に)
    var predictions: List<String> = emptyList()   // 確定のあとに出す候補 (絵文字)
        private set
    /** 提案 (種類, 確定する文)。設定「提案を分けて表示する」のとき、候補の帯の上の行に出す */
    var suggestions: List<Pair<Int, String>> = emptyList()
        private set
    private val glyphPaint = android.graphics.Paint()
    private val glyphOk = HashMap<Int, Boolean>()   // 字ごとの控え (候補の文全体で控えると、打つたびに増え続ける)
    /** この端末のフォントで出せる字か (豆腐にならないか)。変体仮名は Android 15 から出る端末がある */
    private fun renderable(s: String): Boolean {
        var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i)
            if ((cp in 0x1B000..0x1B16F || cp in 0x13000..0x1345F || cp in 0x1F000..0x1F0FF) &&
                !glyphOk.getOrPut(cp) { glyphPaint.hasGlyph(String(Character.toChars(cp))) }) return false
            i += Character.charCount(cp)
        }
        return true
    }

    private var conversionEnd = 0                  // 0 = 読みの全体を変換。それ以外は先頭からこの字数だけ
    private var ctx = ""                           // 変換の文脈 (入力を始めたときのカーソルの左 + その後に確定した分)
    private val composingUndoStack = ArrayList<String>()
    private val romaji = JapaneseConverter(tables.ROMAJI_TABLE)   // QWERTY でかなを打つとき
    // 打ったキー (QWERTY・GODAN のローマ字、フリックのかなはその字のまま)。エンジンに渡して英字の打ち分けに使う (PC と同じ、2026-10-24)。
    // 途中を書き換えたら (消す・小゛゜) 使えない
    private val rawKeys = StringBuilder()
    private var alphaRun = false   // 大文字で始めた英字の続き (かなにしない)
    private var keysValid = true

    /** 読み kana に合う打ったキーの並び (読みの字ごとの「その字までのキーの数」つき)。読みに英字が残るときだけ (PC の prime_keys と同じ) */
    fun keysFor(kana: String): Pair<String, IntArray>? {
        if (!keysValid || rawKeys.isEmpty() || kana.none { it in 'a'..'z' || it in 'A'..'Z' }) return null
        val conv = JapaneseConverter(tables.ROMAJI_TABLE)
        val sim = StringBuilder()
        val span = ArrayList<Int>()
        var k = 0
        for (ch in rawKeys) {
            val lk = ch.lowercaseChar()
            if (lk in 'a'..'z' || lk == '-' || lk == '\'') {
                k++
                val r = conv.processKey(ch.toString())
                val end = if (conv.pending().isEmpty()) k else k - 1
                for (c in r.committed) { sim.append(c); span.add(end) }
            } else {
                for (c in conv.flushPending()) { sim.append(c); span.add(k) }
                k++
                sim.append(ch); span.add(k)
            }
        }
        for (c in conv.flushPending()) { sim.append(c); span.add(k) }
        val s = sim.toString()
        val off = when {
            s.startsWith(kana) -> 0
            s.endsWith(kana) -> s.length - kana.length
            else -> return null
        }
        val ks = if (off == 0) 0 else span[off - 1]
        val ke = span[off + kana.length - 1]
        return rawKeys.substring(ks, ke) to IntArray(kana.length) { span[off + it] - ks }
    }

    /** まだかなに決まっていないローマ字 (QWERTY) */
    val pendingRomaji get() = romaji.pending()

    // kind: 0 入れた・1 消した・2 カーソルを動かした (at = 動かす前の位置)。t は続けて消した分をまとめるための時刻
    private class HostOp(val kind: Int, var text: String, var at: Int, var t: Long = System.currentTimeMillis()) {
        var learn: Learning.Undo? = null   // この確定で学習に足す前の値 (取消で戻す)
    }
    private val hostUndoStack = ArrayList<HostOp>()

    // ---------------------------------------------------------------- 入力欄の出入り
    /** 数字の入力欄 (電話番号・数値) か。数字はそのまま入れる (変換の候補を出さない) */
    private var numericField = false
    private var passwordField = false   // 伏せ字の欄。打った字のとおりに入れる (括弧を組にしない)
    private var privateField = false    // パスワードの欄と、アプリが学習しないよう指定した欄。打った文を端末に残さない
    private var fieldKind = 0           // 入力の種類 (class + variation)

    fun onInputStart(ei: EditorInfo?, restarting: Boolean) {
        val cls = (ei?.inputType ?: 0) and EditorInfo.TYPE_MASK_CLASS
        val variation = (ei?.inputType ?: 0) and EditorInfo.TYPE_MASK_VARIATION
        passwordField = (cls == EditorInfo.TYPE_CLASS_TEXT && variation in PASSWORD_VARIATIONS) ||
            (cls == EditorInfo.TYPE_CLASS_NUMBER && variation == EditorInfo.TYPE_NUMBER_VARIATION_PASSWORD)
        privateField = passwordField || ((ei?.imeOptions ?: 0) and EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING) != 0
        val sameKind = (cls or variation) == fieldKind
        fieldKind = cls or variation
        // ブラウザはページの中の欄の移動も restartInput で来る。種類が同じなら同じ欄として続ける (回転など)
        if (restarting && sameKind) {
            // やり直しの後に未確定の範囲が残っているかは分からない (範囲だけ消えて字が残る入力欄がある)
            fieldComposing = false
            if (inputState != InputState.IDLE) fieldHadComposing = true
            host.post { checkAfterRestart(retry = true) }
            return
        }
        SentenceFlow.reset()   // 別の欄に移った: 途中の節はつながらない
        if (restarting) host.ic?.finishComposingText()   // 種類だけ変わった欄でも、未確定の字は見えているまま残す
        resetComposition(touchField = !restarting)
        numericField = false
        predictions = emptyList()
        hostUndoStack.clear()
        when {
            cls == EditorInfo.TYPE_CLASS_NUMBER || cls == EditorInfo.TYPE_CLASS_PHONE || cls == EditorInfo.TYPE_CLASS_DATETIME -> {
                inputMode = InputMode.ALPHANUMERIC
                subMode = SubMode.NUMERIC
                numericField = true
            }
            cls == EditorInfo.TYPE_CLASS_TEXT && variation in ALPHA_VARIATIONS -> {
                inputMode = InputMode.ALPHANUMERIC
                subMode = SubMode.KANA
            }
            else -> {
                inputMode = InputMode.HIRAGANA
                subMode = SubMode.KANA
            }
        }
        host.render()
    }

    fun onInputStop() {
        resetComposition(touchField = false)   // 字は標準の処理が確定した。ここで消すと、範囲を付け直して消してしまう
        learning.save()
        SentenceFlow.save()
    }

    /**
     * キーボードだけを隠したとき (入力欄は続いている)。Android の標準の処理は入力中の文字をそのまま入力欄に確定するので、
     * その前に見えているとおりに自分で確定して、入力中の状態を空にする。
     * (空にしないと読みが残ったまま、次に打った字を足して入力中に出し直し「かに」+「かにあ」と二重になる)
     */
    fun onViewHidden() = commitShown()

    /** 入力中・選択中の残りを、いま見えている形で確定する (キーボードを閉じたとき・貼り付けの前) */
    fun commitShown() {
        if (inputState == InputState.SELECTING || inputState == InputState.BUNSETSU) commitTopCandidate()   // 選んでいる候補 (+ 範囲の外の残りのかな) が見えている
        if (inputState == InputState.COMPOSING) {
            if (liveTop() != null && romaji.pending().isEmpty()) commitTopCandidate() else commitRaw()   // Enter と同じく見えている変換で
        }
    }

    // ---------------------------------------------------------------- 入力欄の変化
    private var fieldComposing = false      // 入力欄が最後に知らせた未確定の範囲があるか
    private var fieldHadComposing = false   // 入力中に未確定の範囲を知らせてきた (知らせない入力欄では何もしないため)
    private var fieldCheckPending = false
    private var lastComposingEdit = 0L
    private var composingShown = ""         // 最後に入力欄へ書いた未確定の字

    /**
     * 入力欄の未確定の範囲が変わった (onUpdateSelection)。入力中なのに範囲が無くなったら、アプリが未確定の字を
     * 引き取った (送信ボタンで送って欄を空にしたなど) ので、入力中の状態と候補を捨てる。
     * 欄の読み取りは、それより前に送った書き込みの処理の後に動くので、自分の編集の遅れた通知でも誤らない。待たずに読む
     * (待つ間に変換の結果が届くと、アプリが空にした欄へ書き戻してしまう)
     */
    fun onFieldComposing(start: Int, end: Int) {
        fieldComposing = start >= 0 && end > start
        if (inputState == InputState.IDLE) {
            fieldHadComposing = false
        } else if (fieldComposing) {
            fieldHadComposing = true
        } else if (fieldHadComposing) {
            if (composingGone()) dropComposing() else scheduleFieldCheck(FIELD_CHECK_MS)
        }
    }

    /** 最後に書いた未確定の字がカーソルの前に無い。読めないときは false (今のまま) */
    private fun composingGone(): Boolean {
        if (composingShown.isEmpty()) return false
        val before = host.ic?.getTextBeforeCursor(composingShown.length, 0)?.toString() ?: return false
        return before != composingShown
    }

    private fun scheduleFieldCheck(ms: Long) {
        if (fieldCheckPending) return
        fieldCheckPending = true
        host.postDelayed(ms) {
            fieldCheckPending = false
            if (inputState != InputState.IDLE && fieldHadComposing && !fieldComposing) {
                val wait = lastComposingEdit + FIELD_CHECK_MS - SystemClock.uptimeMillis()
                if (wait > 0) {
                    scheduleFieldCheck(wait)   // 自分が書いた直後: その通知を待つ
                } else if (composingGone()) {
                    dropComposing()            // 字が残っていれば入力を続ける (範囲は reclaimComposing が付け直す)
                }
            }
        }
    }

    /**
     * 未確定の範囲だけが消えて字が欄に残ると、次の書き込みはカーソルの位置に足され「かに」+「かにあ」と二重になる。
     * 字がカーソルの前にそのままあれば、書き込む前に範囲を付け直す (範囲が残っていても同じ所を指すだけ)。
     * 範囲が無く字も無ければ、アプリが引き取った: false を返す (呼ぶ側は書き込まない)。読めないときは今のまま
     */
    private fun reclaimComposing(): Boolean {
        if (!fieldHadComposing || fieldComposing || composingShown.isEmpty()) return true
        val ic = host.ic ?: return true
        val before = ic.getTextBeforeCursor(composingShown.length, 0)?.toString() ?: return true
        if (before != composingShown) return false
        val at = cursor()
        if (at == null || !ic.setComposingRegion(at - composingShown.length, at)) ic.deleteSurroundingText(composingShown.length, 0)
        fieldComposing = true   // 次の通知が来るまで付け直さない (消して書き直す経路で、字が無いと誤らないように)
        return true
    }

    /** キーと確定の入口: アプリが字を引き取っていたら入力中の状態を捨てて true (古い読みを足したり、送った文を書き戻したりしない) */
    private fun dropIfTaken(): Boolean {
        if (inputState == InputState.IDLE || reclaimComposing()) return false
        dropComposing()
        return true
    }

    /**
     * 入力のやり直し (restartInput) の後、未確定の字が欄に残っているかを読む。Discord は送信のとき未確定の範囲の通知を
     * 送らず、やり直しだけが来る。待つと帯が残って見えるのですぐ読み、まだ残っていればもう 1 回だけ読む。読めないときは今のまま
     */
    private fun checkAfterRestart(retry: Boolean) {
        if (inputState == InputState.IDLE || composingShown.isEmpty()) return
        val before = host.ic?.getTextBeforeCursor(composingShown.length, 0)?.toString() ?: return
        if (before != composingShown) {
            dropComposing()
        } else if (retry) {
            host.postDelayed(RESTART_RETRY_MS) { checkAfterRestart(retry = false) }
        }
    }

    private fun dropComposing() {
        predictions = emptyList()
        resetComposition(touchField = false)
    }

    // ---------------------------------------------------------------- キー
    /** かな・数字のキー (フリックのかなキー、数字パッド) */
    fun handleDirectKana(kana: String) {
        dropIfTaken()
        // 英字 (と数字の入力欄の数字) は、入力中の文字があれば足して一緒に変換する (ぱちんこ + CR -> パチンコCR)。
        // 無ければそのまま入れる。数字パッドの数字は入力中の文字として組み立てる (2000 -> 2,000・二千)
        val direct = (inputMode == InputMode.ALPHANUMERIC && subMode != SubMode.NUMERIC) || numericField
        if (direct && composingText.isEmpty()) {
            commitPending()
            insertText(kana)
            return
        }
        if (inputState == InputState.SELECTING || inputState == InputState.BUNSETSU) commitTopCandidate()
        beginIfIdle()
        composingText += romaji.flushPending() + kana
        rawKeys.append(kana)
        alphaRun = false
        inputState = InputState.COMPOSING
        conversionEnd = 0
        updateCandidates()
    }

    /** 入力を始めるとき: 文脈 (カーソルの左) を読み、確定の後の帯を消す */
    private fun beginIfIdle() {
        if (composingText.isEmpty() && romaji.pending().isEmpty()) {
            ctx = host.ic?.getTextBeforeCursor(40, 0)?.toString() ?: ""
            rawKeys.setLength(0)   // 新しい入力: 打ったキーも始めから
            keysValid = true
            alphaRun = false
            predictions = emptyList()
        }
    }

    /**
     * QWERTY のキー。英数モードでは打った字をそのまま入れる。かなモードではローマ字をかなにする
     * (HarmonyOS 版 handleKeyPress と同じ: かなに決まった分を読みに足し、決まらない分は pendingRomaji に残す)
     */
    fun handleKeyPress(key: String) {
        dropIfTaken()
        if (inputMode == InputMode.ALPHANUMERIC) {
            // 入力中の文字があれば英字も足して一緒に変換する (ぱちんこ + CR -> パチンコCR)。無ければそのまま入れる (英文)
            if (composingText.isEmpty()) {
                commitPending()
                insertText(key)
                return
            }
            if (inputState == InputState.SELECTING || inputState == InputState.BUNSETSU) commitTopCandidate()
            composingText += romaji.flushPending() + key   // 打ちかけのローマ字は英字の前に
            inputState = InputState.COMPOSING
            conversionEnd = 0
            updateCandidates()
            return
        }
        if (inputState == InputState.SELECTING || inputState == InputState.BUNSETSU) commitTopCandidate()
        beginIfIdle()
        // 大文字: そこから英字のまま (PC 版の caps_alpha と同じ。小文字の続きも英字、空白・記号・かなで終わる。2026-10-24)
        if (key.length == 1 && (key[0] in 'A'..'Z' || (alphaRun && (key[0] in 'a'..'z' || key[0] in '0'..'9')))) {
            composingText += romaji.flushPending() + key
            rawKeys.append(key)
            alphaRun = true
            inputState = InputState.COMPOSING
            conversionEnd = 0
            updateCandidates()
            return
        }
        alphaRun = false
        val r = romaji.processKey(key)
        rawKeys.append(key)
        composingText += r.committed
        inputState = if (composingText.isNotEmpty() || r.pending.isNotEmpty()) InputState.COMPOSING else InputState.IDLE
        conversionEnd = 0
        updateCandidates()
    }

    /** 打ちかけのローマ字を読みに足す (語末の n は ん) */
    private fun flushRomaji(): Boolean {
        val p = romaji.flushPending()
        if (p.isEmpty()) return false
        composingText += p
        return true
    }

    /** 小゛゜ キー: 最後の 1 文字を順に切り替える */
    fun handleVariantCycle() {
        if (dropIfTaken()) return
        keysValid = false
        if (inputState != InputState.COMPOSING || composingText.isEmpty()) return
        val next = tables.VARIANT_CYCLE[composingText.takeLast(1)] ?: return
        composingText = composingText.dropLast(1) + next
        updateCandidates()
    }

    fun handleSpaceKey() {
        if (dropIfTaken()) return
        if (inputState == InputState.BUNSETSU) {   // 今の文節の候補を順に
            if (candidates.isEmpty()) return
            selectedCandidateIndex = (selectedCandidateIndex + 1) % candidates.size
            bChosen[bCur] = candidates[selectedCandidateIndex]
            showComposing()
            host.render()
            return
        }
        alphaRun = false
        if (inputState == InputState.COMPOSING && flushRomaji()) updateCandidates()
        if (inputState == InputState.IDLE || composingText.isEmpty()) {
            insertText(if (inputMode == InputMode.ALPHANUMERIC) " " else "　")
            return
        }
        if (inputState == InputState.COMPOSING && bunsetsuAvailable) {   // 空白キーの「変換」は「文節」に (ユーザー 2026-11-05)
            toggleBunsetsu()
            return
        }
        if (inputState == InputState.COMPOSING) {
            if (candidates.isEmpty()) return
            inputState = InputState.SELECTING
            selectedCandidateIndex = 0
        } else {
            selectedCandidateIndex = (selectedCandidateIndex + 1) % maxOf(1, candidates.size)
        }
        showComposing()
        host.render()
    }

    fun handleBackspace() {
        if (dropIfTaken()) return
        when (inputState) {
            InputState.IDLE -> deleteBeforeCursor()
            InputState.BUNSETSU -> leaveBunsetsu()
            InputState.SELECTING -> {
                inputState = InputState.COMPOSING
                showComposing()
                host.render()
            }
            InputState.COMPOSING -> {
                if (romaji.pending().isNotEmpty()) {
                    romaji.backspace()
                    if (rawKeys.isNotEmpty()) rawKeys.setLength(rawKeys.length - 1)
                } else if (composingText.isNotEmpty()) {
                    keysValid = false
                    composingUndoStack.add(composingText.takeLast(1))
                    composingText = composingText.dropLast(1)
                }
                if (conversionEnd > composingText.length) conversionEnd = 0
                if (composingText.isEmpty() && romaji.pending().isEmpty()) {
                    inputState = InputState.IDLE
                    reclaimComposing()
                    composingShown = ""
                    host.ic?.setComposingText("", 1)
                    host.ic?.finishComposingText()
                    candidates = emptyList()
                    host.render()
                    return
                }
                updateCandidates()
            }
        }
    }

    fun handleEnterKey() {
        if (dropIfTaken()) return   // 「確定」のキーが、そのまま送信にならないように
        when (inputState) {
            InputState.SELECTING, InputState.BUNSETSU -> commitTopCandidate()
            InputState.COMPOSING -> if (liveTop() != null && romaji.pending().isEmpty()) commitTopCandidate() else commitRaw(confirm = true)   // ライブ変換は見えている変換で確定
            InputState.IDLE -> {
                val ic = host.ic ?: return
                val action = enterAction()
                if (action != null) {
                    ic.performEditorAction(action)
                } else if (multiLine()) {
                    // 複数行の欄: 改行の字を入れる (Enter のキーを送っても改行しない欄があった。ユーザー 2026-10-23)
                    ic.commitText("\n", 1)
                } else {
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
                }
                SentenceFlow.boundary()   // 送信・改行で文が終わった
                predictions = emptyList()
                host.render()
            }
        }
    }

    private fun multiLine(): Boolean =
        ((host.editorInfo?.inputType ?: 0) and android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0

    /** Enter で行う動作 (送信・検索など)。null なら改行。複数行の欄では、送信・検索・移動のはっきりした指定のときだけ動作にし、
     *  ほかの指定 (完了・次へ) は改行にする (メモ帳などで Enter が改行にならなかった) */
    private fun enterAction(): Int? {
        val ei = host.editorInfo ?: return null
        if ((ei.imeOptions and EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0) return null
        val action = ei.imeOptions and EditorInfo.IME_MASK_ACTION
        if (action == EditorInfo.IME_ACTION_NONE || action == EditorInfo.IME_ACTION_UNSPECIFIED) return null
        if (multiLine() && action != EditorInfo.IME_ACTION_SEND && action != EditorInfo.IME_ACTION_SEARCH && action != EditorInfo.IME_ACTION_GO) return null
        return action
    }

    /** 確定キーの表示 (入力中は「確定」、そうでなければ入力欄の指定に合わせる) */
    fun enterLabel(): String {
        if (inputState != InputState.IDLE) return "確定"
        return when (enterAction()) {
            EditorInfo.IME_ACTION_SEND -> "送信"
            EditorInfo.IME_ACTION_SEARCH -> "検索"
            EditorInfo.IME_ACTION_GO -> "移動"
            EditorInfo.IME_ACTION_NEXT -> "次へ"
            EditorInfo.IME_ACTION_DONE -> "完了"
            else -> "改行"
        }
    }

    /** かなのまま確定。confirm = Enter で確定した: その読みに学習があれば、かなも 1 回分数える (PC の confirm と同じ) */
    fun commitRaw(confirm: Boolean = false) {
        if (dropIfTaken()) return
        flushRomaji()
        if (composingText.isNotEmpty()) {
            insertLearned(if (confirm && !privateField) learning.confirmKana(composingText, ctx) else null, composingText, composingText)
        }
        resetComposition()
    }

    // 確定した直後に全部消して、10 秒以内に同じ読みで別の表記を確定したら、前の表記は間違いだった (PC 版と同じ。
    // ユーザー 2026-11-06。PC の入力ログで 23 件中 22 件が 10 秒以内)。同じ表記を打ち直したときは何もしない
    private class Retype(val reading: String, val surface: String, val left: String, var chars: Int, val at: Long) { var erased = false }
    private var retype: Retype? = null

    /**
     * text を確定する。undo = 学習の前の値: 取消キーで学習も戻せるように、入れた字の記録に付ける。
     * reading = 確定した文の読み (打ち直しの判定に使う。部分の確定は null)
     */
    private fun insertLearned(undo0: Learning.Undo?, text: String, reading: String? = null) {
        val r = retype
        val demoted = if (reading != null && r != null && r.erased && !privateField && SystemClock.uptimeMillis() - r.at <= RETYPE_MS &&
            reading == r.reading && text != r.surface) learning.demote(r.reading, r.surface, r.left) else null
        val undo = learning.join(demoted, undo0)
        val last = hostUndoStack.lastOrNull()
        val left = ctx
        insertText(text)
        if (reading != null && reading.isNotEmpty() && text.isNotEmpty()) retype = Retype(reading, text, left, text.length, SystemClock.uptimeMillis())
        if (undo == null) return
        hostUndoStack.lastOrNull()?.takeIf { it !== last }?.learn = undo
        saveLearningSoon()
    }

    private var learningSavePending = false

    /** 確定が続く間もプロセスが落ちると学習が消えるので、少し待ってまとめて書く */
    private fun saveLearningSoon() {
        host.learningChanged()
        if (learningSavePending) return
        learningSavePending = true
        host.postDelayed(LEARNING_SAVE_MS) {
            learningSavePending = false
            learning.save()
        }
    }

    /** 候補の帯をタップ */
    /** 提案を選んだ: その文で確定する (打ったとおりの変換ではないので学習しない) */
    fun commitSuggestion(index: Int) {
        if (dropIfTaken()) return
        val sg = suggestions.getOrNull(index) ?: return
        if (!privateField) settings.recordSuggestUse(sg.second)
        // 気分の絵文字・顔文字 (6) は絵文字だけが来る: いま見えている 1 位の後ろに足す
        val text = if (sg.first == 6) (candidates.firstOrNull() ?: "") + sg.second else sg.second
        val (_, tail) = splitAtConversionEnd(composingText)
        insertText(text)
        ctx += text
        suggestions = emptyList()
        if (tail.isNotEmpty()) {
            continueWithTail(tail)
            return
        }
        resetComposition()
    }

    fun commitCandidate(index: Int) {
        if (dropIfTaken()) return
        if (inputState == InputState.BUNSETSU) {
            pickBunsetsu(index)
            return
        }
        val text = candidates.getOrNull(index)
        if (text.isNullOrEmpty()) {
            resetComposition()
            return
        }
        commitChosen(text)
    }

    /** いま選んでいる候補で確定 (候補が無ければかなのまま) */
    fun commitTopCandidate() {
        if (dropIfTaken()) return
        if (inputState == InputState.BUNSETSU) {
            commitBunsetsu()
            return
        }
        val text = candidates.getOrNull(selectedCandidateIndex)
        if (text.isNullOrEmpty()) {
            commitPending()
            return
        }
        commitChosen(text)
    }

    /** 1 位以外を選んだ: 選び直した語だけを語として数える (読み全体だけだと次の文に効かない) */
    private fun learnCommit(reading: String, text: String): Learning.Undo? {
        val words = host.ai?.changedWords(reading, text)?.filter { it.reading != reading }.orEmpty()
        if (words.isEmpty()) return if (reading.length <= MAX_WHOLE_LEARN_LEN) learning.recordChoice(reading, text, ctx) else null
        val done = words.mapNotNull { learning.recordChoice(it.reading, it.surface, it.left) }
        val whole = if (reading.length <= MAX_WHOLE_LEARN_LEN) learning.recordChoice(reading, text, ctx) else null
        return learning.undoOf(done + listOfNotNull(whole))
    }

    private fun commitChosen(text: String) {
        val (reading, tail) = splitAtConversionEnd(composingText)
        insertLearned(if (privateField || isNonLearningCandidate(reading, text)) null else learnCommit(reading, text), text, if (tail.isEmpty()) reading else null)
        if (!privateField) host.ai?.aiLearn(ctx, reading, text)   // AI学習 (設定でオンのときだけ学ぶ)
        ctx += text
        if (tail.isNotEmpty()) {
            continueWithTail(tail)
            return
        }
        resetComposition()
        pushPredictions(text)
    }

    private fun commitPending() {
        flushRomaji()
        if (composingText.isNotEmpty()) insertText(composingText)
        resetComposition()
    }

    /** 確定の後の帯の候補をタップ */
    fun commitPrediction(index: Int) {
        val text = predictions.getOrNull(index) ?: return
        insertText(text, raw = true)
        predictions = afterInsertStrip(emptyList())
        host.render()
    }

    /** 変換範囲を縮めて確定したときの残りを、続けて変換できる状態にする */
    private fun continueWithTail(tail: String) {
        composingUndoStack.clear()
        composingText = tail
        conversionEnd = 0
        inputState = InputState.COMPOSING
        selectedCandidateIndex = 0
        updateCandidates()
    }

    /** AI 変換で固定された前の方 (読み fk -> 表記 fs) を確定し、残りのかなを入力中に残す */
    private fun autoCommitFrozen(target: String, fk: String, fs: String) {
        // 速く打つと、ここに来るまでに入力がもう進んでいる。固定した読みが頭に残っていれば確定してよい
        if (fk.isEmpty() || !composingText.startsWith(fk) || !target.startsWith(fk) || isRangeShrunk()) return
        if (inputState != InputState.COMPOSING) return
        // 確定した文節も数える (PC の固定した文節と同じ。学習のある読みだけ)
        var left = ctx
        val done = if (privateField) emptyList() else host.ai?.frozenChunks().orEmpty().mapNotNull { (k, s) ->
            learning.confirm(k, s, left).also { left += s }
        }
        insertLearned(learning.undoOf(done), fs)
        if (!privateField) host.ai?.aiLearn(ctx, fk, fs)
        ctx += fs
        host.ai?.clearFrozen(fk, fs)
        continueWithTail(composingText.substring(fk.length))
    }

    /** touchField = false: 入力欄の字はアプリが片付けたので、こちらでは消し直さない (送った字やほかの字を消さないため) */
    private fun resetComposition(touchField: Boolean = true) {
        romaji.reset()
        rawKeys.setLength(0)
        keysValid = true
        suggestions = emptyList()
        if (touchField && (composingText.isNotEmpty() || inputState != InputState.IDLE)) {
            reclaimComposing()
            host.ic?.setComposingText("", 1)
            host.ic?.finishComposingText()
        }
        composingShown = ""
        fieldHadComposing = false
        composingUndoStack.clear()
        composingText = ""
        conversionEnd = 0
        candidates = emptyList()
        selectedCandidateIndex = 0
        bSegs.clear()
        bChosen.clear()
        bCur = 0
        inputState = InputState.IDLE
        host.render()
    }

    fun handleCursorLeft() {
        if (inputState == InputState.BUNSETSU) {
            selectBunsetsu(bCur - 1)
            return
        }
        if (inputState != InputState.IDLE && composingText.isNotEmpty()) {
            if (conversionEnd == 0) conversionEnd = composingText.length
            if (conversionEnd > 1) {
                conversionEnd--
                updateCandidates()
            }
        } else {
            moveCursor(KeyEvent.KEYCODE_DPAD_LEFT)
        }
    }

    fun handleCursorRight() {
        if (inputState == InputState.BUNSETSU) {
            selectBunsetsu(bCur + 1)
            return
        }
        if (inputState != InputState.IDLE && composingText.isNotEmpty()) {
            if (conversionEnd > 0 && conversionEnd < composingText.length) {
                conversionEnd++
                if (conversionEnd == composingText.length) conversionEnd = 0
                updateCandidates()
            }
        } else {
            moveCursor(KeyEvent.KEYCODE_DPAD_RIGHT)
        }
    }

    /** あA キー: 日本語 → 英字 (→ 数字パッド。設定でオンのとき) → 日本語 (HarmonyOS 版 cycleInputMode と同じ) */
    // ---------------------------------------------------------------- 文節 (HarmonyOS 版 handleBunsetsu と同じ動き)
    private val bSegs = ArrayList<String>()     // 文節ごとの読み
    private val bChosen = ArrayList<String>()   // 文節ごとの今の表記
    private var bCur = 0

    /** 空白キーで文節に分けられるか (2 字以上の入力中で、AI 変換が使えるとき) */
    val bunsetsuAvailable: Boolean
        get() = host.ai != null && composingText.length >= 2 && inputState == InputState.COMPOSING

    /** 空白キーの表示: 何も打っていなければ idle、入力中は「文節」(分けられないときと、文節の中は「変換」) */
    fun spaceLabel(idle: String): String = when {
        inputState == InputState.IDLE -> idle
        bunsetsuAvailable -> "文節"
        else -> "変換"
    }

    /** 入力中なら文節に分ける。文節の中なら読みに戻す */
    private fun toggleBunsetsu() {
        if (dropIfTaken()) return
        if (inputState == InputState.BUNSETSU) {
            leaveBunsetsu()
            return
        }
        if (inputState != InputState.COMPOSING && inputState != InputState.SELECTING) return
        flushRomaji()
        val ai = host.ai ?: return
        if (composingText.isEmpty()) return
        splitBunsetsu(ai, composingText)
        conversionEnd = 0
        suggestions = emptyList()
        typoTop = ""
        bCur = 0
        inputState = InputState.BUNSETSU
        showBunsetsu()
    }

    /**
     * 文節に分ける: 読み全体を AI 変換して、1 位の候補の語の区切りで分ける。かなだけの語 (助詞・助動詞・送り) は
     * 前の語につなげる (しゃしん|を|とっ|た -> 写真を|撮った)。区切りが取れなければ全体を 1 文節に
     */
    private fun splitBunsetsu(ai: AiConverter, reading: String) {
        val r = ai.convertNow(ctx, reading, 1, keys = keysValid)
        val top = r.cands.firstOrNull() ?: reading
        bSegs.clear()
        bChosen.clear()
        // 数字にする・打ち間違いを直すなどで 1 位が差し替わると、語の区切りが 1 位と合わない。切ると字が欠けるので 1 文節に
        if (r.ends.isEmpty() || r.lens.sum() != top.length) {
            bSegs.add(reading)
            bChosen.add(top)
            return
        }
        var rs = 0
        var ss = 0
        for (i in r.ends.indices) {
            val end = r.ends[i].coerceIn(rs, reading.length)
            val seg = reading.substring(rs, end)
            val surf = top.substring(ss, ss + r.lens[i])
            rs = end
            ss += r.lens[i]
            if (bSegs.isNotEmpty() && surf.isNotEmpty() && surf.all { it in 'ぁ'..'ゖ' || it == 'ー' } && seg !in BUNSETSU_PUNCT) {
                bSegs[bSegs.size - 1] += seg
                bChosen[bChosen.size - 1] += surf
            } else {
                bSegs.add(seg)
                bChosen.add(surf)
            }
        }
        if (rs < reading.length) {   // 区切りが読みの終わりまで届いていなければ、残りを最後の文節に
            bSegs[bSegs.size - 1] += reading.substring(rs)
            bChosen[bChosen.size - 1] += reading.substring(rs)
        }
    }

    /** i 番目の文節の候補: 今の表記を先頭に、前の文節 (今の表記) を文脈にして AI 変換した候補 */
    private fun bunsetsuCandidates(i: Int): List<String> {
        val seg = bSegs[i]
        val left = ctx + bChosen.take(i).joinToString("")
        val out = LinkedHashSet<String>()
        out.add(bChosen[i])
        host.ai?.convertNow(left, seg, 30)?.cands?.forEach { if (renderable(it)) out.add(it) }
        out.add(seg)
        out.add(toKatakana(seg))
        return out.toList()
    }

    private fun showBunsetsu() {
        candidates = bunsetsuCandidates(bCur)
        selectedCandidateIndex = 0
        showComposing()
        host.render()
    }

    private fun selectBunsetsu(i: Int) {
        if (i < 0 || i >= bSegs.size) return
        bCur = i
        showBunsetsu()
    }

    /** 今の文節に候補を選んで、次の文節へ (最後の文節なら全部を確定) */
    private fun pickBunsetsu(index: Int) {
        candidates.getOrNull(index)?.let { bChosen[bCur] = it }
        if (bCur + 1 < bSegs.size) selectBunsetsu(bCur + 1) else commitBunsetsu()
    }

    /** 文節をやめて、読みの入力に戻す */
    private fun leaveBunsetsu() {
        bSegs.clear()
        bChosen.clear()
        bCur = 0
        inputState = InputState.COMPOSING
        selectedCandidateIndex = 0
        updateCandidates()
    }

    /** 文節を全部つないで確定する。文節ごとに学習する (かなのままの文節は、その読みの学習があるときだけ数える) */
    private fun commitBunsetsu() {
        val text = bChosen.joinToString("")
        val reading = bSegs.joinToString("")
        val done = ArrayList<Learning.Undo>()
        if (!privateField) {
            var left = ctx
            for (i in bSegs.indices) {
                val u = if (bSegs[i] == bChosen[i]) learning.confirm(bSegs[i], bChosen[i], left) else learning.recordChoice(bSegs[i], bChosen[i], left)
                if (u != null) done.add(u)
                left += bChosen[i]
            }
        }
        insertLearned(if (privateField) null else learning.undoOf(done), text, reading)
        if (!privateField) host.ai?.aiLearn(ctx, reading, text)
        ctx += text
        resetComposition()
        pushPredictions(text)
    }

    fun cycleInputMode() {
        // 入力中の文字は確定しない (英字・数字も続けて足して一緒に変換する: ぱちんこ + CR、2000 + えん)
        when {
            subMode == SubMode.NUMERIC -> {
                inputMode = InputMode.HIRAGANA
                subMode = SubMode.KANA
            }
            inputMode == InputMode.ALPHANUMERIC -> {
                if (settings.numericPad) subMode = SubMode.NUMERIC
                else inputMode = InputMode.HIRAGANA
            }
            else -> inputMode = InputMode.ALPHANUMERIC
        }
        host.render()
    }

    /** 表示だけ出し直す (キーボードの種類を変えたときなど) */
    fun updateView() = host.render()

    /** 記号・絵文字の帯からの入力 */
    fun insertSymbol(sym: String) {
        commitTopCandidate()
        insertText(sym)
    }

    // ---------------------------------------------------------------- 取消
    /**
     * 取消キー。入力中ならその場で 1 文字ずつ戻す。確定済みなら、入力欄への直前の操作 (入れた・消した) を
     * 起きた位置まで戻って取り消す (本当の元に戻すと同じ順序で遡る)
     */
    fun undoLastDeletion() {
        if (dropIfTaken()) return
        if (inputState != InputState.IDLE) {
            val ch = composingUndoStack.removeLastOrNull() ?: return
            if (inputState == InputState.BUNSETSU) leaveBunsetsu()
            if (inputState == InputState.SELECTING) inputState = InputState.COMPOSING   // 候補を確定せず、読みへ戻す
            handleDirectKana(ch)
            return
        }
        retype = null
        val op = hostUndoStack.removeLastOrNull() ?: return
        val ic = host.ic ?: return
        ic.beginBatchEdit()
        if (op.kind == 2) {
            ic.setSelection(op.at, op.at)
        } else if (op.kind == 0) {
            // 入れた字の直後へ動いて、その字が本当にそこにあるときだけ消す
            ic.setSelection(op.at + op.text.length, op.at + op.text.length)
            if (ic.getTextBeforeCursor(op.text.length, 0)?.toString() == op.text) {
                ic.deleteSurroundingText(op.text.length, 0)
                // その確定で覚えたことも戻す (PC と同じ)。節が閉じて数えた文の流れは戻らない
                op.learn?.let { learning.restore(it); saveLearningSoon() }
                repeat(op.text.length) { SentenceFlow.backspace() }
            }
        } else {
            ic.setSelection(op.at, op.at)
            ic.commitText(op.text, 1)
        }
        ic.endBatchEdit()
    }

    // ---------------------------------------------------------------- 入力欄への出力
    /** 入力欄に文字を確定する (出口はここ 1 箇所。取消のための記録もここ)。raw = 貼り付けなど: 括弧を組にしない */
    fun insertText(text: String, raw: Boolean = false) {
        val ic = host.ic ?: return
        retype = null           // 打ち直しの判定は insertLearned が付け直す
        pasteFrom = null        // 自分で入れた (アプリに頼んだ貼り付けが失敗して自分で入れたときも、ここで記録する)
        reclaimComposing()      // 確定は未確定の範囲を置き換える。範囲が無いと見えている字の後ろに足されてしまう
        composingShown = ""     // ここから先の字は確定済み (同じ字でも付け直さない)
        // 開き括弧 (記号の欄の 1 字・かっこ の変換の組) は閉じと組で入れて、カーソルを中へ (HarmonyOS 版 組で。2026-10-14)
        val close = if (raw || passwordField) null   // 伏せ字の欄では、勝手に足した閉じ括弧に気づけない
            else Brackets.PAIRS[text] ?: if (text.length == 2 && Brackets.PAIRS[text.substring(0, 1)] == text.substring(1)) "" else null
        if (close != null) {
            val pair = if (close.isEmpty()) text else text + close
            ic.beginBatchEdit()
            ic.commitText(pair, 1)
            val at = cursor()
            at?.let { ic.setSelection(it - 1, it - 1); pushHostOp(true, pair, it - pair.length) }
            ic.endBatchEdit()
            if (!privateField) SentenceFlow.feed(pair.substring(0, 1))
            return
        }
        ic.commitText(text, 1)
        cursor()?.let { pushHostOp(true, text, it - text.length) }
        // 文の流れ (SentenceFlow) に入れる。@ を入れた (あっと の変換・記号) ときはメールのドメインの続き、ほかは文の流れの続きを
        // 確定の後の帯に出す (ユーザー 2026-10-13)。変換の確定のときは、この後の pushPredictions がまとめて出し直す
        if (!privateField) SentenceFlow.feed(text)
        val st = afterInsertStrip(emptyList())
        if (st.isNotEmpty()) { predictions = st; host.render() }
    }

    /** 確定の後の帯: メールのドメインの続き > 文の流れの続き > base (絵文字など) */
    private fun afterInsertStrip(base: List<String>): List<String> {
        val es = emailStrip()
        lastEmailStrip = es
        if (es.isNotEmpty()) return es
        return (SentenceFlow.predict() + base).distinct().take(16)
    }

    private var lastEmailStrip: List<String> = emptyList()

    /** 入力欄のカーソルの前が「…@打ちかけのドメイン」なら、ドメインの続き (gmail.com …) */
    private fun emailStrip(): List<String> {
        val before = host.ic?.getTextBeforeCursor(64, 0)?.toString() ?: return emptyList()
        val m = EMAIL_TAIL.find(before) ?: return emptyList()
        val typed = m.groupValues[1].lowercase()
        return EMAIL_DOMAINS.filter { it.startsWith(typed) && it != typed }.take(6).map { it.substring(typed.length) }
    }

    private fun deleteBeforeCursor() {
        deleteBeforeCursorRaw()
        // メールのドメインを出していたら、消した後の形で出し直す (@ を消したら帯を空に。ユーザー 2026-10-13)
        if (lastEmailStrip.isNotEmpty() && predictions == lastEmailStrip) {
            predictions = emailStrip()
            lastEmailStrip = predictions
        }
    }

    private fun deleteBeforeCursorRaw() {
        SentenceFlow.backspace()
        val ic = host.ic ?: return
        val sel = ic.getSelectedText(0)
        if (!sel.isNullOrEmpty()) {
            retype = null
            val at = cursor()
            ic.commitText("", 1)
            at?.let { pushHostOp(false, sel.toString(), it) }
            return
        }
        // 最後の 1 文字 (絵文字の合成列なども 1 文字として) を消す
        val before = ic.getTextBeforeCursor(16, 0)?.toString() ?: ""
        if (before.isEmpty()) {
            // 読めない入力欄 (や先頭): キーを送って入力欄に任せる
            ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL))
            ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL))
            return
        }
        val bi = BreakIterator.getCharacterInstance()
        bi.setText(before)
        val start = bi.preceding(before.length).coerceAtLeast(0)
        val ch = before.substring(start)
        val at = cursor()
        // 確定した字を後ろから消している (確定した分より多く消したら打ち直しではない)
        retype?.let { r -> if (r.erased) retype = null else { r.chars -= ch.length; if (r.chars <= 0) r.erased = true } }
        ic.deleteSurroundingText(ch.length, 0)
        at?.let { pushHostOp(false, ch, it - ch.length) }
        predictions = emptyList()
        host.render()
    }

    // アプリに貼り付けを頼んだときの、頼む前のカーソルの位置 (アプリが入れた字を、取消キーで戻せるように後から記録する)
    private var pasteFrom: Int? = null
    private var pasteUntil = 0L

    /** アプリに貼り付けを頼む直前に呼ぶ。入った字は、カーソルが前へ動いたとき (onHostSelection) に記録する */
    fun expectAppPaste() {
        pasteFrom = cursor()
        pasteUntil = SystemClock.uptimeMillis() + PASTE_WAIT_MS
    }

    /**
     * 入力欄のカーソルが動いた (onUpdateSelection)。アプリに頼んだ貼り付けなら、入った字を取消の記録に積む。
     * 直前の確定の遅れた通知 (位置が動いていない) は飛ばして待つ。待つのは PASTE_WAIT_MS まで (後で指で動かした分を数えない)
     */
    fun onHostSelection(start: Int, end: Int) {
        val from = pasteFrom ?: return
        if (SystemClock.uptimeMillis() > pasteUntil) { pasteFrom = null; return }
        if (start != end || start <= from) return
        pasteFrom = null
        val text = host.ic?.getTextBeforeCursor(start - from, 0)?.toString() ?: return
        if (text.length == start - from) pushHostOp(true, text, from)
    }

    private fun pushHostOp(insert: Boolean, text: String, at: Int) {
        if (text.isEmpty() || at < 0) return
        val now = System.currentTimeMillis()
        val last = hostUndoStack.lastOrNull()
        // ⌫ を続けて押して消した分は 1 回にまとめる (取消キー 1 回で消した語ごと戻る。HarmonyOS 版と同じ、2026-10-14)
        if (!insert && last != null && last.kind == 1 && last.at == at + text.length && now - last.t < UNDO_MERGE_MS) {
            last.text = text + last.text
            last.at = at
            last.t = now
            return
        }
        hostUndoStack.add(HostOp(if (insert) 0 else 1, text, at))
        if (hostUndoStack.size > MAX_HOST_UNDO) hostUndoStack.removeAt(0)
    }

    /** 入力欄のカーソルの位置 (読めなければ null) */
    private fun cursor(): Int? {
        val et = host.ic?.getExtractedText(ExtractedTextRequest().apply { hintMaxChars = 0 }, 0) ?: return null
        if (et.selectionStart < 0) return null
        return et.startOffset + et.selectionStart
    }

    private fun moveCursor(code: Int) {
        val ic = host.ic ?: return
        // ◄► の移動も取消キーで戻せるように (続けて動かした分は 1 つ、動かす前の位置を残す)
        cursor()?.let { from -> if (hostUndoStack.lastOrNull()?.kind != 2) hostUndoStack.add(HostOp(2, "", from)) }
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
    }

    /** 入力中の文字列を入力欄に出す: 読み (変換範囲を縮めていればその部分に色) か、選んでいる候補 + 残りのかな */
    /** ライブ変換で見せる 1 位 (読み全体を変換した候補があるとき)。無ければ null (かなのまま見せる) */
    private fun liveTop(): String? =
        if (settings.liveDisplay && inputState == InputState.COMPOSING && conversionEnd == 0 && composingText.isNotEmpty())
            candidates.firstOrNull()?.takeIf { it.isNotEmpty() } else null

    private fun showComposing() {
        val ic = host.ic ?: return
        lastComposingEdit = SystemClock.uptimeMillis()
        if (!reclaimComposing()) {   // 遅れて届いた変換の結果: アプリが空にした欄へ書き戻さない
            dropComposing()
            return
        }
        if (inputState == InputState.BUNSETSU) {   // 文節をつないで、今の文節に色を付ける
            composingShown = bChosen.joinToString("")
            val s = SpannableString(composingShown)
            val at = bChosen.take(bCur).sumOf { it.length }
            val len = bChosen.getOrNull(bCur)?.length ?: 0
            if (len > 0) s.setSpan(BackgroundColorSpan(RANGE_BG), at, at + len, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            ic.setComposingText(s, 1)
            return
        }
        if (inputState == InputState.SELECTING) {
            val (_, tail) = splitAtConversionEnd(composingText)
            composingShown = (candidates.getOrNull(selectedCandidateIndex) ?: "") + tail
            ic.setComposingText(composingShown, 1)
            return
        }
        val live = liveTop()
        val shown = (live ?: composingText) + romaji.pending()   // 打ちかけのローマ字も後ろに出す
        composingShown = shown
        if (live == null && isRangeShrunk()) {
            val s = SpannableString(shown)
            s.setSpan(BackgroundColorSpan(RANGE_BG), 0, conversionEnd, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            ic.setComposingText(s, 1)
        } else {
            ic.setComposingText(shown, 1)
        }
    }

    // ---------------------------------------------------------------- 候補
    private fun isRangeShrunk() = conversionEnd in 1 until composingText.length

    private fun splitAtConversionEnd(composing: String): Pair<String, String> {
        val end = if (isRangeShrunk()) conversionEnd else composing.length
        return composing.substring(0, end) to composing.substring(end)
    }

    private fun conversionTarget() = splitAtConversionEnd(composingText).first

    /** 予測 (読みの続きの語・決まり文句) で出した候補。読みが打った分より長いので、打った読みでは学習しない */
    private var completions: Set<String> = emptySet()

    /** 日付・数字の書き換え・絵文字・予測は、その場の値や記号や読みの違う語なので学習しない */
    private fun isNonLearningCandidate(reading: String, text: String): Boolean =
        text in completions ||
            text in DateTimePredictor.predict(reading, Calendar.getInstance()) ||
            text in NumberFormatter.predict(reading) ||
            tables.EMOJI_MAP[reading]?.contains(text) == true ||
            tables.KAOMOJI_MAP[reading]?.contains(text) == true

    fun updateCandidates() {
        if (inputState == InputState.BUNSETSU) return   // 文節の候補は文節ごとに出す (裏の変換の結果が届いても上書きしない)
        if (composingText.isEmpty()) {
            candidates = emptyList()
            suggestions = emptyList()
            showComposing()   // ローマ字の打ちかけだけのとき
            host.render()
            return
        }
        val target = conversionTarget()
        val fullKatakana = toKatakana(target)
        val ai = host.ai
        var cands = ArrayList<String>()
        if (ai != null) {
            ai.liveCommit = settings.liveCommit
            cands.addAll(ai.candidates(ctx, target) { if (conversionTarget() == target) updateCandidates() })
            // 長い入力で前の方が固定されたとき: 既定 (保留) では入力欄に確定しない (固定した部分に候補を持たせ、一覧から選び直せる)。
            // 設定「オート確定」がオンなら、固定した所を入力欄に確定して、残りだけを入力中に残す
            val fk = ai.frozenKana()
            if (ai.liveCommit && fk.isNotEmpty() && !isRangeShrunk()) {
                val fs = ai.frozenSurf()
                host.post { autoCommitFrozen(target, fk, fs) }
            }
        }
        if (fullKatakana !in cands) cands.add(fullKatakana)
        if (target !in cands) cands.add(target)
        // 日付・時刻と数字の書き換えは 1 位のすぐ後ろに
        var insAt = if (cands.isNotEmpty()) 1 else 0
        for (d in DateTimePredictor.predict(target, Calendar.getInstance()) + NumberFormatter.predict(target)) {
            if (d !in cands) cands.add(insAt++, d)
        }
        // 括弧は全種類を必ず出す (Brackets.kt)。括弧 1 字を打ったときは同じ側の全種類を先頭から (打った字が 1 位)、
        // かっこ などの読みでは全種類の組を 1 位の後ろに足す
        val brackets = Brackets.variants(target)
        if (brackets.firstOrNull() == target) {
            cands = ArrayList(brackets + cands.filter { it !in brackets })
        } else {
            // 辞書に下の方で入っている組 (（）「」 など) も引き上げる
            for (b in brackets) {
                val at = cands.indexOf(b)
                if (at in 0 until insAt) continue
                if (at >= 0) cands.removeAt(at)
                cands.add(insAt++, b)
            }
        }
        preferGreetings(target, cands)
        // 予測 (HarmonyOS 版と同じ置き方): 決まり文句は先頭に、読みの続く語 (学習した語・辞書の語) は 1 位の直後に 3 つまで
        val comp = LinkedHashSet<String>()
        val phrase = tables.PREDICTIVE_PHRASES.firstOrNull {
            target.length >= it.minPrefix && target.length < it.reading.length && it.reading.startsWith(target)
        }
        if (target.length >= 2 && !isRangeShrunk()) {
            comp.addAll(learning.completions(target, 2))
            // 辞書の続きは、4 字以上打っていて続きの語が 1 つに絞れたときだけ (HarmonyOS 版 PredictiveConversion と同じ。
            // 何でも足すと、打った以上に続きが付いた候補が並んだ。ユーザー 2026-10-14)
            if (target.length >= 4) ai?.complete(target, 2)?.let { if (it.size == 1) comp.addAll(it) }
        }
        val inserted = ArrayList<String>()
        var at = minOf(1, cands.size)
        for (c in comp) {
            if (inserted.size >= 3) break
            if (c in cands || c == phrase?.text) continue
            cands.add(at++, c)
            inserted.add(c)
        }
        if (phrase != null) {
            cands.remove(phrase.text)
            cands.add(0, phrase.text)
        }
        completions = (inserted + listOfNotNull(phrase?.text)).toSet()
        // ユーザー辞書に登録した語は先頭に (HarmonyOS 版 withRegisteredWords と同じ)
        val user = userDict.lookup(target)
        if (user.isNotEmpty()) cands = ArrayList(user + cands.filter { it !in user })
        // メールアドレス: @ の後ろにドメインの候補 (HarmonyOS 版 emailCompletions と同じ。ユーザー 2026-10-13)
        var emailAt = minOf(1, cands.size)
        for (m in emailCompletions(target)) if (m !in cands) cands.add(emailAt++, m)
        // あっと -> 全角の ＠ しか無かった。メールアドレスに使う半角の @ をその前に
        val fwAt = cands.indexOf("＠")
        if (fwAt >= 0 && "@" !in cands) cands.add(fwAt, "@")
        // 絵文字・顔文字は後ろに (HarmonyOS 版の辞書変換と同じ位置)
        for (e in (tables.EMOJI_MAP[target] ?: emptyList()) + (tables.KAOMOJI_MAP[target] ?: emptyList())) {
            if (e !in cands) cands.add(e)
        }
        cands = ArrayList(learning.applyLearnedOrder(target, cands))
        // 提案: 候補に混ぜず分ける (エンジンの提案 + こちらの日付・括弧・絵文字・顔文字)。候補からは外す
        if (settings.separateSuggest && ai != null) {
            // 1 位にした打ち間違いの直し (種類 8) は提案に出さず、候補の帯で “ ” で囲む印にだけ使う
            typoTop = ai.suggestions.firstOrNull { it.first == 8 }?.second ?: ""
            afterEmoji = ai.suggestions.filter { it.first == 9 }.map { it.second }   // 確定の後の帯に出す絵文字・顔文字
            val sg = ArrayList(ai.suggestions.filter { it.first != 8 && it.first != 9 })
            val dates = DateTimePredictor.predict(target, Calendar.getInstance())
            val moveDates = sg.any { it.first == 2 }
            // 単位の記号 (℃・%・° …) は文の中で使うので提案に回さない (HarmonyOS 版のエンジンと同じ、2026-10-14)
            val symbolic = { c: String -> c.any { it !in UNIT_MARKS } && c.none { it.isLetterOrDigit() } || (tables.EMOJI_MAP[target]?.contains(c) == true) || (tables.KAOMOJI_MAP[target]?.contains(c) == true) }
            val moved = cands.filter { c -> c != target && (sg.any { it.second == c } || (moveDates && c in dates) || (symbolic(c) && target.any { it in 'ぁ'..'ゖ' })) }
            for (c in moved) if (sg.none { it.second == c }) sg.add(7 to c)
            val rest = cands.filter { it !in moved }
            if (rest.isNotEmpty()) cands = ArrayList(rest)
            // 選んだことのある提案を同じ種類の中で前に (括弧は人によって使う組が違う。HarmonyOS 版と同じ、2026-10-14)
            val kindOrder = sg.map { it.first }.distinct()
            val use = settings.suggestUse
            suggestions = sg.filter { renderable(it.second) }.distinctBy { it.second }
                .sortedWith(compareBy<Pair<Int, String>>({ kindOrder.indexOf(it.first) }, { -(use[it.second] ?: 0) })).take(12)
        } else {
            suggestions = emptyList()
        }
        // 半角カタカナ (ほかに出す手段が無かった。ユーザー 2026-10-13)。いちばん後ろに
        val half = toHalfKatakana(toKatakana(target))
        if (half != toKatakana(target) && half !in cands) cands.add(half)
        cands = ArrayList(cands.filter { renderable(it) })
        // 読みのままの形は 5 番目までに必ず出す (あいうえおんがく のような題名。スマホには無変換キーが無い。ユーザー 2026-10-31)
        val kanaAt = cands.indexOf(target)
        if (kanaAt > 4) {
            cands.removeAt(kanaAt)
            cands.add(4, target)
        }
        candidates = cands
        if (inputState != InputState.SELECTING) selectedCandidateIndex = 0
        else selectedCandidateIndex = selectedCandidateIndex.coerceAtMost(cands.size - 1)
        showComposing()
        host.render()
    }

    /**
     * 挨拶はかな書きを先に。モデルは「こんにちは」を「今日は」と書きがち (教材の読みの入れ替え「今日 → こんにち」の副作用)。
     * こんにちは は読みのどこにあっても (きょうは も打っていなければ)、こんばんは は読みがそれだけのとき (今晩は寒い があるので)、
     * 1 位の「今日は」「今晩は」をかな書きに直した候補を 1 位に置く (漢字の方も残す)
     */
    private fun preferGreetings(target: String, cands: MutableList<String>) {
        val top = cands.firstOrNull() ?: return
        val rules = listOf(
            Triple("今日は", "こんにちは", "こんにちは" in target && "きょうは" !in target),
            Triple("今晩は", "こんばんは", target.trimEnd('、', '。', '！', '？', '!', '?', 'ー', '〜') == "こんばんは"),
        )
        for ((kanji, kana, ok) in rules) {
            if (!ok || kanji !in top) continue
            val fixed = top.replace(kanji, kana)
            cands.remove(fixed)
            cands.add(0, fixed)
            return
        }
    }

    /**
     * 確定の後の帯: 確定した文の終わりの語に決まった絵文字があれば出す (HarmonyOS 版 EmojiSuggest)。
     * 向こうは確定した語で引くが、AI 変換は文ごと確定するので、文の末尾のいちばん長い一致で引く
     */
    private fun pushPredictions(text: String) {
        val strip = ArrayList<String>()
        // 言い方の揺れ (疲れました・疲れちゃった・嬉しいです) は表の形 (疲れた・嬉しい) に戻して引く (HarmonyOS 版 EmojiSuggest.forms)
        val t = text.replace(Regex("[。．、！!？?ー〜~…・wｗ笑]+$"), "")
        search@ for (i in t.indices) {
            val s = t.substring(i)
            val forms = mutableListOf(s)
            for (p in PAST_FORMS) if (s.length > p.length && s.endsWith(p)) forms.add(s.dropLast(p.length) + "た")
            for (p in TAIL_PARTICLES) if (s.length > p.length && s.endsWith(p)) forms.add(s.dropLast(p.length))
            for (k in forms) {
                val e = tables.EMOJI_FOR_SURFACE[k] ?: continue
                for (x in e) if (x !in strip && strip.size < 16) strip.add(x)
                break@search
            }
        }
        for (x in afterEmoji) if (x !in strip && strip.size < 16) strip.add(x)   // 文の中の語の絵文字・顔文字 (2026-10-14)
        afterEmoji = emptyList()
        predictions = afterInsertStrip(strip)
        host.render()
        SentenceFlow.save()
    }

    companion object {
        private const val UNDO_MERGE_MS = 1500L
        private const val PASTE_WAIT_MS = 1500L   // アプリに頼んだ貼り付けの字が入るのを待つ長さ
        private const val FIELD_CHECK_MS = 150L   // 未確定の範囲が消えた通知を信じるまでの時間
        private const val RESTART_RETRY_MS = 80L  // 入力のやり直しの後、欄を読み直すまでの時間
        private const val LEARNING_SAVE_MS = 500L // 確定から学習をファイルに書くまでの時間 (HarmonyOS 版と同じ)
        private const val RETYPE_MS = 10_000L     // 確定してから打ち直しとみなす時間 (PC 版と同じ)
        private const val UNIT_MARKS = "℃°%％‰℉Ω㎏㎞㎝㎜㎡㎥¥￥$＄€£"
        private val PAST_FORMS = listOf("てしまいました", "ちゃいました", "てしまった", "ちゃった", "ちゃう", "ました", "ています", "ている", "てる", "ます")
        private val TAIL_PARTICLES = listOf("ですね", "ですよ", "でした", "です", "だね", "だよ", "だな", "すぎる", "すぎ", "ね", "よ", "な", "わ", "だ")
        private val EMAIL_TAIL = Regex("""[A-Za-z0-9._%+-]*[@＠]([A-Za-z0-9.-]*)$""")
        private const val MAX_HOST_UNDO = 50
        private val RANGE_BG = Color.argb(0x60, 0x4C, 0x8D, 0xFF)
        private const val BUNSETSU_PUNCT = "、。！？!?"
        private val PASSWORD_VARIATIONS = setOf(
            EditorInfo.TYPE_TEXT_VARIATION_PASSWORD, EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
            EditorInfo.TYPE_TEXT_VARIATION_WEB_PASSWORD,
        )
        private val ALPHA_VARIATIONS = PASSWORD_VARIATIONS + setOf(
            EditorInfo.TYPE_TEXT_VARIATION_EMAIL_ADDRESS,
            EditorInfo.TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS, EditorInfo.TYPE_TEXT_VARIATION_URI,
        )

        fun toKatakana(s: String) = buildString { for (c in s) append(if (c in 'ぁ'..'ゖ') c + 0x60 else c) }

        private const val HALF_BASE = "ｧｱｨｲｩｳｪｴｫｵｶｶｷｷｸｸｹｹｺｺｻｻｼｼｽｽｾｾｿｿﾀﾀﾁﾁｯﾂﾂﾃﾃﾄﾄﾅﾆﾇﾈﾉﾊﾊﾊﾋﾋﾋﾌﾌﾌﾍﾍﾍﾎﾎﾎﾏﾐﾑﾒﾓｬﾔｭﾕｮﾖﾗﾘﾙﾚﾛﾜﾜｲｴｦﾝｳｶｹ"
        private const val MAX_WHOLE_LEARN_LEN = 12   // 語に分けられたとき、読み全体も覚えるのはこの字数まで (長い文は二度と来ない)
        private const val HALF_MARK ="00000000000101010101010101010101010010101000000120120120120120000000000000000000000100"

        /** 全角カタカナ -> 半角カタカナ (HarmonyOS 版 toHalfKatakana と同じ表) */
        fun toHalfKatakana(s: String) = buildString {
            for (c in s) {
                val i = c - 'ァ'
                when {
                    i in HALF_BASE.indices -> { append(HALF_BASE[i]); when (HALF_MARK[i]) { '1' -> append('ﾞ'); '2' -> append('ﾟ') } }
                    c == 'ー' -> append('ｰ')
                    c == '・' -> append('･')
                    else -> append(c)
                }
            }
        }

        /** メールのドメイン。順は教材に「@ドメイン」が書かれた回数の多い順 (HarmonyOS 版 EMAIL_DOMAINS と同じ) */
        private val EMAIL_DOMAINS = listOf("gmail.com", "yahoo.co.jp", "docomo.ne.jp", "i.softbank.jp", "hotmail.com", "ezweb.ne.jp",
            "live.jp", "hotmail.co.jp", "ybb.ne.jp", "softbank.ne.jp", "nifty.com", "me.com", "yahoo.com", "icloud.com", "outlook.jp",
            "outlook.com", "au.com", "rakuten.jp")
        private val EMAIL = Regex("""^([A-Za-z0-9._%+-]+)@([A-Za-z0-9.-]*)$""")

        fun emailCompletions(s: String): List<String> {
            val m = EMAIL.matchEntire(s) ?: return emptyList()
            val typed = m.groupValues[2].lowercase()
            return EMAIL_DOMAINS.filter { it.startsWith(typed) && it != typed }.take(5).map { "${m.groupValues[1]}@$it" }
        }
    }
}

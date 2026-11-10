package com.shunti.japaneseime

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.inputmethodservice.InputMethodService
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputContentInfo
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import java.util.concurrent.Executors

/**
 * shunti IME (Android 版)。入力の決まりは HarmonyOS 版の移植 (InputHandler)、変換は AI 変換 (shuntelligence) だけ。
 * 画面: 上に候補の帯 (入力中は変換の候補、確定の後は絵文字とクリップボード。右端に ⚙)、
 * 下にキーボード (フリック / QWERTY / 記号・絵文字の一覧 / メニュー のどれか)。
 * 置き方は 3 通り: 普通 (幅と左右の位置を設定で変えられる)・片手 (左か右に寄せる)・フローティング (画面の上の好きな所)。
 */
class KkcIme : InputMethodService(), InputHandler.Host, MenuView.Host {
    companion object {
        const val AI_LEARN_FILE = "ai_learn.bin"   // AI学習で育てた中身 (filesDir の中。学習のリセットで消す)
        private const val EXTRA_SENSITIVE = "android.content.extra.IS_SENSITIVE"   // ClipDescription.EXTRA_IS_SENSITIVE (API 33)
        private const val CLIP_PREFIX = "\u0001clip\u0001"   // 候補の帯の中で、貼り付けのチップを見分ける印 (表示はしない)
    }

    private val worker = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private var engine: Engine? = null
    override var ai: AiConverter? = null
        private set

    private lateinit var tables: Tables
    private lateinit var learning: Learning
    private lateinit var recent: RecentSymbols
    private lateinit var input: InputHandler
    private lateinit var settings: Settings
    private lateinit var haptics: Haptics
    private var viewSig = ""   // 画面の作りに効く設定。変わったら作り直す

    private var bar: LinearLayout? = null
    // 提案の行 (候補の帯の上。提案があるときだけ出す)
    private var suggBox: FrameLayout? = null
    private var suggRow: LinearLayout? = null
    private var suggKey = ""
    private val suggShow = Runnable { showSuggestions() }
    private var scroll: HorizontalScrollView? = null
    private var flick: KeyboardView? = null
    private var qwerty: QwertyKeyboardView? = null
    private var godan: GodanKeyboardView? = null
    private var palette: PaletteView? = null
    private var menu: MenuView? = null
    private var root: FrameLayout? = null
    private var block: LinearLayout? = null   // 候補の帯 + キーボード (片手・フローティングで動かす塊)
    private var theme: Map<String, Int> = emptyMap()
    private var navInset = 0   // 画面下のナビゲーションバーの高さ (最後に分かった値)

    // クリップボード: 履歴は ClipStore (文・画像・動画を保存。2026-11-05)。fresh = まだ貼っていない新しいコピー
    private lateinit var clipStore: ClipStore
    private val clipWorker = Executors.newSingleThreadExecutor()   // 画像・動画の写しを取る (変換の worker を待たせない)
    private var freshClip: ClipStore.Clip? = null
    private val thumbs = android.util.LruCache<Long, android.graphics.Bitmap>(40)
    private var clipboard: ClipboardManager? = null
    private val clipListener = ClipboardManager.OnPrimaryClipChangedListener { readClip() }

    override val ic: InputConnection? get() = currentInputConnection
    override val editorInfo: EditorInfo? get() = currentInputEditorInfo

    override fun onCreate() {
        super.onCreate()
        tables = Tables.load(assets)
        learning = Learning.get(this)
        SentenceFlow.init(this)
        settings = Settings(this)
        haptics = Haptics(this)
        recent = RecentSymbols(getSharedPreferences("shunti", MODE_PRIVATE), tables.DEFAULT_RECENT_SYMBOLS)
        input = InputHandler(this, tables, learning, settings, UserDict.get(this))
        clipStore = ClipStore.get(this)
        clipboard = (getSystemService(CLIPBOARD_SERVICE) as? ClipboardManager)?.also { it.addPrimaryClipChangedListener(clipListener) }
        openEngine()
    }

    private var engineModel = ""   // 開いているモデルの資産の名前

    // 辞書とモデルは裏で開く (開くまでは、かなとカタカナだけを出す)。設定で変換モデルを替えたときも開き直す
    private fun openEngine() {
        val file = settings.modelFile
        if (file == engineModel) return
        engineModel = file
        worker.execute {
            val e = Engine.open(assets, tables.SLURS, model = file)
            main.post {
                if (worker.isShutdown) {   // 開いている間に IME が終わった
                    e?.close()
                    return@post
                }
                val old = engine
                engine = e
                e?.setSuggest(settings.separateSuggest)
                e?.allowSlurs = settings.allowSlurs
                ai = if (e != null) AiConverter(e, worker, main).also { a -> a.keysFor = { k -> input.keysFor(k) }; a.learnedTop = { r, l, lid -> learning.top(r, l, lid) } } else null
                // 品詞の記録の無い (前の版の) 学習の表記は、読みと表記だけで網をたどって品詞を決める (PC 版と同じ。2026-11-09)
                learning.lidResolver = ai?.let { a -> { r: String, s: String -> a.resolveLid(r, s) } }
                ai?.setAiLearn(aiLearnPath())
                userSynced = -1   // 新しいエンジンにユーザー辞書を入れ直す
                sentForms = null
                syncUserWords()
                if (input.composingText.isNotEmpty()) input.updateCandidates()
                // 前のエンジンは、作業用のスレッドに残っている変換が済んでから閉じる (スレッドは 1 本なので順番どおり)
                if (old != null) worker.execute { old.close() }
            }
        }
    }

    override fun onDestroy() {
        clipboard?.removePrimaryClipChangedListener(clipListener)
        learning.save()
        recent.save()
        ai = null   // この後に届く変換の結果で、閉じたエンジンを使わないように
        worker.execute { engine?.close() }
        worker.shutdown()
        clipWorker.shutdown()
        super.onDestroy()
    }

    /** 横向きでも全画面の入力欄 (抽出画面) にしない */
    override fun onEvaluateFullscreenMode() = false

    // ---------------------------------------------------------------- 画面の組み立て
    private fun sig() = listOf(
        settings.isDark(this), settings.heightPercent, settings.flickHints, settings.widthPercent, settings.offsetPercent, settings.bottomExtraDp,
        settings.oneHanded, settings.floating, resources.configuration.orientation,
    ).joinToString("/")

    override fun onCreateInputView(): View {
        val dp = resources.displayMetrics.density
        val screenW = resources.displayMetrics.widthPixels
        viewSig = sig()
        theme = if (settings.isDark(this)) tables.DARK_THEME else tables.LIGHT_THEME
        val floating = settings.floating
        val side = if (floating) "off" else settings.oneHanded
        val landscape = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        val scale = settings.heightPercent / 100f * (if (floating) 0.85f else 1f)
        // 1 行の高さ (設定の 100% のとき)。以前の 120% を標準にした (前の標準は低すぎた)
        val rowH = (if (landscape) 53 else 67) * dp * scale
        val kbH = rowH * 4 + 8 * dp

        val kb = KeyboardView(this, input, tables, ::tick, ::special).also {
            it.theme = theme
            it.hints = settings.flickHints
            it.rowH = rowH
        }
        val qw = QwertyKeyboardView(this, input, tables, ::tick, ::special).also { it.theme = theme; it.totalH = kbH }
        val gd = GodanKeyboardView(this, input, tables, ::tick).also { it.theme = theme; it.totalH = kbH; it.mirror = settings.godanMirror }
        val pal = PaletteView(this, input, tables, recent, ::tick).also { it.theme = theme; it.totalH = kbH }
        val mv = MenuView(this, this, ::tick).also { it.theme = theme; it.totalH = kbH }
        val frame = FrameLayout(this).apply {
            addView(kb)
            addView(qw)
            addView(gd)
            addView(pal)
            addView(mv)
        }
        // 候補の帯 (横に動かせる) と、右端の ⚙
        val b = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val sc = HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(b)
        }
        val gear = TextView(this).apply {
            text = "⚙"
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            setTextColor(theme.getValue("textSecondary"))
            gravity = Gravity.CENTER
            setOnClickListener {
                tick()
                input.commitTopCandidate()
                input.subMode = if (input.subMode == InputHandler.SubMode.MENU) InputHandler.SubMode.KANA else InputHandler.SubMode.MENU
                if (input.subMode == InputHandler.SubMode.MENU) menu?.reset()
                render()
            }
            // 長押しでフローティングの切り替え
            setOnLongClickListener {
                tick()
                toggleFloating()
                true
            }
        }
        val strip = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(sc, LinearLayout.LayoutParams(0, (44 * dp).toInt(), 1f))
            addView(gear, LinearLayout.LayoutParams((44 * dp).toInt(), (44 * dp).toInt()))
        }
        // 提案の行: 「提案」+ 札を横に (はみ出したら横に動かせる)。打ち間違いの直しは左寄せ、ほかは右寄せ
        val sRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val sScroll = HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            isFillViewport = true
            addView(sRow, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        val sBox = FrameLayout(this).apply {
            visibility = suggHidden()
            addView(sScroll, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        val blk = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(theme.getValue("panelBg"))
            if (floating) addView(DragHandle(), LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (20 * dp).toInt()))
            addView(sBox, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (34 * dp).toInt()))
            addView(strip)
            addView(frame)
        }
        val r = FrameLayout(this)
        if (floating) {
            // 窓は画面いっぱいの高さにして中は透明。触れられるのはキーボードの所だけ (onComputeInsets)
            val w = screenW * Settings.FLOAT_PERCENT / 100
            r.minimumHeight = resources.displayMetrics.heightPixels
            blk.elevation = 8 * dp
            r.addView(blk, FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT))
            r.post { placeFloating() }
        } else {
            val w = if (side != "off") screenW * Settings.ONE_HAND_PERCENT / 100 else screenW * settings.widthPercent / 100
            val left = when (side) {
                "left" -> 0
                "right" -> screenW - w
                else -> (screenW - w) * settings.offsetPercent / 100
            }
            r.setBackgroundColor(theme.getValue("panelBg"))
            r.addView(blk, FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT).apply { leftMargin = left })
            if (side != "off") {
                // 空いた側: 反対側へ寄せる・片手をやめる
                val pane = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    gravity = Gravity.CENTER
                    addView(sideButton("⇆") { toggleOneHanded(if (side == "left") "right" else "left") })
                    addView(sideButton("⤢") { toggleOneHanded(side) })
                }
                r.addView(pane, FrameLayout.LayoutParams(screenW - w, ViewGroup.LayoutParams.MATCH_PARENT).apply {
                    leftMargin = if (side == "left") w else 0
                })
            }
            // 画面下のナビゲーションバー (キーボードを隠す・切り替えるボタン) にキーが重ならないよう、下を空ける。
            // 端末によっては知らせてくる高さより大きいボタンを出す (Galaxy で確定キーと隠すボタンが重なった) ので、
            // 少なくとも 48dp (標準のナビゲーションバーの高さ) は空け、設定の「キーボードの下の余白」を足す。
            // 作り直した画面には余白の知らせが来ないことがあるので、最後に分かった値を先に当てておく
            val extra = (settings.bottomExtraDp * dp).toInt()
            fun pad(nav: Int) = maxOf(nav, (48 * dp).toInt()) + extra
            r.setPadding(0, 0, 0, pad(navInset))
            r.setOnApplyWindowInsetsListener { v, insets ->
                val bottom = if (android.os.Build.VERSION.SDK_INT >= 30) insets.getInsets(android.view.WindowInsets.Type.navigationBars()).bottom
                else @Suppress("DEPRECATION") insets.systemWindowInsetBottom
                navInset = bottom
                v.setPadding(0, 0, 0, pad(bottom))
                insets
            }
            r.post { r.requestApplyInsets() }
        }
        bar = b
        scroll = sc
        suggBox = sBox
        suggRow = sRow
        suggKey = ""
        flick = kb
        qwerty = qw
        godan = gd
        palette = pal
        menu = mv
        block = blk
        root = r
        styleNavBar()
        render()
        return r
    }

    /**
     * ナビゲーションバーの下地をキーボードと続いて見えるようにする。
     * targetSdk 35 以降の Android 15 以降は画面の端まで描く決まり (edge-to-edge) で、3 ボタンのときだけ
     * システムがバーの裏に半透明の暗い幕を掛ける (ジェスチャーのときは掛けない)。キーボードは下の余白を自分の色で塗っているので、
     * 幕を外せば続いて見える。Android 14 以前はバーの色そのものをキーボードの色にする。
     * ボタンの色も背景の明るさに合わせる (明るいテーマで白いボタンだと見えない)
     */
    @Suppress("DEPRECATION")
    private fun styleNavBar() {
        val w = window?.window ?: return
        val sdk = android.os.Build.VERSION.SDK_INT
        if (sdk >= 29) w.isNavigationBarContrastEnforced = false
        if (sdk < 35) w.navigationBarColor = theme.getValue("panelBg")
        val light = !settings.isDark(this)
        if (sdk >= 30) {
            val flag = android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            w.insetsController?.setSystemBarsAppearance(if (light) flag else 0, flag)
        } else {
            val v = w.decorView
            v.systemUiVisibility = if (light) v.systemUiVisibility or View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
            else v.systemUiVisibility and View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR.inv()
        }
    }

    private fun sideButton(label: String, onClick: () -> Unit) = TextView(this).apply {
        val dp = resources.displayMetrics.density
        text = label
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
        setTextColor(theme.getValue("textSecondary"))
        gravity = Gravity.CENTER
        setPadding(0, (14 * dp).toInt(), 0, (14 * dp).toInt())
        setOnClickListener {
            tick()
            onClick()
        }
    }

    // ---------------------------------------------------------------- フローティング
    /** 保存した位置 (画面に対する割合) に置く。画面からはみ出さないように */
    private fun placeFloating() {
        val r = root ?: return
        val b = block ?: return
        if (r.width == 0 || b.width == 0) {   // まだ大きさが決まっていない: 決まってから置く
            r.post { placeFloating() }
            return
        }
        val lp = b.layoutParams as FrameLayout.LayoutParams
        lp.leftMargin = (settings.floatX * r.width).toInt().coerceIn(0, maxOf(0, r.width - b.width))
        lp.topMargin = (settings.floatY * r.height).toInt().coerceIn(0, maxOf(0, r.height - b.height))
        b.layoutParams = lp
    }

    /**
     * フローティングの上端のつまみ: ドラッグで動かし、離したら位置を覚える。
     * 画面の下の端より先まで押し込んで離すと、フローティングをやめて下に戻す (押し込んでいる間は薄く出す)
     */
    private inner class DragHandle : View(this@KkcIme) {
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private var sx = 0f
        private var sy = 0f
        private var ml = 0
        private var mt = 0
        private var dock = false

        override fun onDraw(canvas: Canvas) {
            val dp = resources.displayMetrics.density
            p.color = theme.getValue("textSecondary")
            canvas.drawRoundRect(width / 2f - 20 * dp, height / 2f - 2 * dp, width / 2f + 20 * dp, height / 2f + 2 * dp, 2 * dp, 2 * dp, p)
        }

        @android.annotation.SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            val r = root ?: return true
            val b = block ?: return true
            val lp = b.layoutParams as FrameLayout.LayoutParams
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    sx = e.rawX; sy = e.rawY; ml = lp.leftMargin; mt = lp.topMargin
                    dock = false
                }
                MotionEvent.ACTION_MOVE -> {
                    val maxTop = maxOf(0, r.height - b.height)
                    val want = (mt + (e.rawY - sy)).toInt()
                    lp.leftMargin = (ml + (e.rawX - sx)).toInt().coerceIn(0, maxOf(0, r.width - b.width))
                    lp.topMargin = want.coerceIn(0, maxTop)
                    b.layoutParams = lp
                    dock = want > maxTop + 48 * resources.displayMetrics.density
                    b.alpha = if (dock) 0.55f else 1f
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (dock && e.actionMasked == MotionEvent.ACTION_UP) {
                        toggleFloating()   // 下に戻す
                        return true
                    }
                    b.alpha = 1f
                    if (r.width > 0 && r.height > 0) {
                        settings.floatX = lp.leftMargin.toFloat() / r.width
                        settings.floatY = lp.topMargin.toFloat() / r.height
                    }
                }
            }
            return true
        }
    }

    /**
     * フローティングのとき: 入力欄のアプリは画面いっぱいのまま (キーボードの分を縮めない)、
     * 触れて反応するのはキーボードの所だけにする。ほかは下のアプリに触れる
     */
    override fun onComputeInsets(outInsets: Insets) {
        super.onComputeInsets(outInsets)
        val r = root ?: return
        val b = block ?: return
        if (!settings.floating || r.height == 0) return
        val loc = IntArray(2)
        b.getLocationInWindow(loc)
        val rootLoc = IntArray(2)
        r.getLocationInWindow(rootLoc)
        val bottom = rootLoc[1] + r.height
        outInsets.contentTopInsets = bottom
        outInsets.visibleTopInsets = bottom
        outInsets.touchableInsets = Insets.TOUCHABLE_INSETS_REGION
        outInsets.touchableRegion.set(Rect(loc[0], loc[1], loc[0] + b.width, loc[1] + b.height))
    }

    // ---------------------------------------------------------------- 入力欄の出入り
    /** キーボードを開くたびに、設定画面で変えた見た目を反映し、新しいコピーがないか見る */
    override fun onStartInputView(info: EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        if (root != null && sig() != viewSig) setInputView(onCreateInputView())
        styleNavBar()   // 開くたびに当て直す (窓を作り直すと戻ることがある)
        readClip()
        openEngine()   // 設定で変換モデルを替えた (同じなら何もしない)
        engine?.setSuggest(settings.separateSuggest)
        engine?.allowSlurs = settings.allowSlurs
        ai?.setAiLearn(aiLearnPath())   // 設定で変えた・学習のリセットで消された
        syncUserWords()
    }

    /** AI学習の保存先 (オフなら null) */
    private fun aiLearnPath(): String? = if (settings.aiLearn) java.io.File(filesDir, AI_LEARN_FILE).path else null

    private var userSynced = -1

    private var learnSynced = -1
    private var sentForms: String? = null   // 最後に網に入れた語 (新しいエンジンでは null に戻す)

    /** ユーザー辞書か、学習で 2 回以上選んだ語が変わっていたら、変換の網に入れ直す (変換と同じ裏のスレッドで) */
    private fun syncUserWords() {
        val e = engine ?: return
        val dict = UserDict.get(this)
        val v = dict.version
        if (v == userSynced && learning.version == learnSynced) return
        userSynced = v
        learnSynced = learning.version
        val forms = dict.engineForms() + learning.strongForms()
        // 学習のたびに版は変わるが、網に入れる語はたいてい同じ。同じなら入れ直さない (変換の控えも捨てずに済む)
        val key = forms.joinToString("\u0001") { "${it.reading}\u0002${it.surface}\u0002${it.tmplReading}\u0002${it.tmplSurface}\u0002${it.lid}" }
        if (key == sentForms) return
        sentForms = key
        worker.execute {
            e.setUserWords(forms)
            main.post { ai?.clearCache() }
        }
    }

    override fun onStartInput(attribute: EditorInfo?, restarting: Boolean) {
        super.onStartInput(attribute, restarting)
        input.onInputStart(attribute, restarting)
    }

    /** キーボードだけを隠したとき (ナビゲーションバーの ∨ など)。標準の処理より先に、入力中の文字を確定して状態を空にする */
    override fun onFinishInputView(finishingInput: Boolean) {
        if (!finishingInput) input.onViewHidden()
        super.onFinishInputView(finishingInput)
    }

    /** 入力欄の未確定の範囲が変わった (アプリの送信ボタンで欄が空になったときなど) */
    override fun onUpdateSelection(oldSelStart: Int, oldSelEnd: Int, newSelStart: Int, newSelEnd: Int, candidatesStart: Int, candidatesEnd: Int) {
        super.onUpdateSelection(oldSelStart, oldSelEnd, newSelStart, newSelEnd, candidatesStart, candidatesEnd)
        input.onFieldComposing(candidatesStart, candidatesEnd)
        input.onHostSelection(newSelStart, newSelEnd)
    }

    override fun onFinishInput() {
        super.onFinishInput()
        input.onInputStop()
        recent.save()
    }

    /** キーの長押しの操作 (KeyboardView / QwertyKeyboardView から) */
    private fun special(action: String) {
        when (action) {
            "onehand-left" -> toggleOneHanded("left")
            "onehand-right" -> toggleOneHanded("right")
        }
    }

    /** キーに触れた瞬間の振動 (設定でオフにできる) */
    private fun tick() {
        if (settings.haptic) haptics.tick(settings.hapticStrength)
    }

    // ---------------------------------------------------------------- クリップボード
    private var lastClip: String? = null
    private var lastClipStamp = 0L

    /**
     * 新しくコピーされた文を履歴に足し、候補の帯に貼り付けのチップを出す (読めるのは選ばれているキーボードのときだけ)。
     * 同じ文をもう一度コピーしたときも出す (コピーした時刻で見分ける。前は文が同じだと出なかった)
     */
    private fun readClip() {
        val clip = runCatching { clipboard?.primaryClip }.getOrNull() ?: return
        if (clip.itemCount == 0) return
        val desc = clip.description
        val stamp = desc?.timestamp ?: 0L
        val item = clip.getItemAt(0)
        val uri = item.uri
        // 自分が貼り付けのために入れ直した写し: 履歴には足さない
        if (uri != null && uri.authority == "$packageName.clips") {
            lastClipStamp = stamp
            return
        }
        // パスワードなど、コピーした側が機密の印を付けたもの (Android 13 から): 履歴に入れず、中身も帯に出さない
        if (desc?.extras?.getBoolean(EXTRA_SENSITIVE) == true) {
            if (stamp != lastClipStamp) { lastClipStamp = stamp; freshClip = null; render() }
            return
        }
        // 画像・動画・ファイル: 写しを取ってから履歴と帯に出す (読む権限はクリップボードを読んだ今だけなので、すぐに)
        val mime = desc?.takeIf { it.mimeTypeCount > 0 }?.getMimeType(0)
        if (uri != null && uri.scheme == "content" && mime != null && !mime.startsWith("text/")) {
            if (stamp == lastClipStamp && stamp != 0L) return
            lastClipStamp = stamp
            clipWorker.execute {
                val c = runCatching { clipStore.addMedia(contentResolver, uri, mime) }.getOrNull()
                main.post {
                    if (c != null) freshClip = c
                    render()
                }
            }
            return
        }
        val t = runCatching { item.coerceToText(this)?.toString() }.getOrNull()
        if (t.isNullOrEmpty() || (t == lastClip && stamp == lastClipStamp)) return
        lastClip = t
        lastClipStamp = stamp
        freshClip = clipStore.addText(t)
        render()
    }

    /** 履歴の見本 (小さい絵)。画像・動画だけ */
    override fun clipThumb(c: ClipStore.Clip): android.graphics.Bitmap? {
        if (c.kind == 0) return null
        thumbs.get(c.id)?.let { return it }
        val f = clipStore.thumbOf(c) ?: return null
        return android.graphics.BitmapFactory.decodeFile(f.absolutePath)?.also { thumbs.put(c.id, it) }
    }

    /** 長押しで出す大きな見本 (画像は大きめに読み直す。動画は 1 コマ) */
    override fun clipPreview(c: ClipStore.Clip): android.graphics.Bitmap? =
        if (c.kind == 1) clipStore.fileOf(c)?.let { ClipStore.decodeSampled(it, 1080) } ?: clipThumb(c) else clipThumb(c)

    override fun pasteClip(c: ClipStore.Clip) {
        if (c.kind == 0) paste(c.text) else pasteMedia(c)
    }

    override fun toggleClipPin(c: ClipStore.Clip) {
        clipStore.togglePin(c.id)
        menu?.invalidate()
    }

    override fun removeClip(c: ClipStore.Clip) {
        clipStore.remove(c.id)
        if (freshClip?.id == c.id) freshClip = null
        menu?.invalidate()
    }

    override fun clearClips() {
        clipStore.clear(all = false)
        if (freshClip?.pinned != true) freshClip = null
        menu?.invalidate()
    }

    /**
     * 画像・動画を貼る: 入力欄が受け取れる種類なら、写しをそのまま渡す (commitContent。チャットの画像など)。
     * 受け取れない欄では、写しをクリップボードに入れ直して、アプリに貼り付けを頼む (アプリが受け取れれば入る)
     */
    private fun pasteMedia(c: ClipStore.Clip) {
        val f = clipStore.fileOf(c) ?: return
        input.commitShown()
        val uri = android.net.Uri.parse("content://$packageName.clips/${f.name}")
        val label = c.name.ifEmpty { if (c.kind == 1) "画像" else if (c.kind == 2) "動画" else "ファイル" }
        val desc = ClipDescription(label, arrayOf(c.mime))
        val accepts = currentInputEditorInfo?.contentMimeTypes.orEmpty().any { ClipDescription.compareMimeTypes(c.mime, it) }
        val ok = accepts && currentInputConnection?.commitContent(InputContentInfo(uri, desc), InputConnection.INPUT_CONTENT_GRANT_READ_URI_PERMISSION, null) == true
        if (!ok) {
            runCatching { clipboard?.setPrimaryClip(ClipData(desc, ClipData.Item(uri))) }
            val pasted = currentInputConnection?.performContextMenuAction(android.R.id.paste) == true
            if (!pasted) android.widget.Toast.makeText(this, "クリップボードに入れました。入力欄の長押しから貼り付けてください", android.widget.Toast.LENGTH_SHORT).show()
        }
        freshClip = null
        closeMenu()
    }

    // ---------------------------------------------------------------- MenuView.Host
    override val oneHanded get() = if (settings.floating) "off" else settings.oneHanded
    override val floating get() = settings.floating
    override val themeLabel get() = listOf("端末", "ライト", "ダーク")[settings.theme.coerceIn(0, 2)]
    override val clipHistory: List<ClipStore.Clip> get() = clipStore.list()

    private fun rebuild() {
        input.subMode = InputHandler.SubMode.KANA
        setInputView(onCreateInputView())
    }

    override fun toggleOneHanded(side: String) {
        settings.oneHanded = if (settings.oneHanded == side && !settings.floating) "off" else side
        settings.floating = false
        rebuild()
    }

    override fun toggleFloating() {
        settings.floating = !settings.floating
        rebuild()
    }

    override fun cycleTheme() {
        settings.theme = (settings.theme + 1) % 3
        setInputView(onCreateInputView())   // メニューを開いたまま色だけ変える
    }

    override fun openApp(section: String?) {
        closeMenu()
        startActivity(Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (section != null) putExtra("section", section)
        })
    }

    override fun showImePicker() {
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).showInputMethodPicker()
    }

    private fun paste(text: String) {
        input.commitShown()   // 入力中の残りがあれば、見えている形で先に確定する (貼り付けが読みを置き換えないように)
        input.insertText(text, raw = true)
        freshClip = null
        closeMenu()
    }

    /**
     * 候補の帯の「貼り付け」(新しいコピーを読めなかったときに出す): 入力欄のアプリに貼り付けを頼む (長押しメニューの
     * 「貼り付け」と同じ)。アプリ自身がクリップボードを読むので、キーボードがクリップボードを読めないとき
     * (端末の設定や機種によって止められることがある) でも貼り付けられる。頼めない入力欄では、読めればキーボードから入れる
     */
    private fun pasteFromClipboard() {
        input.commitShown()   // 入力中の残りは先に確定する (paste と同じ)
        // アプリが貼り付けた字は、取消キーで戻せるように後から記録する (記録していなくて戻せなかった。ユーザー 2026-11-03)
        input.expectAppPaste()
        val ok = currentInputConnection?.performContextMenuAction(android.R.id.paste) == true
        if (!ok) {
            val t = runCatching { clipboard?.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)?.toString() }
                .getOrNull()
            if (!t.isNullOrEmpty()) input.insertText(t, raw = true)
        }
        freshClip = null
        render()
    }

    override fun closeMenu() {
        input.subMode = InputHandler.SubMode.KANA
        render()
    }

    /** 貼り付けのチップの字: 小さい「貼り付け」を色の字で、その後ろにコピーした文 (キーボードの 2 色の見た目に合わせ、絵文字の印は使わない) */
    private fun clipLabel(preview: String): CharSequence {
        if (preview.isEmpty()) {   // 中身の見えない「貼り付け」だけのチップ: 色の字で普通の大きさ
            return android.text.SpannableString("貼り付け").apply {
                setSpan(android.text.style.ForegroundColorSpan(theme.getValue("accent")), 0, length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        val head = "貼り付け  "
        return android.text.SpannableStringBuilder(head + preview).apply {
            setSpan(android.text.style.ForegroundColorSpan(theme.getValue("accent")), 0, head.length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            setSpan(android.text.style.RelativeSizeSpan(0.7f), 0, head.length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
    }

    // ---------------------------------------------------------------- InputHandler.Host
    override fun post(r: () -> Unit) {
        main.post(r)
    }

    override fun postDelayed(ms: Long, r: () -> Unit) {
        main.postDelayed(r, ms)
    }

    override fun learningChanged() = syncUserWords()

    /**
     * 提案が無いときの提案の行。提案を分けて出す設定では場所を空けたまま (INVISIBLE) にする:
     * 出し入れでキーボードの高さが変わると、打っている最中にキーが上下してフリックが暴発した (ユーザー 2026-10-09)
     */
    private fun suggHidden(): Int = if (settings.separateSuggest) View.INVISIBLE else View.GONE

    private fun showSuggestions() {
        val box = suggBox ?: return
        val row = suggRow ?: return
        val sg = input.suggestions
        if (sg.isEmpty() || input.inputState != InputHandler.InputState.COMPOSING) { box.visibility = suggHidden(); return }
        val dp = resources.displayMetrics.density
        val accent = theme.getValue("accent")
        row.removeAllViews()
        row.addView(TextView(this).apply {
            text = "提案"
            typeface = JpFont.get(context)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextColor(accent)
            setPadding((8 * dp).toInt(), 0, (6 * dp).toInt(), 0)
        })
        val top = input.candidates.firstOrNull() ?: ""
        for ((i, s) in sg.withIndex()) {
            val shown = when {
                s.first == 1 -> "$top→${s.second}"
                s.first == 6 && s.second.startsWith(top) -> s.second.substring(top.length)
                else -> s.second
            }
            row.addView(TextView(this).apply {
                text = shown
                typeface = JpFont.get(context)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setTextColor(accent)
                setPadding((10 * dp).toInt(), (2 * dp).toInt(), (10 * dp).toInt(), (2 * dp).toInt())
                background = android.graphics.drawable.GradientDrawable().apply {
                    cornerRadius = 12 * dp
                    setStroke((1 * dp).toInt().coerceAtLeast(1), accent)
                }
                setOnClickListener {
                    tick()
                    input.commitSuggestion(i)
                    render()
                }
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginEnd = (6 * dp).toInt() })
        }
        // 打ち間違いの直しは入力中の文字に近い左、日付・絵文字などは右に寄せる
        row.gravity = Gravity.CENTER_VERTICAL or (if (sg.first().first == 1) Gravity.START else Gravity.END)
        (row.layoutParams as? FrameLayout.LayoutParams)?.let { it.width = ViewGroup.LayoutParams.MATCH_PARENT; row.layoutParams = it }
        box.visibility = View.VISIBLE
        // 右から引き出すように出る
        box.translationX = box.width.takeIf { it > 0 }?.toFloat() ?: (200 * dp)
        box.alpha = 0f
        box.animate().translationX(0f).alpha(1f).setDuration(220).start()
    }

    override fun render() {
        val b = bar ?: return
        val sub = input.subMode
        val layout = if (input.inputMode == InputHandler.InputMode.HIRAGANA) settings.kanaLayout else settings.alphaLayout
        val shown: View? = when (sub) {
            InputHandler.SubMode.MENU -> menu
            InputHandler.SubMode.SYMBOL, InputHandler.SubMode.EMOJI -> palette
            InputHandler.SubMode.KANA -> when (layout) {
                "QWERTY" -> qwerty
                "GODAN" -> godan
                else -> flick
            }
            else -> flick
        }
        godan?.mirror = settings.godanMirror   // 設定の変更を毎回読む (キーボードを作り直さなくても効くように)
        for (v in listOf(flick, qwerty, godan, palette, menu)) {
            v?.visibility = if (v === shown) View.VISIBLE else if (v === flick) View.INVISIBLE else View.GONE
            if (v === shown) v?.invalidate()
        }

        // 候補の帯: 入力中は変換の候補。そうでなければ貼り付け (新しいコピー) と、確定の後の絵文字
        val dp = resources.displayMetrics.density
        val inBunsetsu = input.inputState == InputHandler.InputState.BUNSETSU
        val sel = if (input.inputState == InputHandler.InputState.SELECTING || inBunsetsu) input.selectedCandidateIndex else -1

        val chips = ArrayList<Pair<String, () -> Unit>>()
        val suggAt = HashSet<Int>()   // 提案のチップ (強調色に ✦)
        if (input.inputState != InputHandler.InputState.IDLE) {
            for ((i, s) in input.candidates.withIndex()) chips.add((if (s == input.typoTop) "“$s”" else s) to { input.commitCandidate(i) })
            // 提案は別の行をやめて候補の帯に強調色で。候補と交互に 1 つずつ (2 位の後ろから): ひと固めにすると惜しい 2 位以降が
            // 遠くなった (HarmonyOS 版と同じ。ユーザー 2026-10-14)
            if (input.inputState == InputHandler.InputState.COMPOSING && chips.isNotEmpty()) {
                val top = input.candidates.firstOrNull() ?: ""
                for ((j, g) in input.suggestions.withIndex()) {
                    val at = 2 + 2 * j
                    if (at > chips.size) break
                    // 地味に (2026-10-14): 打ち間違いの直しは “ ” で囲んだ普通の字、ほかは強調色の字だけ (枠なし)
                    val shown = if (g.first == 1) "“${g.second}”" else g.second
                    chips.add(at, shown to { input.commitSuggestion(j) })
                    suggAt.add(at)
                }
            }
        } else {
            freshClip?.let { c ->
                val preview = when (c.kind) {
                    0 -> c.text.replace('\n', ' ').let { if (it.length > 14) it.take(14) + "…" else it }
                    1 -> "画像"
                    2 -> "動画"
                    else -> c.name.ifEmpty { "ファイル" }.let { if (it.length > 14) it.take(14) + "…" else it }
                }
                chips.add(CLIP_PREFIX + preview to { pasteClip(c) })
            }
            for ((i, s) in input.predictions.withIndex()) chips.add(s to { input.commitPrediction(i) })
            // 新しいコピーのチップが無いときも、帯の端にいつも「貼り付け」を置く (中身を読めない端末でも、ここから貼り付けられる)
            if (freshClip == null) chips.add(CLIP_PREFIX to { pasteFromClipboard() })
        }
        b.removeAllViews()
        for ((i, c) in chips.withIndex()) {
            val tv = TextView(this).apply {
                text = if (c.first.startsWith(CLIP_PREFIX)) clipLabel(c.first.removePrefix(CLIP_PREFIX)) else c.first
                typeface = JpFont.get(context)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
                setTextColor(theme.getValue(if (i == sel) "onAccent" else "textPrimary"))
                if (i == sel) setBackgroundColor(theme.getValue("accent"))
                // 提案は青枠を残す (消すと余計に浮いた。ユーザー 2026-10-14)
                else if (i in suggAt) background = android.graphics.drawable.InsetDrawable(android.graphics.drawable.GradientDrawable().apply {
                    cornerRadius = 18 * dp
                    setStroke((1 * dp).toInt().coerceAtLeast(1), theme.getValue("accent"))
                }, 0, (6 * dp).toInt(), 0, (6 * dp).toInt())
                setPadding((12 * dp).toInt(), 0, (12 * dp).toInt(), 0)
                gravity = Gravity.CENTER_VERTICAL
                setOnClickListener {
                    tick()
                    c.second()
                }
            }
            b.addView(tv, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, (44 * dp).toInt()))
        }
        suggBox?.visibility = View.GONE   // 提案の行は使わない (候補の帯に入れた)
        // 選んでいる候補が見えるように
        if (sel >= 0) b.post { b.getChildAt(sel)?.let { scroll?.smoothScrollTo(maxOf(0, it.left - (40 * dp).toInt()), 0) } }
        else scroll?.scrollTo(0, 0)
    }
}

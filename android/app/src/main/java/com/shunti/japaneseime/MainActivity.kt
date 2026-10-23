package com.shunti.japaneseime

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.text.method.LinkMovementMethod
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView

/**
 * アプリの画面 = 設定画面。キーボードを使えるようにする手順、試し打ち、振動・見た目・学習の設定、ライセンスなど。
 * 部品は Android 標準のものだけ (AndroidX を使わない)。設定はキーボードを次に開いたときに反映される。
 */
class MainActivity : Activity() {
    private lateinit var settings: Settings
    private lateinit var haptics: Haptics
    private lateinit var col: LinearLayout
    private lateinit var step1: TextView
    private lateinit var step2: TextView
    private lateinit var scroll: ScrollView
    private lateinit var dictSection: View
    private lateinit var dictReading: EditText
    private lateinit var dictList: LinearLayout
    private val dp get() = resources.displayMetrics.density

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        haptics = Haptics(this)
        col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(20), px(16), px(20), px(32))
        }
        scroll = ScrollView(this).apply {
            addView(col)
            // 画面の端まで描く端末 (Android 15 以降) で、状態バーとナビゲーションバーに重ならないように
            setOnApplyWindowInsetsListener { v, insets ->
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    val b = insets.getInsets(android.view.WindowInsets.Type.systemBars())
                    v.setPadding(b.left, b.top, b.right, b.bottom)
                }
                insets
            }
        }
        setContentView(scroll)
        build()
        jumpTo(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        jumpTo(intent)
    }

    /** キーボードのメニューの「辞書に登録」から来たときは、ユーザー辞書の所へ */
    private fun jumpTo(intent: Intent?) {
        if (intent?.getStringExtra("section") != "dict") return
        scroll.post {
            scroll.smoothScrollTo(0, dictSection.top)
            dictReading.requestFocus()
        }
    }

    override fun onResume() {
        super.onResume()
        updateSteps()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) updateSteps()   // キーボードの選択の画面から戻ったとき
    }

    private fun px(v: Int) = (v * dp).toInt()

    // ---------------------------------------------------------------- 画面の組み立て
    private fun build() {
        col.addView(text("shunti IME", 26f, bold = true))
        col.addView(text("文脈を読む AI 変換（shuntelligence）で打つ、日本語のフリックキーボード。変換は端末の中だけで行い、通信はしません。", 14f, secondary = true))

        section("キーボードを使えるようにする")
        step1 = text("", 15f)
        col.addView(step1)
        col.addView(button("キーボードの設定を開く") { startActivity(Intent(android.provider.Settings.ACTION_INPUT_METHOD_SETTINGS)) })
        step2 = text("", 15f)
        col.addView(step2)
        col.addView(button("キーボードを切り替える") { (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).showInputMethodPicker() })
        col.addView(EditText(this).apply {
            hint = "試し打ち"
            minLines = 3
            gravity = Gravity.TOP
        }, lp(top = 8))

        section("入力")
        col.addView(text("かなの配列", 14f, secondary = true))
        val kanaLayouts = listOf("FLICK", "QWERTY", "GODAN")
        col.addView(radios(listOf("フリック", "QWERTY (ローマ字)", "GODAN (ローマ字)"), kanaLayouts.indexOf(settings.kanaLayout).coerceAtLeast(0)) {
            settings.kanaLayout = kanaLayouts[it]
        })
        col.addView(switch("GODAN を左右反転", settings.godanMirror) { settings.godanMirror = it }, lp(top = 4))
        col.addView(text("英字の配列", 14f, secondary = true), lp(top = 8))
        col.addView(radios(listOf("QWERTY", "フリック"), if (settings.alphaLayout == "FLICK") 1 else 0) {
            settings.alphaLayout = if (it == 1) "FLICK" else "QWERTY"
        })
        col.addView(switch("あA キーで数字パッドにも切り替える", settings.numericPad) { settings.numericPad = it }, lp(top = 8))
        col.addView(text("オンにすると、あA キーで 日本語 → 英字 → 数字 → 日本語 と回ります。", 12f, secondary = true))

        section("変換")
        col.addView(text("変換モデル", 14f, secondary = true))
        col.addView(radios(listOf("標準 (S8plus)", "軽量 (XS5)"), if (settings.model == "light") 1 else 0) {
            settings.model = if (it == 1) "light" else "standard"
        })
        col.addView(text("変換が遅いと感じる端末では軽量にしてください。次にキーボードを開いたときから変わります。", 12f, secondary = true))

        section("振動")
        val strength = radios(listOf("弱", "中", "強"), settings.hapticStrength) {
            settings.hapticStrength = it
            haptics.tick(it)   // 強さを選んだら一度鳴らす
        }
        col.addView(switch("キー入力時の振動", settings.haptic) {
            settings.haptic = it
            for (i in 0 until strength.childCount) strength.getChildAt(i).isEnabled = it
            if (it) haptics.tick(settings.hapticStrength)
        })
        col.addView(text("強さ", 14f, secondary = true), lp(top = 8))
        col.addView(strength)
        for (i in 0 until strength.childCount) strength.getChildAt(i).isEnabled = settings.haptic
        col.addView(text("端末の「タッチ時の振動」の設定に関係なく鳴ります。", 12f, secondary = true))

        section("見た目")
        col.addView(text("テーマ", 14f, secondary = true))
        col.addView(radios(listOf("端末に合わせる", "ライト", "ダーク"), settings.theme) { settings.theme = it })
        val hLabel = text("", 14f, secondary = true)
        fun showH(v: Int) { hLabel.text = "キーボードの高さ  $v%" }
        showH(settings.heightPercent)
        col.addView(hLabel, lp(top = 12))
        col.addView(SeekBar(this).apply {
            max = (Settings.HEIGHT_MAX - Settings.HEIGHT_MIN) / 5
            progress = (settings.heightPercent - Settings.HEIGHT_MIN) / 5
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    val v = Settings.HEIGHT_MIN + p * 5
                    showH(v)
                    if (fromUser) settings.heightPercent = v
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
        })
        col.addView(slider("キーボードの下の余白 (画面下のボタンと重なるとき)", "dp", 0, 48, 4, settings.bottomExtraDp) {
            settings.bottomExtraDp = it
        }, lp(top = 8))
        col.addView(slider("キーボードの幅", "%", Settings.WIDTH_MIN, 100, 5, settings.widthPercent) { settings.widthPercent = it }, lp(top = 8))
        col.addView(slider("キーボードの位置 (0 = 左、50 = 真ん中、100 = 右)", "", 0, 100, 10, settings.offsetPercent) {
            settings.offsetPercent = it
        }, lp(top = 8))
        col.addView(text("片手モード", 14f, secondary = true), lp(top = 12))
        col.addView(radios(listOf("オフ", "左手", "右手"), listOf("off", "left", "right").indexOf(settings.oneHanded).coerceAtLeast(0)) {
            settings.oneHanded = listOf("off", "left", "right")[it]
            if (it != 0) settings.floating = false
        })
        col.addView(switch("フローティング (画面の上の好きな所に小さく出す)", settings.floating) { settings.floating = it }, lp(top = 8))
        col.addView(text("フローティングは上端のつまみで動かせます。片手・フローティングは、キーボードの ⚙ からも切り替えられます。", 12f, secondary = true))
        col.addView(switch("フリックの上下左右の字を出す", settings.flickHints) { settings.flickHints = it }, lp(top = 8))
        col.addView(text("見た目の変更は、キーボードを次に開いたときに反映されます。", 12f, secondary = true))

        section("ユーザー辞書")
        dictSection = col.getChildAt(col.childCount - 1)
        col.addView(text(
            "登録した単語は、AI 変換が文の中でもほかの語と比べて選びます (しゅんてぃはすごい → shuntiはすごい)。" +
                "品詞を選ぶと、動詞・形容詞は活用した形 (ググれば・ググった) でも変換されます。読みがちょうど一致したときは候補の先頭に出ます。",
            14f, secondary = true,
        ))
        dictReading = EditText(this).apply { hint = "読み (ひらがな)" }
        val dictWord = EditText(this).apply { hint = "単語" }
        col.addView(dictReading)
        col.addView(dictWord)
        val posKeys = UserDict.POS_LABELS.keys.toList()
        var pos = "noun"
        var group = "godan"
        val groupRow = radios(listOf("五段 (書く・ググる)", "一段 (食べる)"), 0) { group = if (it == 1) "ichidan" else "godan" }
        groupRow.visibility = View.GONE
        col.addView(text("品詞", 14f, secondary = true), lp(top = 8))
        col.addView(radios(UserDict.POS_LABELS.values.toList(), 0) {
            pos = posKeys[it]
            groupRow.visibility = if (pos == "verb") View.VISIBLE else View.GONE
        })
        col.addView(groupRow)
        val dictError = text("", 13f, secondary = true)
        col.addView(button("登録") {
            val err = UserDict.get(this).add(dictReading.text.toString(), dictWord.text.toString(), pos, group)
            dictError.text = err ?: ""
            if (err == null) {
                dictReading.setText("")
                dictWord.setText("")
                refreshDict()
            }
        })
        col.addView(dictError)
        dictList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        col.addView(dictList)
        refreshDict()

        section("学習")
        col.addView(text("選んだ候補を覚えて、次から前に出します。覚えた中身はこの端末の中だけに保存します。", 14f, secondary = true))
        col.addView(button("学習をリセット") {
            AlertDialog.Builder(this)
                .setTitle("学習をリセット")
                .setMessage("覚えた変換をすべて消します。元に戻せません。")
                .setPositiveButton("消す") { _, _ ->
                    Learning.get(this).apply { clear(); save() }
                    SentenceFlow.init(this); SentenceFlow.clear()
                }
                .setNegativeButton("やめる", null)
                .show()
        })

        section("応援する")
        col.addView(text("shunti IME は無料です。気に入ったら、開発の応援をしてもらえるとうれしいです。", 14f, secondary = true))
        col.addView(button("Buy Me a Coffee") { open("https://buymeacoffee.com/shunti") })
        col.addView(button("GitHub Sponsors") { open("https://github.com/sponsors/shuntilettuce") })

        section("このアプリについて")
        val ver = packageManager.getPackageInfo(packageName, 0).versionName
        col.addView(text("バージョン $ver", 14f, secondary = true))
        col.addView(button("ライセンス") { showDoc("ライセンス", "licenses.txt") })
        col.addView(button("プライバシーポリシー") { showDoc("プライバシーポリシー", "privacy.txt") })
        col.addView(button("ソースコード (GitHub)") { open("https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next") })
    }

    private fun open(url: String) {
        runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
    }

    /** 登録した単語の一覧 (新しいものが上。それぞれ削除できる) */
    private fun refreshDict() {
        dictList.removeAllViews()
        val dict = UserDict.get(this)
        for (e in dict.list().asReversed()) {
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val posLabel = UserDict.POS_LABELS[e.pos] ?: "名詞"
            val g = if (e.pos == "verb") (if (e.group == "ichidan") "・一段" else "・五段") else ""
            row.addView(text("${e.reading} → ${e.word}  ($posLabel$g)", 15f), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(button("削除") {
                dict.remove(e)
                refreshDict()
            })
            dictList.addView(row)
        }
        if (dict.list().isEmpty()) dictList.addView(text("まだ登録していません", 13f, secondary = true))
    }

    /** 手順の状態: 有効にしたか・選んでいるか (設定の画面から戻るたびに見直す) */
    private fun updateSteps() {
        if (!::step1.isInitialized) return
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        val enabled = imm.enabledInputMethodList.any { it.packageName == packageName }
        val cur = android.provider.Settings.Secure.getString(contentResolver, android.provider.Settings.Secure.DEFAULT_INPUT_METHOD) ?: ""
        val selected = cur.startsWith("$packageName/")
        step1.text = if (enabled) "✓ 1. キーボードの一覧で shunti IME を有効にした" else "1. キーボードの一覧で shunti IME を有効にする"
        step2.text = if (selected) "✓ 2. shunti IME に切り替えた" else "2. 入力欄をタップして、shunti IME に切り替える"
    }

    private fun showDoc(title: String, asset: String) {
        val body = runCatching { assets.open(asset).bufferedReader().readText() }.getOrDefault("")
        val tv = text(body, 13f).apply {
            setPadding(px(20), px(8), px(20), px(8))
            setTextIsSelectable(true)
            movementMethod = LinkMovementMethod.getInstance()
        }
        AlertDialog.Builder(this)
            .setTitle(title)
            .setView(ScrollView(this).apply { addView(tv) })
            .setPositiveButton("閉じる", null)
            .show()
    }

    // ---------------------------------------------------------------- 部品
    private fun lp(top: Int = 0) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
        topMargin = px(top)
    }

    private fun section(title: String) {
        col.addView(text(title, 18f, bold = true), lp(top = 28))
    }

    private fun text(s: String, sp: Float, bold: Boolean = false, secondary: Boolean = false) = TextView(this).apply {
        text = s
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sp)
        if (bold) typeface = Typeface.DEFAULT_BOLD
        // 見出し・本文はテーマのはっきりした文字色、補足は薄い文字色
        val attr = obtainStyledAttributes(intArrayOf(if (secondary) android.R.attr.textColorSecondary else android.R.attr.textColorPrimary))
        attr.getColorStateList(0)?.let { setTextColor(it) }
        attr.recycle()
        setPadding(0, px(4), 0, px(4))
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        setOnClickListener { onClick() }
    }

    @Suppress("DEPRECATION")
    private fun switch(label: String, on: Boolean, onChange: (Boolean) -> Unit) = Switch(this).apply {
        text = label
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        isChecked = on
        setPadding(0, px(8), 0, px(8))
        setOnCheckedChangeListener { _, v -> onChange(v) }
    }

    /** 目盛りつきのつまみ。値は min..max を step 刻み */
    private fun slider(label: String, unit: String, min: Int, max: Int, step: Int, value: Int, onChange: (Int) -> Unit): View {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val t = text("", 14f, secondary = true)
        fun show(v: Int) { t.text = "$label  $v$unit" }
        show(value)
        box.addView(t)
        box.addView(SeekBar(this).apply {
            this.max = (max - min) / step
            progress = (value - min) / step
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    val v = min + p * step
                    show(v)
                    if (fromUser) onChange(v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
        })
        return box
    }

    private fun radios(labels: List<String>, selected: Int, onPick: (Int) -> Unit) = RadioGroup(this).apply {
        orientation = RadioGroup.HORIZONTAL
        for ((i, l) in labels.withIndex()) {
            addView(RadioButton(this@MainActivity).apply {
                id = View.generateViewId()
                text = l
                isChecked = i == selected
                setPadding(0, 0, px(12), 0)
                setOnClickListener { onPick(i) }
            })
        }
    }
}

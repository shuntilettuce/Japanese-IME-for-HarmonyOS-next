package com.shunti.japaneseime

import android.content.Context
import android.content.SharedPreferences
import android.content.res.Configuration
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/** アプリの設定 (端末の中の SharedPreferences にだけ保存)。キーボードは開くたびに読み直す */
class Settings(context: Context) {
    private val p: SharedPreferences = context.getSharedPreferences("shunti", Context.MODE_PRIVATE)

    init {
        // 高さの基準を 1.2 倍にした (前の 120% が今の 100%)。前に保存した値は、同じ高さになるよう 1 度だけ換算する
        if (!p.getBoolean("heightBase2", false)) {
            val e = p.edit().putBoolean("heightBase2", true)
            if (p.contains("heightPercent")) {
                val v = (p.getInt("heightPercent", 100) / 1.2f / 5).let { Math.round(it) * 5 }
                e.putInt("heightPercent", v.coerceIn(HEIGHT_MIN, HEIGHT_MAX))
            }
            e.apply()
        }
    }

    var haptic: Boolean
        get() = p.getBoolean("haptic", true)
        set(v) = p.edit().putBoolean("haptic", v).apply()

    /** 振動の強さ: 0 弱, 1 中, 2 強 */
    var hapticStrength: Int
        get() = p.getInt("hapticStrength", 0)
        set(v) = p.edit().putInt("hapticStrength", v).apply()

    /** 0 端末に合わせる, 1 ライト, 2 ダーク */
    var theme: Int
        get() = p.getInt("theme", 0)
        set(v) = p.edit().putInt("theme", v).apply()

    /** キーボードの高さ (100 = 標準) */
    var heightPercent: Int
        get() = p.getInt("heightPercent", 100)
        set(v) = p.edit().putInt("heightPercent", v).apply()

    /** キーボードの下の余白 (dp)。画面下のボタンとキーが重なる端末で足す */
    var bottomExtraDp: Int
        get() = p.getInt("bottomExtraDp", 0)
        set(v) = p.edit().putInt("bottomExtraDp", v).apply()

    /** フリックの上下左右の小さい字 */
    var flickHints: Boolean
        get() = p.getBoolean("flickHints", true)
        set(v) = p.edit().putBoolean("flickHints", v).apply()

    /** GODAN の左右を逆にする (機能キーを左に) */
    var godanMirror: Boolean
        get() = p.getBoolean("godanMirror", false)
        set(v) = p.edit().putBoolean("godanMirror", v).apply()

    /** かなの配列: "FLICK" (フリック)・"QWERTY" (ローマ字)・"GODAN" (母音と子音、ローマ字) */
    var kanaLayout: String
        get() = p.getString("kanaLayout", "FLICK") ?: "FLICK"
        set(v) = p.edit().putString("kanaLayout", v).apply()

    /** 英字の配列: "QWERTY" か "FLICK" */
    var alphaLayout: String
        get() = p.getString("alphaLayout", "QWERTY") ?: "QWERTY"
        set(v) = p.edit().putString("alphaLayout", v).apply()

    /** 長い入力の扱い: false = 保留 (既定。前の方は候補を持たせて入力中のまま)、true = オート確定 (前の方から次々に確定) */
    /** 提案 (打ち間違いの直し・日付・住所・絵文字・顔文字・かっこ など) を候補に混ぜず、候補の帯の上の行に分けて出す */
    /** 提案を選んだ回数 (表記 -> 回数)。よく選ぶ提案を前に */
    val suggestUse: Map<String, Int>
        get() = runCatching {
            val o = org.json.JSONObject(p.getString("suggestUse", "{}") ?: "{}")
            o.keys().asSequence().associateWith { o.getInt(it) }
        }.getOrDefault(emptyMap())

    fun recordSuggestUse(t: String) {
        val m = suggestUse.toMutableMap()
        m[t] = (m[t] ?: 0) + 1
        p.edit().putString("suggestUse", org.json.JSONObject(m as Map<*, *>).toString()).apply()
    }

    var separateSuggest: Boolean
        get() = true   // 設定は廃止 (2026-10-14)
        set(v) = p.edit().putBoolean("separateSuggest", v).apply()

    /** 差別的・不適切とされる語も候補に出す (既定オン。ユーザー 2026-10-13) */
    var allowSlurs: Boolean
        get() = true   // 常に出す (設定は廃止、2026-10-13)
        set(v) = p.edit().putBoolean("allowSlurs", v).apply()

    var liveCommit: Boolean
        get() = false   // 設定は廃止 (2026-10-14)
        set(v) = p.edit().putBoolean("liveCommit", v).apply()

    /** 変換モデル: "standard" (S6、既定) か "light" (XS3、性能の低い端末向け) */
    var model: String
        get() = p.getString("model", "standard") ?: "standard"
        set(v) = p.edit().putString("model", v).apply()

    /** 変換モデルの資産の名前 */
    val modelFile: String get() = if (model == "light") "kkc_model_light.bin" else "kkc_model.bin"

    /** あA キーで 日本語 → 英字 → 数字パッド と回すか (HarmonyOS 版と同じく既定はオフ) */
    var numericPad: Boolean
        get() = p.getBoolean("numericPad", false)
        set(v) = p.edit().putBoolean("numericPad", v).apply()

    /** キーボードの幅 (100 = 画面いっぱい) と左右の位置 (0 = 左寄せ, 50 = 真ん中, 100 = 右寄せ) */
    var widthPercent: Int
        get() = p.getInt("widthPercent", 100)
        set(v) = p.edit().putInt("widthPercent", v).apply()
    var offsetPercent: Int
        get() = p.getInt("offsetPercent", 50)
        set(v) = p.edit().putInt("offsetPercent", v).apply()

    /** 片手モード: "off" / "left" / "right" */
    var oneHanded: String
        get() = p.getString("oneHanded", "off") ?: "off"
        set(v) = p.edit().putString("oneHanded", v).apply()

    /** フローティング (画面の上の好きな所に小さく出す) と、その位置 (画面に対する割合) */
    var floating: Boolean
        get() = p.getBoolean("floating", false)
        set(v) = p.edit().putBoolean("floating", v).apply()
    var floatX: Float
        get() = p.getFloat("floatX", 0.12f)
        set(v) = p.edit().putFloat("floatX", v).apply()
    var floatY: Float
        get() = p.getFloat("floatY", 0.45f)
        set(v) = p.edit().putFloat("floatY", v).apply()

    fun isDark(context: Context): Boolean = when (theme) {
        1 -> false
        2 -> true
        else -> (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
    }

    val prefs get() = p

    companion object {
        const val HEIGHT_MIN = 80
        const val HEIGHT_MAX = 130
        const val WIDTH_MIN = 60
        const val ONE_HAND_PERCENT = 80   // 片手モードの幅
        const val FLOAT_PERCENT = 72      // フローティングの幅
    }
}

/**
 * キーを押したときの振動。振動モーターを直接鳴らすので、端末の「タッチ時の振動」の設定に関係なく鳴る
 * (VIBRATE はインストール時に自動で許可される権限で、確認の画面は出ない)。
 */
class Haptics(context: Context) {
    private val vib: Vibrator? =
        if (Build.VERSION.SDK_INT >= 31) (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
        else @Suppress("DEPRECATION") (context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator)

    /** strength: 0 弱 (カチッ), 1 中 (クリック), 2 強 (重いクリック) */
    fun tick(strength: Int) {
        val v = vib ?: return
        if (!v.hasVibrator()) return
        val effect = if (Build.VERSION.SDK_INT >= 29) {
            VibrationEffect.createPredefined(
                when (strength) {
                    2 -> VibrationEffect.EFFECT_HEAVY_CLICK
                    1 -> VibrationEffect.EFFECT_CLICK
                    else -> VibrationEffect.EFFECT_TICK
                },
            )
        } else {
            VibrationEffect.createOneShot(longArrayOf(10, 20, 35)[strength.coerceIn(0, 2)], VibrationEffect.DEFAULT_AMPLITUDE)
        }
        runCatching { v.vibrate(effect) }
    }
}

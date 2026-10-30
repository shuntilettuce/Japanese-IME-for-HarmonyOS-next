package com.shunti.japaneseime

import android.content.Context
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily
import android.os.Build

/**
 * キーボード・候補の字のフォント (HarmonyOS 版の JpFont.ets と同じ 2 本。tools/build_ud_font.py で作る)。
 * 本体は BIZ UDP ゴシック (ユニバーサルデザインのフォント。シ/ツ・ば/ぱ が見分けやすい)。
 * BIZ UDP ゴシックに無いまれな漢字 (約 1,800 字) は、それだけを切り出した Noto Sans JP で補う。
 * 端末のフォントに任せると、中国語の端末では漢字が中国語の字形になる。
 * 絵文字・変体仮名など 2 本のどちらにも無い字は端末のフォントで出す。
 */
object JpFont {
    private const val UD = "font/BIZUDPGothic-Regular.ttf"
    private const val FALLBACK = "font/NotoSansJP-Fallback.ttf"
    @Volatile private var cached: Typeface? = null

    fun get(context: Context): Typeface = cached ?: synchronized(this) {
        cached ?: build(context.applicationContext).also { cached = it }
    }

    private fun build(context: Context): Typeface = try {
        val assets = context.assets
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            Typeface.CustomFallbackBuilder(FontFamily.Builder(Font.Builder(assets, UD).build()).build())
                .addCustomFallback(FontFamily.Builder(Font.Builder(assets, FALLBACK).build()).build())
                .setSystemFallback("sans-serif")
                .build()
        } else {
            Typeface.createFromAsset(assets, UD)   // Android 9 まで: 補いの 1 本は付けられない (無い字は端末のフォント)
        }
    } catch (e: Exception) {
        android.util.Log.e("shuntiIME", "JP font load failed", e)
        Typeface.DEFAULT
    }
}

package com.shunti.japaneseime

/**
 * 括弧の全種類 (開き, 閉じ)。どの括弧も必ず打てるように、変換の候補はここから出す。
 * HarmonyOS 版 ime/Brackets.ets・Windows 版 desktop/core/composer.cpp の BRACKETS も同じ並び (変えるときは 3 つとも)。
 */
object Brackets {
    val ALL = listOf(
        "（" to "）", "(" to ")", "「" to "」", "『" to "』", "【" to "】", "［" to "］", "[" to "]",
        "｛" to "｝", "{" to "}", "〔" to "〕", "〈" to "〉", "《" to "》", "〖" to "〗", "〘" to "〙",
        "〚" to "〛", "｢" to "｣", "＜" to "＞", "<" to ">", "«" to "»", "‹" to "›", "“" to "”",
        "‘" to "’", "〝" to "〟", "｟" to "｠",
    )
    /** 開きを入れたら閉じも組で入れる括弧 (＜ < ‘ は不等号・アポストロフィにも使うので除く。HarmonyOS 版 BRACKET_PAIRS) */
    val PAIRS: Map<String, String> = ALL.filter { it.first !in setOf("＜", "<", "‘") }.toMap()

    private val OPENS = ALL.map { it.first }
    private val CLOSES = ALL.map { it.second }

    /**
     * 読みに合う括弧の候補。括弧 1 字 → 同じ側 (開き・閉じ) の全種類 (打った字が先頭)、
     * かっこ → 全種類の組、かっこひらき・かっことじ → 開き・閉じの全種類。括弧でない読みは空
     */
    fun variants(reading: String): List<String> = when {
        reading == "かっこ" -> ALL.map { it.first + it.second }
        reading == "かっこひらき" -> OPENS
        reading == "かっことじ" -> CLOSES
        reading in OPENS -> listOf(reading) + OPENS.filter { it != reading }
        reading in CLOSES -> listOf(reading) + CLOSES.filter { it != reading }
        else -> marks(reading)
    }

    /**
     * 記号 1 字: 全角・半角の両方と仲間の記号 (打った字が先頭)。MS-IME と同じく・で ／ /、￥ で ＼ \ (ブログの報告 2026-11-02。
     * Windows 版 composer.cpp の MARKS・HarmonyOS 版 Brackets.ets と同じ)。英数字は空
     */
    private val MARKS = listOf(
        listOf("・", "／", "/"), listOf("、", "，", ","), listOf("。", "．", "."), listOf("ー", "－", "-", "―", "‐"), listOf("～", "〜", "~"),
        listOf("￥", "＼", "\\"), listOf("＊", "*", "※", "×"), listOf("＂", "\"", "“", "”"), listOf("＇", "'", "‘", "’"),
    )

    private fun marks(reading: String): List<String> {
        if (reading.length != 1) return emptyList()
        val group = MARKS.firstOrNull { reading in it } ?: run {
            val ch = reading[0]
            val half = if (ch in '！'..'～') ch - 0xFEE0 else ch
            if (half <= ' ' || half > '~' || half.isLetterOrDigit()) return emptyList()
            listOf((half + 0xFEE0).toString(), half.toString())
        }
        return listOf(reading) + group.filter { it != reading }
    }
}

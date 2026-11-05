package com.shunti.japaneseime

/**
 * ローマ字 → かな (HarmonyOS 版 ime/JapaneseConverter.ets の移植。決まりは向こうと同じ)。
 * 表 (ROMAJI_TABLE) は Tables が HarmonyOS 版から読んだもの。打ったキーを溜めて、かなに決まった分を committed、
 * まだ決まらないローマ字を pending として返す (kk → っk、n の後が母音・y・n 以外なら ん)。
 */
class JapaneseConverter(private val table: Map<String, String>) {
    class Result(val committed: String, val pending: String)

    private var buffer = ""
    private val prefixes: Set<String> = HashSet<String>().apply {
        for (k in table.keys) for (i in 1..k.length) add(k.substring(0, i))
        add("n")
    }

    fun reset() {
        buffer = ""
    }

    fun backspace() {
        if (buffer.isNotEmpty()) buffer = buffer.dropLast(1)
    }

    fun pending() = buffer

    fun flushPending(): String {
        val p = if (buffer == "n") "ん" else buffer   // PC と同じ。語末の n は ん
        buffer = ""
        return p
    }

    fun processKey(key: String): Result {
        buffer += key.lowercase()
        if (buffer.length >= 2) {
            val c0 = buffer[0]
            if (c0 == buffer[1] && c0 != 'n' && c0 in CONSONANTS) {
                buffer = buffer.substring(1)
                val sub = processBuffered()
                return Result("っ" + sub.committed, sub.pending)
            }
        }
        return processBuffered()
    }

    private fun processBuffered(): Result {
        if (buffer == "n") return Result("", "n")
        if (buffer.length >= 2 && buffer[0] == 'n' && buffer[1] !in "aiueoyn") {
            buffer = buffer.substring(1)
            val sub = processBuffered()
            return Result("ん" + sub.committed, sub.pending)
        }
        table[buffer]?.let {
            buffer = ""
            return Result(it, "")
        }
        if (buffer in prefixes) return Result("", buffer)
        for (i in buffer.length - 1 downTo 1) {
            val kana = table[buffer.substring(0, i)] ?: continue
            buffer = buffer.substring(i)
            val sub = processBuffered()
            return Result(kana + sub.committed, sub.pending)
        }
        val emitted = buffer.substring(0, 1)
        buffer = buffer.substring(1)
        if (buffer.isNotEmpty()) {
            val sub = processBuffered()
            return Result(emitted + sub.committed, sub.pending)
        }
        return Result(emitted, "")
    }

    companion object {
        private const val CONSONANTS = "bcdfghjklmnpqrstvwxyz"
    }
}

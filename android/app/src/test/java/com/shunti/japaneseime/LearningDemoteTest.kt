package com.shunti.japaneseime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/** 打ち直しで前の表記の票を減らす (demote) と、確定の取り消しで戻ること (2026-11-06) */
class LearningDemoteTest {
    @Test
    fun demoteAndRestore() {
        val f = File.createTempFile("learned", ".json").also { it.delete() }
        val l = Learning(f)
        l.recordChoice("やまば", "山場", "今日が")
        l.recordChoice("やまば", "山場", "今日が")
        assertEquals("山場", l.top("やまば", "今日が"))
        // 票の無い表記は何もしない
        assertNull(l.demote("やまば", "矢場", "今日が"))
        val pick = l.recordChoice("やまば", "矢場", "今日が")
        val d = l.demote("やまば", "山場", "今日が")!!
        val u = l.join(d, pick)!!
        assertNull(l.top("やまば", "今日が"))   // 山場 1 票・矢場 1 票で決まらない
        l.restore(u)
        assertEquals("山場", l.top("やまば", "今日が"))
        // 1 票まで減らすと表記ごと消える
        l.demote("やまば", "山場", "今日が")
        l.demote("やまば", "山場", "今日が")
        assertNull(l.demote("やまば", "山場", "今日が"))   // もう票が無い
        f.delete()
    }

    /** 確定の取り消しで、押し出した古い文脈の票と上書きした品詞も戻る (Muse 依頼 19 の A・B。PC 版と同じ。2026-11-13) */
    @Test
    fun undoRestoresEvictedContextAndPos() {
        val f = File.createTempFile("learned", ".json").also { it.delete() }
        val l = Learning(f)
        for (left in listOf("あい", "かき", "さし", "たち", "なに", "はひ", "まみ", "やゆ")) l.recordChoice("きごう", "記号", left, 5)
        l.save()
        val before = f.readText()
        val u = l.undoOf(listOfNotNull(l.recordChoice("きごう", "記号", "らり", 9)))!!   // 9 個目の文脈で「あい」が押し出され、品詞は 9 に
        l.save()
        assert(f.readText() != before)
        l.restore(u)
        l.save()
        assertEquals(before, f.readText())
        f.delete()
    }
}

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
}

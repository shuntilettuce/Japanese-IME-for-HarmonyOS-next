// 直前の kkc_convert の 1 位の経路を、PC 版 (desktop/core/converter.cpp の group_phrases) と同じ決まりで文節 (自立語 + 付属語)
// にまとめ、文節ごとの読みの終わりを返す。スマホの学習の置き換え (withLearned) を PC と同じ文節の単位にするのに使う
// (語の単位で引くと、か・と・て のような 1 字の付属語まで覚えた表記に置き換わった。ユーザー 2026-11-07)
#pragma once
#include <cstdint>

#include "../../../../desktop/core/pos_classes.h"
#include "engine.h"

// kana = 変換した読み、top = 1 位の候補 (kkc_convert が返した 1 位のまま)。返り値は文節の数 (分からなければ 0)
inline int kkc_phrase_ends(kkc_engine* e, const uint16_t* kana, int nk, const uint16_t* top, int nt, int32_t* out, int cap) {
    constexpr int W = 512;
    int32_t ends[W], lens[W], lids[W], rids[W];
    const int m = kkc_last_segments(e, ends, lens, W);
    if (m <= 0 || kkc_last_segment_pos(e, lids, rids, W) != m) return 0;
    auto cls = [](int id) { return id >= 0 && id < shunti::POS_CLASS_COUNT ? shunti::POS_CLASSES[id] : '0'; };
    auto punct = [](const uint16_t* s, int n) {
        if (n <= 0) return false;
        static const char16_t P[] = u"、。，．！？!?…‥・,.";
        for (int i = 0; i < n; i++) {
            bool hit = false;
            for (const char16_t* p = P; *p; p++) hit = hit || s[i] == *p;
            if (!hit) return false;
        }
        return true;
    };
    auto reading_char = [](uint16_t c) { return (c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x3094; };
    int n = 0, rs = 0, ts = 0;
    bool in_tail = false, prev_pass = false;
    int prev_rid = -1;
    for (int i = 0; i < m; i++) {
        const int re = ends[i], tl = lens[i];
        if (re <= rs || re > nk || ts + tl > nt) return 0;
        const uint16_t* r = kana + rs;
        const uint16_t* s = top + ts;
        const int rl = re - rs;
        bool pass = rl == tl && !reading_char(r[0]);
        for (int k = 0; pass && k < rl; k++) pass = r[k] == s[k];
        bool attach = false, head = false;
        if (i > 0) {
            attach = cls(lids[i]) == '1' || punct(s, tl) || (cls(lids[i]) == '4' && cls(prev_rid) == '3');
            head = !attach && (cls(prev_rid) == '2' || (prev_pass && pass)) && !in_tail;
        }
        if ((attach || head) && n > 0) {
            out[n - 1] = re;
            if (!head) in_tail = true;
        } else {
            if (n >= cap) return 0;
            out[n++] = re;
            in_tail = false;
        }
        prev_rid = rids[i];
        prev_pass = pass;
        rs = re;
        ts += tl;
    }
    return rs == nk ? n : 0;
}

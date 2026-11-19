#include "typo_alts.h"

#include <algorithm>
#include <set>

namespace shunti {

bool kana_modify(char16_t& prev, char16_t c) {
    static const u16 DAKU_FROM = u"かきくけこさしすせそたちつてとはひふへほうカキクケコサシスセソタチツテトハヒフヘホウ";
    static const u16 DAKU_TO = u"がぎぐげござじずぜぞだぢづでどばびぶべぼゔガギグゲゴザジズゼゾダヂヅデドバビブベボヴ";
    static const u16 HAN_FROM = u"はひふへほハヒフヘホ";
    static const u16 HAN_TO = u"ぱぴぷぺぽパピプペポ";
    static const u16 SMALL_FROM = u"あいうえおやゆよつわかけアイウエオヤユヨツワカケ";
    static const u16 SMALL_TO = u"ぁぃぅぇぉゃゅょっゎゕゖァィゥェォャュョッヮヵヶ";
    size_t at;
    if (c == u'') {
        if ((at = SMALL_FROM.find(prev)) != u16::npos) prev = SMALL_TO[at];
        else if ((at = SMALL_TO.find(prev)) != u16::npos) prev = SMALL_FROM[at];
        return true;
    }
    if (c == u'') {
        if ((at = DAKU_FROM.find(prev)) != u16::npos) prev = DAKU_TO[at];                 // か -> が
        else if ((at = DAKU_TO.find(prev)) != u16::npos) {                                // が -> か、ば -> ぱ
            const char16_t plain = DAKU_FROM[at];
            const size_t h = HAN_FROM.find(plain);
            prev = h != u16::npos ? HAN_TO[h] : plain;
        } else if ((at = HAN_TO.find(prev)) != u16::npos) prev = HAN_FROM[at];            // ぱ -> は
        else if ((at = SMALL_FROM.find(prev)) != u16::npos) prev = SMALL_TO[at];          // や -> ゃ
        else if ((at = SMALL_TO.find(prev)) != u16::npos) prev = SMALL_FROM[at];
        return true;
    }
    if (c == u'゛' || c == u'゜') {
        const u16& from = c == u'゛' ? DAKU_FROM : HAN_FROM;
        const u16& to = c == u'゛' ? DAKU_TO : HAN_TO;
        if ((at = from.find(prev)) != u16::npos) { prev = to[at]; return true; }
    }
    return false;
}

u16 decode_unit_texts(const std::vector<u16>& outs, bool letters, Romaji& rom) {
    u16 kana;
    if (letters) {   // Composer の replay と同じ: ローマ字の字は表に、それ以外は打ちかけを出してからそのまま
        rom.reset();
        for (auto& o : outs)
            for (char16_t key : o) {
                const char16_t lk = key >= u'A' && key <= u'Z' ? char16_t(key + 32) : key;
                if (key == lk && ((lk >= u'a' && lk <= u'z') || lk == u'-' || lk == u'\'' || rom.is_prefix(u16(1, lk)))) {
                    kana += rom.process(lk).committed;
                } else {
                    kana += rom.flush();
                    kana += key;
                }
            }
        kana += rom.flush();
        return kana;
    }
    for (auto& o : outs)
        for (char16_t c : o) {
            const bool mod = c == u'' || c == u'' || c == u'゛' || c == u'゜';
            if (mod && !kana.empty() && kana_modify(kana.back(), c)) continue;
            if (c == u'' || c == u'') continue;
            kana.push_back(c);
        }
    return kana;
}

bool alt_readings(const std::vector<u16>& orig, const std::vector<std::vector<u16>>& alts, bool letters, Romaji& rom,
                  const u16& all, size_t off, std::vector<u16>& out) {
    out.clear();
    const size_t n = all.size();
    if (off > n) return false;
    const u16 d0 = decode_unit_texts(orig, letters, rom);
    if (d0.size() != n) return false;
    std::vector<char> diff(n);
    for (size_t i = 0; i < n; i++) diff[i] = d0[i] != all[i];
    auto kana_only = [](const u16& s) {
        for (char16_t c : s) if (!((c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) || c == u'ー')) return false;
        return true;
    };
    const u16 reading = all.substr(off);
    std::set<u16> seen;
    for (auto& a : alts) {
        const u16 d = decode_unit_texts(a, letters, rom);
        const size_t m = d.size();
        size_t p = 0, q = 0;
        while (p < n && p < m && d[p] == d0[p]) p++;
        while (q < n - p && q < m - p && d[m - 1 - q] == d0[n - 1 - q]) q++;
        if ((p == n && m == n) || p < off) continue;
        bool ok = true;
        for (size_t i = p > 0 ? p - 1 : 0; i < std::min(n, n - q + 1); i++) if (diff[i]) { ok = false; break; }
        const u16 mid = d.substr(p, m - q - p);
        if (!ok || !kana_only(mid)) continue;
        u16 alt = all.substr(off, p - off) + mid + all.substr(n - q);
        if (alt != reading && seen.insert(alt).second) out.push_back(std::move(alt));
    }
    return true;
}

const Layout& qwerty_layout() {
    static const Layout L = [] {
        Layout x;
        x.load("#! name QWERTY のローマ字\n#! kind romaji\n");   // 綴りも読み替えも無い: キーがそのまま単位
        return x;
    }();
    return L;
}

bool units_from_keys(const u16& keys, std::vector<std::string>& units) {
    units.clear();
    for (char16_t c : keys) {
        if (!((c >= u'a' && c <= u'z') || c == u'-' || c == u'\'' || c == u';' || c == u':' || c == u',' || c == u'.' || c == u'/' ||
              c == u'[' || c == u']' || (c >= u'0' && c <= u'9')))
            return false;
        units.push_back(std::string(1, char(c)));
    }
    return !units.empty();
}

}  // namespace shunti

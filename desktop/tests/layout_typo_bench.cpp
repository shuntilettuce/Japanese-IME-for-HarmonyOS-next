// キー配列の打ち間違いの直しのベンチ (2026-11-15)。
//   layout_typo_bench.exe <kkc_lex.bin> <kkc_model.bin> <layouts のフォルダ> <問題.tsv> <配列名,配列名…|all> [件数] [種]
// 問題.tsv: 種類<TAB>左の文脈<TAB>読み (かな)。読みを配列の打鍵の単位に直し、1 か所だけ物理キーの打ち間違い
// (隣のキー・別の面・抜け・余分な 1 打・入れ替え) を入れて変換し、打ち間違いの無い読みの 1 位に戻るかを数える。
//   off = 今までの直し (読みからローマ字を作って QWERTY の隣で打ち直す)、on = 物理キーの打ち直し (kkc_set_alt_readings)
//   誤爆 = 打ち間違いの無い読みに打ち直しを渡したときに 1 位が変わった数
// 配列を使わない QWERTY のローマ字 (qwerty) も同じ物差しで測る (アプリでは QWERTY のローマ字には使わない。比べるため)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../core/converter.h"
#include "../core/layout.h"
#include "../core/romaji.h"
#include "../core/tables.h"
#include "../core/text.h"
#include "../core/typo_alts.h"

using namespace shunti;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool kana_only(const u16& s) {
    for (char16_t c : s) if (!((c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) || c == u'ー')) return false;
    return !s.empty();
}

bool kana_modify_local(char16_t& prev, char16_t c) {   // typo_alts の kana_modify と同じ
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
        if ((at = DAKU_FROM.find(prev)) != u16::npos) prev = DAKU_TO[at];
        else if ((at = DAKU_TO.find(prev)) != u16::npos) {
            const char16_t plain = DAKU_FROM[at];
            const size_t h = HAN_FROM.find(plain);
            prev = h != u16::npos ? HAN_TO[h] : plain;
        } else if ((at = HAN_TO.find(prev)) != u16::npos) prev = HAN_FROM[at];
        else if ((at = SMALL_FROM.find(prev)) != u16::npos) prev = SMALL_TO[at];
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

struct Lay {
    std::string name;
    Layout L;
    bool qwerty = false;   // 配列なしの QWERTY のローマ字
    bool letters = true;
    Romaji rom;
    u16 text(const std::string& u) const { return qwerty ? u16(1, char16_t(u[0])) : L.unit_text(u); }
};

u16 decode(Lay& y, const std::vector<std::string>& units) {
    u16 kana;
    if (y.letters) {
        y.rom.reset();
        for (auto& u : units)
            for (char16_t key : y.text(u)) {
                const char16_t lk = key >= u'A' && key <= u'Z' ? char16_t(key + 32) : key;
                if ((lk >= u'a' && lk <= u'z') || lk == u'-' || lk == u'\'' || y.rom.is_prefix(u16(1, lk))) kana += y.rom.process(lk).committed;
                else { kana += y.rom.flush(); kana += key; }
            }
        kana += y.rom.flush();
        return kana;
    }
    for (auto& u : units)
        for (char16_t c : y.text(u)) {
            const bool mod = c == u'' || c == u'' || c == u'゛' || c == u'゜';
            if (mod && !kana.empty() && kana_modify(kana.back(), c)) continue;
            if (c == u'' || c == u'') continue;
            kana.push_back(c);
        }
    return kana;
}

// 読み → 打鍵の単位 (作れなければ空)
std::vector<std::string> encode(Lay& y, const u16& kana) {
    std::vector<std::string> out;
    if (y.letters) {
        // 綴り: 配列の綴り (kind romaji) とふつうの表から、かなのいちばん長い一致・綴りのいちばん短いもの
        std::map<u16, u16> best;
        auto put = [&](const u16& k, const u16& v) {
            auto it = best.find(v);
            if (it == best.end() || k.size() < it->second.size()) best[v] = k;
        };
        if (!y.qwerty)
            for (auto& [k, v] : y.L.romaji_pairs()) put(k, v);
        u16 spell;
        auto try_spell = [&](bool use_layout) -> bool {
            spell.clear();
            if (!use_layout) {   // ふつうのローマ字 (ん・っ の決まりも)
                if (!y.qwerty && y.L.romaji_base_empty()) return false;
                spell = y.rom.to_romaji(kana);
                return true;
            }
            for (size_t i = 0; i < kana.size();) {
                size_t len = 0;
                u16 piece;
                if (use_layout)
                    for (size_t n = std::min<size_t>(4, kana.size() - i); n >= 1 && !len; n--) {
                        auto it = best.find(kana.substr(i, n));
                        if (it != best.end()) { piece = it->second; len = n; }
                    }
                if (!len) {
                    if (!y.qwerty && y.L.romaji_base_empty()) return false;
                    piece = y.rom.to_romaji(kana.substr(i, 1));
                    // 拗音などは 2 字で
                    if (i + 1 < kana.size()) {
                        const u16 two = y.rom.to_romaji(kana.substr(i, 2));
                        if (u16(u"ゃゅょぁぃぅぇぉ").find(kana[i + 1]) != u16::npos) { piece = two; len = 2; }
                    }
                    if (!len) len = 1;
                }
                spell += piece;
                i += len;
            }
            return true;
        };
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            if (pass == 0 && best.empty()) continue;
            if (!try_spell(pass == 0)) continue;
            // 綴りの字 → 単位 (読み替えるキーは出る字のいちばん長い一致)
            out.clear();
            std::map<u16, std::string> inv;
            if (!y.qwerty && y.L.has_key_map())
                for (auto& u : y.L.all_units()) {
                    if (u.compare(0, 2, "S-") == 0) continue;
                    const u16 t = y.L.unit_text(u);
                    if (!t.empty() && !inv.count(t)) inv[t] = u;
                }
            bool cover = true;
            for (size_t i = 0; i < spell.size() && cover;) {
                size_t len = 0;
                for (size_t n = std::min<size_t>(3, spell.size() - i); n >= 1 && !len; n--) {
                    auto it = inv.find(spell.substr(i, n));
                    if (it != inv.end()) { out.push_back(it->second); len = n; }
                }
                if (!len) {
                    const char16_t c = spell[i];
                    const bool passthru = y.qwerty || (y.L.kind() == Layout::Kind::Romaji && (!y.L.has_key_map() || !inv.count(u16(1, c))));
                    if (passthru && c < 0x80 && !(y.L.kind() == Layout::Kind::Romaji && y.L.has_key_map() && y.L.handles(std::string(1, char(c))))) {
                        out.push_back(std::string(1, char(c)));
                        len = 1;
                    } else cover = false;
                }
                i += len;
            }
            ok = cover && decode(y, out) == kana;
        }
        if (!ok) out.clear();
        return out;
    }
    // かなの配列: 出るかな → 単位 (単打を先に)。無いかなは 清音 + 濁点・小さい字は 大きい字 + 拗
    std::map<u16, std::string> inv;
    auto rank = [](const std::string& u) { return (u.find('+') != std::string::npos) + (u.find('>') != std::string::npos) + (u.compare(0, 2, "S-") == 0); };
    for (auto& u : y.L.all_units()) {
        const u16 t = y.L.unit_text(u);
        if (t.empty()) continue;
        auto it = inv.find(t);
        if (it == inv.end() || rank(u) < rank(it->second)) inv[t] = u;
    }
    static const u16 DAKU_FROM = u"かきくけこさしすせそたちつてとはひふへほう", DAKU_TO = u"がぎぐげござじずぜぞだぢづでどばびぶべぼゔ";
    static const u16 HAN_FROM = u"はひふへほ", HAN_TO = u"ぱぴぷぺぽ";
    static const u16 SMALL_FROM = u"あいうえおやゆよつわ", SMALL_TO = u"ぁぃぅぇぉゃゅょっゎ";
    auto mod_unit = [&](std::initializer_list<const char16_t*> marks) -> std::string {
        for (const char16_t* m : marks) {
            auto it = inv.find(m);
            if (it != inv.end()) return it->second;
        }
        return std::string();
    };
    for (size_t i = 0; i < kana.size();) {
        size_t len = 0;
        for (size_t n = std::min<size_t>(3, kana.size() - i); n >= 1 && !len; n--) {
            auto it = inv.find(kana.substr(i, n));
            if (it != inv.end()) { out.push_back(it->second); len = n; }
        }
        if (!len) {
            const char16_t c = kana[i];
            size_t at;
            std::string base, mark;
            if ((at = DAKU_TO.find(c)) != u16::npos) { auto it = inv.find(u16(1, DAKU_FROM[at])); if (it != inv.end()) base = it->second; mark = mod_unit({u"゛", u""}); }
            else if ((at = HAN_TO.find(c)) != u16::npos) { auto it = inv.find(u16(1, HAN_FROM[at])); if (it != inv.end()) base = it->second; mark = mod_unit({u"゜"}); }
            else if ((at = SMALL_TO.find(c)) != u16::npos) { auto it = inv.find(u16(1, SMALL_FROM[at])); if (it != inv.end()) base = it->second; mark = mod_unit({u""}); }
            if (base.empty() || mark.empty()) return {};
            out.push_back(base);
            out.push_back(mark);
            len = 1;
        }
        i += len;
    }
    if (decode(y, out) != kana) out.clear();
    return out;
}

// QWERTY のローマ字の隣 (配列なしの比べる用)
std::vector<std::string> qwerty_neighbors(const std::string& k) {
    static const char* const ROWS[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    std::vector<std::string> out;
    for (int r = 0; r < 3; r++) {
        const std::string row = ROWS[r];
        const size_t j = row.find(k[0]);
        if (j == std::string::npos) continue;
        auto put = [&](int rr, long c) { const std::string o = ROWS[rr]; if (c >= 0 && size_t(c) < o.size()) out.push_back(std::string(1, o[size_t(c)])); };
        put(r, long(j) - 1); put(r, long(j) + 1);
        if (r > 0) { put(r - 1, long(j)); put(r - 1, long(j) + 1); }
        if (r < 2) { put(r + 1, long(j) - 1); put(r + 1, long(j)); }
    }
    return out;
}

std::vector<std::string> alts_of(Lay& y, const std::string& u) {
    if (y.qwerty) return qwerty_neighbors(u);
    return y.L.unit_alts(u);
}

// 打ち間違いを 1 つ入れる (op: 0 = 隣のキー・別の面、1 = 抜け、2 = 余分な 1 打 (隣のキー)、3 = 入れ替え)
bool make_typo(Lay& y, const std::vector<std::string>& u, std::mt19937& rng, std::vector<std::string>& t, int& op) {
    const size_t n = u.size();
    std::uniform_int_distribution<int> pick_op(0, 9);
    const int r = pick_op(rng);
    op = r < 4 ? 0 : r < 6 ? 1 : r < 8 ? 2 : 3;
    std::uniform_int_distribution<size_t> pos(0, n - 1);
    const size_t i = pos(rng);
    t = u;
    if (op == 0) {
        auto a = alts_of(y, u[i]);
        if (a.empty()) return false;
        t[i] = a[std::uniform_int_distribution<size_t>(0, a.size() - 1)(rng)];
    } else if (op == 1) {
        if (n < 2) return false;
        t.erase(t.begin() + long(i));
    } else if (op == 2) {   // 隣のキーを一緒に押した: その単位の隣を前か後ろに
        auto a = alts_of(y, u[i]);
        if (a.empty()) return false;
        const std::string extra = a[std::uniform_int_distribution<size_t>(0, a.size() - 1)(rng)];
        t.insert(t.begin() + long(i + (rng() & 1)), extra);
    } else {
        if (n < 2 || i + 1 >= n || u[i] == u[i + 1]) return false;
        std::swap(t[i], t[i + 1]);
    }
    return true;
}

// 打ち間違えた単位の並びから、読みの打ち直し (Composer::prime_alts と同じ決まり)
std::vector<u16> alt_readings(Lay& y, const std::vector<std::string>& t, const u16& r) {
    std::vector<u16> out;
    std::set<u16> seen;
    std::vector<std::vector<std::string>> vs;
    if (y.qwerty) {   // QWERTY: 単位ごとの言い換えを自分で (隣・抜け・入れ替え・母音などの足し)
        const size_t n = t.size();
        for (size_t i = 0; i < n; i++) {
            auto v = t; v.erase(v.begin() + long(i)); vs.push_back(v);
            for (auto& a : qwerty_neighbors(t[i])) { auto w = t; w[i] = a; vs.push_back(w); }
            if (i + 1 < n && t[i] != t[i + 1]) { auto w = t; std::swap(w[i], w[i + 1]); vs.push_back(w); }
        }
        for (size_t i = 0; i <= n; i++) {
            for (const char* c = "aiueonxtsrkhy"; *c; c++) { auto w = t; w.insert(w.begin() + long(i), std::string(1, *c)); vs.push_back(w); }
            if (i > 0) { auto w = t; w.insert(w.begin() + long(i), t[i - 1]); vs.push_back(w); }
        }
    }
    std::vector<std::vector<u16>> texts = y.qwerty ? std::vector<std::vector<u16>>() : y.L.typo_variants(t);
    auto consider = [&](const u16& d) {
        const size_t n = r.size(), m = d.size();
        size_t p = 0, q = 0;
        while (p < n && p < m && d[p] == r[p]) p++;
        while (q < n - p && q < m - p && d[m - 1 - q] == r[n - 1 - q]) q++;
        if (p == n && m == n) return;
        if (!kana_only(d.substr(p, m - q - p)) && m - q - p > 0) return;
        if (d != r && seen.insert(d).second) out.push_back(d);
    };
    for (auto& v : vs) consider(decode(y, v));
    for (auto& tx : texts) {   // 出す字の並び → 読み
        u16 kana;
        if (y.letters) {
            y.rom.reset();
            for (auto& s : tx)
                for (char16_t key : s) {
                    const char16_t lk = key >= u'A' && key <= u'Z' ? char16_t(key + 32) : key;
                    if ((lk >= u'a' && lk <= u'z') || lk == u'-' || lk == u'\'' || y.rom.is_prefix(u16(1, lk))) kana += y.rom.process(lk).committed;
                    else { kana += y.rom.flush(); kana += key; }
                }
            kana += y.rom.flush();
        } else {
            for (auto& s : tx)
                for (char16_t c : s) {
                    const bool mod = c == u'' || c == u'' || c == u'゛' || c == u'゜';
                    if (mod && !kana.empty() && kana_modify(kana.back(), c)) continue;
                    if (c == u'' || c == u'') continue;
                    kana.push_back(c);
                }
        }
        consider(kana);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: layout_typo_bench lex model layouts_dir cases.tsv names|all [n] [seed]\n"); return 2; }
    const int N = argc > 6 ? atoi(argv[6]) : 150;
    const unsigned seed = argc > 7 ? unsigned(atoi(argv[7])) : 1;
    const std::string lex = slurp(argv[1]), model = slurp(argv[2]);
    Converter conv;
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) { fprintf(stderr, "engine open failed\n"); return 2; }
    struct Case { u16 ctx, kana; std::vector<u16> gold; };
    std::vector<Case> cases;
    {
        const std::string t = slurp(argv[4]);
        size_t i = 0;
        while (i < t.size()) {
            size_t j = t.find('\n', i);
            if (j == std::string::npos) j = t.size();
            const std::string line = t.substr(i, j - i);
            i = j + 1;
            const size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
            Case cs{from_utf8(line.substr(a + 1, b - a - 1)), from_utf8(line.substr(b + 1, c - b - 1)), {}};
            const std::string g = line.substr(c + 1);
            for (size_t x = 0; x <= g.size();) {
                size_t y2 = g.find('|', x);
                if (y2 == std::string::npos) y2 = g.size();
                cs.gold.push_back(from_utf8(g.substr(x, y2 - x)));
                x = y2 + 1;
            }
            cases.push_back(std::move(cs));
        }
    }
    std::vector<std::string> names;
    {
        const std::string s = argv[5];
        if (s == "all") {
            names.push_back("qwerty");
            for (auto& de : fs::directory_iterator(argv[3]))
                if (de.path().extension() == ".tsv") names.push_back(de.path().stem().string());
        } else {
            size_t i = 0;
            while (i <= s.size()) {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                names.push_back(s.substr(i, j - i));
                i = j + 1;
            }
        }
    }
    printf("配列\t件数\t読めず\toff\ton\ton(提案込み)\t1位の変化\t正解(打ち間違いなし) 前→後\tms/回\n");
    for (auto& name : names) {
        Lay y;
        y.name = name;
        if (name == "qwerty_self") y.qwerty = true;   // ベンチの中で作る QWERTY の打ち直し (前の版の比べる用)
        else if (name == "qwerty") y.L = qwerty_layout();   // アプリと同じ (typo_alts.h)
        else if (!y.L.load(slurp(fs::path(argv[3]) / (name + ".tsv")))) { printf("%s\tload failed\n", name.c_str()); continue; }
        if (!y.qwerty) {
            y.letters = y.L.kind() == Layout::Kind::Letters || y.L.kind() == Layout::Kind::Romaji;
            if (y.L.kind() == Layout::Kind::Romaji && name != "qwerty") {
                y.rom.set_extended(true);
                if (y.L.romaji_base_empty()) y.rom.clear_table();
                for (auto& [k, v] : y.L.romaji_pairs()) y.rom.add_pair(k, v);
            }
        }
        std::mt19937 rng(seed);
        int n = 0, unenc = 0, fix_off = 0, fix_on = 0, fix_on_s = 0, false_chg = 0, gold_base = 0, gold_clean = 0;
        double ms_alt = 0;
        int op_n[4] = {}, op_on[4] = {}, op_off[4] = {};
        for (size_t ci = 0; ci < cases.size() && n < N; ci++) {
            const Case& c = cases[(ci * 7919 + seed) % cases.size()];
            const auto units = encode(y, c.kana);
            if (units.empty()) { unenc++; continue; }
            std::vector<std::string> t;
            u16 r;
            int op = 0;
            bool made = false;
            for (int tries = 0; tries < 8 && !made; tries++) {
                if (!make_typo(y, units, rng, t, op)) continue;
                r = decode(y, t);
                made = r != c.kana && kana_only(r);
            }
            if (!made) continue;
            n++;
            op_n[op]++;
            const u16 base = conv.convert(c.ctx, c.kana, 10).cands.at(0);
            const Conversion off = conv.convert(c.ctx, r, 10);
            const bool ok_off = !off.cands.empty() && off.cands[0] == base;
            auto t0 = std::chrono::steady_clock::now();
            const std::vector<u16> alts = alt_readings(y, t, r);
            ms_alt += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            conv.set_alt_readings(alts, r.size());
            const Conversion on = conv.convert(c.ctx, r, 10);
            const bool ok_on = !on.cands.empty() && on.cands[0] == base;
            const bool ok_on_s = ok_on || std::find(on.sugg.begin(), on.sugg.end(), base) != on.sugg.end();
            fix_off += ok_off;
            fix_on += ok_on;
            fix_on_s += ok_on_s;
            op_off[op] += ok_off;
            op_on[op] += ok_on;
            // 打ち間違いの無い読みに打ち直しを渡しても 1 位が変わらないか
            conv.set_alt_readings(alt_readings(y, units, c.kana), c.kana.size());
            const Conversion clean = conv.convert(c.ctx, c.kana, 10);
            auto is_gold = [&](const u16& x) { return std::find(c.gold.begin(), c.gold.end(), x) != c.gold.end(); };
            gold_base += is_gold(base);
            gold_clean += !clean.cands.empty() && is_gold(clean.cands[0]);
            if (clean.cands.empty() || clean.cands[0] != base) {
                false_chg++;
                if (getenv("LT_SHOW")) fprintf(stderr, "誤爆 %s: %s → %s\n", name.c_str(), to_utf8(base).c_str(), clean.cands.empty() ? "" : to_utf8(clean.cands[0]).c_str());
            }
        }
        auto pct = [&](int a, int b) { return b ? 100.0 * a / b : 0.0; };
        printf("%s\t%d\t%d\t%.1f\t%.1f\t%.1f\t%d\t%d→%d\t%.2f\t[隣・面 %d: %.0f→%.0f / 抜け %d: %.0f→%.0f / 余分 %d: %.0f→%.0f / 入れ替え %d: %.0f→%.0f]\n",
               name.c_str(), n, unenc, pct(fix_off, n), pct(fix_on, n), pct(fix_on_s, n), false_chg, gold_base, gold_clean, n ? ms_alt / n : 0.0,
               op_n[0], pct(op_off[0], op_n[0]), pct(op_on[0], op_n[0]), op_n[1], pct(op_off[1], op_n[1]), pct(op_on[1], op_n[1]),
               op_n[2], pct(op_off[2], op_n[2]), pct(op_on[2], op_n[2]), op_n[3], pct(op_off[3], op_n[3]), pct(op_on[3], op_n[3]));
        fflush(stdout);
    }
    return 0;
}

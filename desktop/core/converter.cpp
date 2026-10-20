#include <cstdlib>
#include "converter.h"

#include <cmath>

#include <algorithm>

#include "engine.h"
#include "pos_classes.h"
#include "tables.h"

namespace shunti {

namespace {
const uint16_t* p16(const u16& s) { return reinterpret_cast<const uint16_t*>(s.data()); }

char pos_class(int id) { return id >= 0 && id < POS_CLASS_COUNT ? POS_CLASSES[id] : '0'; }

bool is_punct(const u16& s) {
    return !s.empty() && s.find_first_not_of(u"、。，．！？!?…‥・,.") == u16::npos;
}

// 長すぎる読みを分ける位置 (真ん中より前の最後の句読点の後ろ。無ければ真ん中)
size_t split_point(const u16& kana) {
    size_t mid = std::min<size_t>(kana.size() / 2, 120);
    for (size_t i = mid; i > mid / 2; i--)
        if (u16(u"、。！？!?").find(kana[i - 1]) != u16::npos) return i;
    return mid;
}
}  // namespace

// 不適切な語も候補に出す (ユーザー 2026-10-13「不適切語は常時出していい」。スマホ版の設定の既定と同じ)
bool allow_slurs = true;

bool is_slur(const u16& s) {
    for (const auto& r : SLURS) {
        u16 w = r.word;
        for (size_t i = s.find(w); i != u16::npos; i = s.find(w, i + 1)) {
            bool excused = false;
            for (int k = 0; r.not_after[k]; k++)
                if (s.compare(i + w.size(), u16(r.not_after[k]).size(), r.not_after[k]) == 0) excused = true;
            if (!excused) return true;
        }
    }
    return false;
}

Converter::~Converter() {
    if (e_) kkc_close(e_);
}

bool Converter::open(const void* lex, size_t lex_size, const void* model, size_t model_size, int threads) {
    if (e_) kkc_close(e_);
    e_ = kkc_open(lex, lex_size, model, model_size);
    if (e_ && threads > 0) kkc_set_threads(e_, threads);
    if (e_) kkc_set_suggest(e_, suggest_ ? 1 : 0);
    if (e_ && !user_.empty()) set_user_words(std::vector<UserDict::Form>(user_), user_bonus_);   // モデルを替えて開き直したとき
    return e_ != nullptr;
}

bool Converter::convert_once(const u16& ctx, const u16& kana, int max, Conversion& out) {
    out = Conversion();
    int want = max * 2;
    std::vector<uint16_t> buf(size_t(want) * (kana.size() * 4 + 16) + 64);
    // 変換の方式 (engine.h の use_model。1 = 毎回モデル、2 = 段階式)。計測用に SHUNTI_KKC_MODE で替えられる
    // (atoi は使わない: glibc 2.38 の __isoc23_strtol を要求して、Linux 版の動く範囲が狭まる)
    static const int mode = [] { const char* m = getenv("SHUNTI_KKC_MODE"); return m && *m >= '0' && *m <= '9' ? *m - '0' : 1; }();
    int n = kkc_convert(e_, p16(ctx), int(ctx.size()), p16(kana), int(kana.size()), want, mode, buf.data(), int(buf.size()));
    if (n == -3) return false;
    if (n <= 0) return true;
    std::vector<u16> raw;
    for (size_t i = 0, st = 0; int(raw.size()) < n && i < buf.size(); i++) {
        if (buf[i] == 0) { raw.emplace_back(reinterpret_cast<const char16_t*>(buf.data() + st), i - st); st = i + 1; }
    }
    for (auto& c : raw) {
        if (int(out.cands.size()) >= max) break;
        if (allow_slurs || !is_slur(c)) out.cands.push_back(c);
    }
    {   // 提案
        std::vector<uint16_t> sb(8192);
        int32_t kinds[32];
        int ns = kkc_last_suggest(e_, sb.data(), int(sb.size()), kinds, 32);
        for (size_t i = 0, st = 0, k = 0; ns > 0 && int(k) < ns && i < sb.size(); i++) {
            if (sb[i] != 0) continue;
            u16 t(reinterpret_cast<const char16_t*>(sb.data() + st), i - st);
            if (allow_slurs || !is_slur(t)) { out.sugg.push_back(std::move(t)); out.sugg_kinds.push_back(kinds[k]); }
            st = i + 1;
            k++;
        }
    }
    // 。。。・・・ (ピリオドや中黒を 3 つ打った) は三点リーダー … と半角の ... にした形も 1 位のすぐ後ろに (ユーザー 2026-10-13)
    if (!out.cands.empty()) {
        u16 t = out.cands[0];
        bool hit = false;
        for (const u16& dots : {u16(u"。。。"), u16(u"・・・"), u16(u"．．．")}) {
            for (size_t at = t.find(dots); at != u16::npos; at = t.find(dots, at + 1)) {
                t.replace(at, 3, u"…");
                hit = true;
            }
        }
        if (hit && std::find(out.cands.begin(), out.cands.end(), t) == out.cands.end()) {
            out.cands.insert(out.cands.begin() + 1, t);
            // 半角の ... の形も (ユーザー 2026-10-13)
            u16 a = t;
            for (size_t at = a.find(u"…"); at != u16::npos; at = a.find(u"…", at + 3)) a.replace(at, 1, u"...");
            if (std::find(out.cands.begin(), out.cands.end(), a) == out.cands.end()) out.cands.insert(out.cands.begin() + 2, a);
        }
    }
    // 差別語で 1 位が落ちたときは区切りを使わない (区切りは元の 1 位のもの)
    if (raw.empty() || out.cands.empty() || out.cands[0] != raw[0]) return true;
    std::vector<int32_t> ends(kana.size() + 1), lens(kana.size() + 1), lids(kana.size() + 1), rids(kana.size() + 1);
    int m = kkc_last_segments(e_, ends.data(), lens.data(), int(ends.size()));
    if (m <= 0 || kkc_last_segment_pos(e_, lids.data(), rids.data(), int(lids.size())) != m) return true;
    const u16& top = raw[0];
    size_t r0 = 0, s0 = 0;
    for (int i = 0; i < m; i++) {
        size_t r1 = size_t(ends[i]), s1 = s0 + size_t(lens[i]);
        if (r1 < r0 || r1 > kana.size() || s1 > top.size()) { out.words.clear(); return true; }
        Word w{kana.substr(r0, r1 - r0), top.substr(s0, s1 - s0), lids[i], rids[i], false};
        w.pass = !w.reading.empty() && w.reading == w.surface && !is_reading_char(w.reading[0]);
        out.words.push_back(std::move(w));
        r0 = r1;
        s0 = s1;
    }
    if (r0 != kana.size() || s0 != top.size()) { out.words.clear(); return true; }
    {   // 候補の木
        constexpr int PM = 40, WC = 2048, TC = 8192;
        std::vector<double> costs(PM);
        std::vector<int32_t> cnt(PM), pe(WC), pl(WC);
        std::vector<uint16_t> tx(TC);
        int np = kkc_last_paths(e_, PM, costs.data(), cnt.data(), pe.data(), pl.data(), WC, tx.data(), TC);
        std::vector<std::pair<double, std::vector<TreeWord>>> paths;
        size_t wi = 0, ti = 0;
        for (int p = 0; p < np; p++) {
            std::vector<TreeWord> ws;
            int r = 0;
            for (int k = 0; k < cnt[size_t(p)]; k++, wi++) {
                TreeWord w;
                w.surface.assign(reinterpret_cast<const char16_t*>(tx.data() + ti), size_t(pl[wi]));
                ti += size_t(pl[wi]);
                w.rs = r;
                w.re = r = pe[wi];
                ws.push_back(std::move(w));
            }
            paths.push_back({costs[size_t(p)], std::move(ws)});
        }
        // 1 位の経路が候補の 1 位と同じときだけ (差別語で落ちたときなどは出さない)
        if (!paths.empty()) {
            u16 j;
            for (auto& w : paths[0].second) j += w.surface;
            if (j == top) {
                std::vector<int> cuts;
                int r = 0;
                for (auto& ph : group_phrases(out.words)) cuts.push_back(r += int(ph.reading.size()));
                out.tree = build_tree(paths, cuts);
            }
        }
    }
    return true;
}

// 経路のコストの差を確率に直す温度 (コストは辞書のコストと同じ単位。1000 ほどの差で約 e 倍)
constexpr double TREE_TEMP = 2500.0;

std::vector<TreeNode> build_tree(const std::vector<std::pair<double, std::vector<TreeWord>>>& raw, const std::vector<int>& cuts) {
    std::vector<TreeNode> out;
    if (raw.empty() || raw[0].second.empty()) return out;
    // 語を文節にまとめる: 経路ごとに、その経路の区切りのうち 1 位の文節の終わりと合う所だけで切る
    // (1 位の文節の終わりをまたぐ語がある所は、その経路だけ文節が長くなる = 区切り方で迷う所)
    std::vector<std::pair<double, std::vector<TreeWord>>> paths;
    for (auto& [c, ws] : raw) {
        std::vector<TreeWord> ch;
        TreeWord cur;
        bool open = false;
        for (auto& x : ws) {
            if (!open) { cur = TreeWord(); cur.rs = x.rs; open = true; }
            cur.surface += x.surface;
            cur.re = x.re;
            if (std::find(cuts.begin(), cuts.end(), x.re) != cuts.end()) { ch.push_back(cur); open = false; }
        }
        if (open) ch.push_back(cur);
        paths.push_back({c, std::move(ch)});
    }
    // 文節にまとめると同じになる経路は 1 つに (確率は後で足す)
    const size_t P = paths.size();
    std::vector<double> w(P);
    double sum = 0;
    for (size_t i = 0; i < P; i++) sum += w[i] = std::exp(-(paths[i].first - paths[0].first) / TREE_TEMP);
    for (auto& x : w) x /= sum;
    auto has = [&](size_t p, const TreeWord& t) {
        for (auto& x : paths[p].second)
            if (x.rs == t.rs && x.re == t.re && x.surface == t.surface) return true;
        return false;
    };
    auto bounds = [&](size_t p) {
        std::vector<int> b;
        for (auto& x : paths[p].second) b.push_back(x.re);
        return b;
    };
    const std::vector<TreeWord>& top = paths[0].second;
    const std::vector<int> b0 = bounds(0);
    // 1 位の語ごとの強さ・別の表記・前の語との結びつき
    std::vector<TreeWord> words = top;
    for (size_t k = 0; k < words.size(); k++) {
        TreeWord& t = words[k];
        double s = 0, both = 0, prev = 0;
        std::vector<std::pair<u16, double>> alt;
        for (size_t p = 0; p < P; p++) {
            const bool h = has(p, t);
            if (h) s += w[p];
            if (k > 0 && has(p, top[k - 1])) { prev += w[p]; if (h) both += w[p]; }
            for (auto& x : paths[p].second) {
                if (x.rs != t.rs || x.re != t.re || x.surface == t.surface) continue;
                auto it = std::find_if(alt.begin(), alt.end(), [&](auto& a) { return a.first == x.surface; });
                if (it == alt.end()) alt.push_back({x.surface, w[p]});
                else it->second += w[p];
            }
        }
        t.s = float(s);
        t.link = k == 0 ? 1.f : float(prev > 0 ? both / prev : s);
        std::stable_sort(alt.begin(), alt.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < alt.size() && i < 3; i++) t.alts.push_back({alt[i].first, float(alt[i].second)});
    }
    // 区切り方で迷う所: 1 位と区切りが違う経路のうち確率の高いものから、違う区間 (両方の区切りが合う所で挟む) を取る
    struct Fork { int a, b; size_t p; double mass, best; };   // best = 出す別の道 (いちばん強い経路) の確率
    std::vector<Fork> forks;
    auto in = [](const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    for (size_t p = 1; p < P; p++) {
        const std::vector<int> bp = bounds(p);
        // 違う区切りの位置を 1 位の区切りの区間に広げる
        int a = -1, b = -1;
        for (int x : bp) if (!in(b0, x)) { if (a < 0 || x < a) a = x; if (x > b) b = x; }
        for (int x : b0) if (!in(bp, x) && x != b0.back()) { if (a < 0 || x < a) a = x; if (x > b) b = x; }
        if (a < 0) continue;
        int lo = 0, hi = b0.back();
        for (int x : b0) { if (x < a && in(bp, x)) lo = std::max(lo, x); if (x > b && in(bp, x)) hi = std::min(hi, x); }
        // ほかの迷う所と重なるなら足さない
        bool clash = false;
        for (auto& f : forks)
            if (!(hi <= f.a || lo >= f.b)) { if (lo == f.a && hi == f.b) f.mass += w[p]; clash = true; break; }
        if (!clash) forks.push_back({lo, hi, p, w[p], w[p]});
    }
    // 迷う所ごとに節を作る
    size_t k = 0;
    while (k < words.size()) {
        const Fork* f = nullptr;
        for (auto& x : forks)
            if (x.a == words[k].rs && x.mass >= 0.08 && x.best >= 0.06) { f = &x; break; }
        TreeNode n;
        if (!f) { n.main.push_back(words[k++]); out.push_back(std::move(n)); continue; }
        while (k < words.size() && words[k].re <= f->b) n.main.push_back(words[k++]);
        for (auto x : paths[f->p].second)
            if (x.rs >= f->a && x.re <= f->b) {
                double s = 0;
                for (size_t p = 0; p < P; p++) if (has(p, x)) s += w[p];
                x.s = float(s);
                n.fork.push_back(std::move(x));
            }
        n.p = float(std::max(0.0, 1.0 - f->mass));
        out.push_back(std::move(n));
    }
    return out;
}

Conversion Converter::convert(const u16& ctx, const u16& kana, int max) {
    Conversion out;
    if (!e_ || kana.empty()) return out;
    if (convert_once(ctx, kana, max, out)) return out;
    // 長すぎる: 前と後ろに分けて変換し、1 位どうしをつなぐ (後ろは前の 1 位を文脈にする)
    size_t sp = split_point(kana);
    Conversion a = convert(ctx, kana.substr(0, sp), 1);
    u16 sa = a.cands.empty() ? kana.substr(0, sp) : a.cands[0];
    Conversion b = convert(ctx + sa, kana.substr(sp), 1);
    u16 sb = b.cands.empty() ? kana.substr(sp) : b.cands[0];
    out.cands.push_back(sa + sb);
    if (a.words.empty()) a.words.push_back({kana.substr(0, sp), sa});
    if (b.words.empty()) b.words.push_back({kana.substr(sp), sb});
    out.words = a.words;
    out.words.insert(out.words.end(), b.words.begin(), b.words.end());
    return out;
}

void Converter::set_suggest(bool on) {
    suggest_ = on;
    if (e_) kkc_set_suggest(e_, on ? 1 : 0);
}

std::vector<u16> Converter::complete(const u16& prefix, int max, int max_extra) {
    std::vector<u16> out;
    if (!e_ || prefix.empty()) return out;
    std::vector<uint16_t> buf(size_t(max) * 64 + 64);
    int n = kkc_complete(e_, p16(prefix), int(prefix.size()), max_extra, max, buf.data(), int(buf.size()));
    for (size_t i = 0, st = 0; n > 0 && int(out.size()) < n && i < buf.size(); i++) {
        if (buf[i] == 0) { out.emplace_back(reinterpret_cast<const char16_t*>(buf.data() + st), i - st); st = i + 1; }
    }
    if (!allow_slurs) out.erase(std::remove_if(out.begin(), out.end(), is_slur), out.end());
    return out;
}

int Converter::set_user_words(const std::vector<UserDict::Form>& forms, int bonus) {
    user_ = forms;
    user_bonus_ = bonus;
    if (!e_) return 0;
    kkc_user_clear(e_);
    int n = 0;
    for (auto& f : forms)
        n += kkc_user_add_like(e_, p16(f.reading), int(f.reading.size()), p16(f.surface), int(f.surface.size()),
                               p16(f.tmpl_reading), int(f.tmpl_reading.size()), p16(f.tmpl_surface), int(f.tmpl_surface.size()), bonus);
    return n;
}

std::vector<Phrase> group_phrases(const std::vector<Word>& words) {
    std::vector<Phrase> out;
    bool in_tail = false;   // いまの文節の付属語の部分に入った
    for (size_t i = 0; i < words.size(); i++) {
        const Word& w = words[i];
        bool attach = false, head = false;
        if (i > 0) {
            const Word& p = words[i - 1];
            attach = pos_class(w.lid) == '1' || is_punct(w.surface) || (pos_class(w.lid) == '4' && pos_class(p.rid) == '3');
            head = !attach && (pos_class(p.rid) == '2' || (p.pass && w.pass)) && !in_tail;
            // 辞書に無い語 (かなのまま・カタカナ) は名詞の ID なので自立語になる
        }
        if ((attach || head) && !out.empty()) {
            Phrase& ph = out.back();
            ph.reading += w.reading;
            ph.surface += w.surface;
            if (head) { ph.head_r = ph.reading.size(); ph.head_s = ph.surface.size(); }
            else in_tail = true;
        } else {
            out.push_back({w.reading, w.surface, w.reading.size(), w.surface.size()});
            in_tail = false;
        }
    }
    return out;
}

}  // namespace shunti

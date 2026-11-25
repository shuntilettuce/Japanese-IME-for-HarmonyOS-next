#include "glide.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace glide {

namespace {

// かな (1 字か 2 字) -> 綴り (同じ字が続くのは 1 つにまとめた後のキーの並び)
struct Spell {
    std::map<std::u16string, std::vector<std::string>> m;
    static std::string collapse(const std::string& s) {
        std::string o;
        for (char c : s) if (o.empty() || o.back() != c) o += c;
        return o;
    }
    void add(const char16_t* k, std::initializer_list<const char*> rs) {
        auto& v = m[k];
        for (const char* r : rs) {
            std::string c = collapse(r);
            if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);
        }
    }
    Spell() {
        const char* V = "aiueo";
        const std::pair<const char16_t*, const char*> rows[] = {
            {u"あいうえお", ""}, {u"かきくけこ", "k"}, {u"さしすせそ", "s"}, {u"たちつてと", "t"}, {u"なにぬねの", "n"},
            {u"はひふへほ", "h"}, {u"まみむめも", "m"}, {u"らりるれろ", "r"}, {u"がぎぐげご", "g"}, {u"ざじずぜぞ", "z"},
            {u"だぢづでど", "d"}, {u"ばびぶべぼ", "b"}, {u"ぱぴぷぺぽ", "p"}, {u"ぁぃぅぇぉ", "x"}};
        for (auto& rw : rows)
            for (int i = 0; i < 5; i++) {
                const char16_t k[2] = {rw.first[i], 0};
                add(k, {(std::string(rw.second) + V[i]).c_str()});
            }
        add(u"し", {"shi"}); add(u"ち", {"chi"}); add(u"つ", {"tsu"}); add(u"ふ", {"fu"}); add(u"じ", {"ji"});
        add(u"ぁ", {"la"}); add(u"ぃ", {"li"}); add(u"ぅ", {"lu"}); add(u"ぇ", {"le"}); add(u"ぉ", {"lo"});
        add(u"や", {"ya"}); add(u"ゆ", {"yu"}); add(u"よ", {"yo"}); add(u"わ", {"wa"}); add(u"を", {"wo"});
        add(u"ん", {"n"}); add(u"ー", {"-"}); add(u"ゔ", {"vu"}); add(u"っ", {""});
        add(u"ゃ", {"xya"}); add(u"ゅ", {"xyu"}); add(u"ょ", {"xyo"});
        const std::pair<char16_t, const char*> yoon[] = {
            {u'き', "ky"}, {u'し', "sy"}, {u'ち', "ty"}, {u'に', "ny"}, {u'ひ', "hy"}, {u'み', "my"}, {u'り', "ry"}, {u'ぎ', "gy"},
            {u'じ', "zy"}, {u'び', "by"}, {u'ぴ', "py"}, {u'ぢ', "dy"}};
        const char16_t sm[3] = {u'ゃ', u'ゅ', u'ょ'};
        const char* yv = "auo";
        for (auto& y : yoon)
            for (int i = 0; i < 3; i++) {
                const char16_t k[3] = {y.first, sm[i], 0};
                add(k, {(std::string(y.second) + yv[i]).c_str()});
            }
        add(u"しゃ", {"sha"}); add(u"しゅ", {"shu"}); add(u"しょ", {"sho"});
        add(u"ちゃ", {"cha", "cya"}); add(u"ちゅ", {"chu", "cyu"}); add(u"ちょ", {"cho", "cyo"});
        add(u"じゃ", {"ja", "jya"}); add(u"じゅ", {"ju", "jyu"}); add(u"じょ", {"jo", "jyo"});
        add(u"しぇ", {"she", "sye"}); add(u"ちぇ", {"che", "tye"}); add(u"じぇ", {"je", "zye"});
        add(u"ふぁ", {"fa"}); add(u"ふぃ", {"fi"}); add(u"ふぇ", {"fe"}); add(u"ふぉ", {"fo"});
        add(u"てぃ", {"thi"}); add(u"でぃ", {"dhi"}); add(u"うぃ", {"wi"}); add(u"うぇ", {"we"});
        add(u"ゔぁ", {"va"}); add(u"ゔぃ", {"vi"}); add(u"ゔぇ", {"ve"}); add(u"ゔぉ", {"vo"});
    }
};
const Spell& spell() { static Spell s; return s; }

constexpr float INF = 1e30f;

}  // namespace

void Decoder::build(const std::vector<std::pair<std::u16string, int32_t>>& words0, Mode mode) {
    mode_ = mode;
    std::vector<std::pair<std::u16string, int32_t>> words = words0;
    std::sort(words.begin(), words.end());
    node_.assign(1, Node{});
    std::vector<int32_t> lastc(1, -1), path(1, 0);
    std::u16string prev;
    for (auto& w : words) {
        const std::u16string& s = w.first;
        size_t l = 0;
        while (l < s.size() && l < prev.size() && s[l] == prev[l]) l++;
        path.resize(l + 1);
        for (size_t i = l; i < s.size(); i++) {
            const int32_t par = path.back(), id = int32_t(node_.size());
            node_.push_back(Node{s[i]});
            lastc.push_back(-1);
            if (lastc[size_t(par)] < 0) node_[size_t(par)].first = id; else node_[size_t(lastc[size_t(par)])].next = id;
            lastc[size_t(par)] = id;
            path.push_back(id);
        }
        Node& n = node_[size_t(path.back())];
        n.cost = int16_t(std::min<int32_t>(n.cost, std::max<int32_t>(0, std::min<int32_t>(w.second, NOC - 1))));
        prev = s;
    }
    // 子は親より後ろに作ったので、後ろから親へ部分木の最小コストを送る
    std::vector<int32_t> par(node_.size(), -1);
    for (size_t i = 0; i < node_.size(); i++)
        for (int c = node_[i].first; c >= 0; c = node_[size_t(c)].next) par[size_t(c)] = int32_t(i);
    for (size_t i = node_.size(); i-- > 0;) {
        node_[i].minsub = std::min(node_[i].minsub, node_[i].cost);
        if (par[i] >= 0) node_[size_t(par[i])].minsub = std::min(node_[size_t(par[i])].minsub, node_[i].minsub);
    }
}

namespace {
inline int slot(char16_t c) { return (c >= 0x3040 && c < 0x3100) ? c - 0x3040 : (c < 128 ? 192 + c : -1); }
}

void Decoder::set_keys(const std::vector<Key>& keys) {
    keys_ = keys;
    for (int& k : kidx_) k = -1;
    for (size_t i = 0; i < keys.size(); i++) kidx_[uint8_t(keys[i].c) & 127] = int(i);
    // 綴りをキーの番号の並びにしておく
    sp1_.assign(NSLOT, {});
    has2_.assign(NSLOT, 0);
    sp2_.clear();
    auto conv = [&](const std::string& r, std::vector<int8_t>& o) {
        o.clear();
        for (char c : r) { const int k = kidx_[uint8_t(c) & 127]; if (k < 0) return false; o.push_back(int8_t(k)); }
        return true;
    };
    std::vector<int8_t> o;
    if (mode_ == Mode::Kana) {
        for (auto& kv : spell().m) {
            const std::u16string& k = kv.first;
            for (const std::string& r : kv.second) {
                if (!conv(r, o)) continue;
                if (r[0] == 'x' || r[0] == 'l') continue;
                if (k.size() == 1) { if (slot(k[0]) >= 0) sp1_[size_t(slot(k[0]))].push_back(o); }
                else if (slot(k[0]) >= 0 && slot(k[1]) >= 0) {
                    sp2_[uint32_t(slot(k[0])) << 10 | uint32_t(slot(k[1]))].push_back(o);
                    has2_[size_t(slot(k[0]))] = 1;
                }
            }
        }
    } else {
        for (int c = 0; c < 128; c++)
            if (kidx_[c] >= 0) sp1_[size_t(192 + c)].push_back({int8_t(kidx_[c])});
    }
    vowel_.assign(keys_.size(), 0);
    for (char v : std::string("aiueon-")) if (kidx_[int(v)] >= 0) vowel_[size_t(kidx_[int(v)])] = 1;
    // 2 字の綴りの後ろの字になる子を節ごとに
    two_.clear();
    std::vector<uint8_t> second(NSLOT, 0);
    for (auto& kv : sp2_) second[kv.first & 1023] = 1;
    for (size_t i = 0; i < node_.size(); i++) {
        const int s1 = slot(node_[i].ch);
        if (s1 < 0 || !has2_[size_t(s1)]) continue;
        for (int g = node_[i].first; g >= 0; g = node_[size_t(g)].next) {
            const int s2 = slot(node_[size_t(g)].ch);
            if (s2 >= 0 && second[size_t(s2)] && sp2_.count(uint32_t(s1) << 10 | uint32_t(s2))) two_[int32_t(i)].push_back(g);
        }
    }
}

struct Search {
    static constexpr int MAXG = 4;   // 最後のキーの違う綴りのまとまりの数の上限
    const Decoder& d;
    const Params& P;
    int N = 0, K = 0;
    float wpt = 1;
    std::vector<float> px, py, tx, ty, kc, kcpre;   // tx, ty = 軌跡の向き (単位ベクトル)
    std::vector<std::vector<float>> segc;   // (a * K + b) -> a-b の線からのずれの累積 (空 = まだ)
    std::unordered_map<std::u16string, std::pair<float, float>> res;
    float best = INF;
    uint64_t expanded = 0;
    std::vector<std::vector<float>> pool;   // 深さごとの置き場: まとまり MAXG 個 + 途中の 2 個
    std::u16string rd;

    Search(const Decoder& d0, const Params& p0) : d(d0), P(p0) {}

    const float* segpre(int a, int b) {
        std::vector<float>& s = segc[size_t(a * K + b)];
        if (!s.empty()) return s.data();
        s.assign(size_t(N), 0.0f);
        const float ax = d.keys_[size_t(a)].x, ay = d.keys_[size_t(a)].y, bx = d.keys_[size_t(b)].x, by = d.keys_[size_t(b)].y;
        const float vx = bx - ax, vy = by - ay, vv = vx * vx + vy * vy;
        const float inv = wpt / (P.sigma_seg * P.sigma_seg);
        float acc = 0;
        for (int t = 0; t < N; t++) {
            float u = vv > 0 ? ((px[size_t(t)] - ax) * vx + (py[size_t(t)] - ay) * vy) / vv : 0;
            u = std::min(1.0f, std::max(0.0f, u));
            const float dx = px[size_t(t)] - (ax + u * vx), dy = py[size_t(t)] - (ay + u * vy);
            acc += (dx * dx + dy * dy) * inv;
            if (vv > 0) {
                const float cs = (tx[size_t(t)] * vx + ty[size_t(t)] * vy) / std::sqrt(vv);
                acc += P.dir_w * wpt * (1 - cs) * 0.5f;
            }
            s[size_t(t)] = acc;
        }
        return s.data();
    }

    // D (最後のキー a) にキー b を足して out に。a < 0 なら最初のキー
    void step(const float* D, int a, int b, float* out) {
        const float* kb = &kc[size_t(b) * size_t(N)];
        if (a < 0) {
            const float* pre = &kcpre[size_t(b) * size_t(N)];
            out[0] = kb[0];
            for (int t = 1; t < N; t++) out[t] = pre[t - 1] + kb[t];
            return;
        }
        if (a == b) { if (out != D) memcpy(out, D, sizeof(float) * size_t(N)); return; }
        const float* S = segpre(a, b);
        float m = INF;
        for (int t = 0; t < N; t++) {
            float v = D[t];
            if (t > 0) v = std::min(v, S[t - 1] + m);
            const float dt = D[t] - S[t];
            out[t] = v + kb[t];
            m = std::min(m, dt);
        }
    }

    // 綴りの組 sps を D から当て、最後のキーが同じものは要素ごとに小さい方にまとめて buf に置く。まとまりの数を返す
    int apply_all(const std::vector<std::vector<int8_t>>& sps, const float* D, int a, float* buf, int* lastk) {
        int ng = 0;
        float* A = buf + MAXG * N;
        float* B = A + N;
        for (const auto& sp : sps) {
            if (sp.empty() && a < 0) continue;   // 先頭の っ
            const float* cur = D;
            int l = a;
            if (sp.empty()) { memcpy(A, D, sizeof(float) * size_t(N)); cur = A; }
            for (size_t i = 0; i < sp.size(); i++) {
                float* dst = (cur == A) ? B : A;
                step(cur, l, sp[i], dst);
                cur = dst;
                l = sp[i];
            }
            int g = 0;
            while (g < ng && lastk[g] != l) g++;
            if (g == ng) {
                if (ng == MAXG) continue;
                memcpy(buf + g * N, cur, sizeof(float) * size_t(N));
                lastk[ng++] = l;
            } else {
                float* G = buf + g * N;
                for (int t = 0; t < N; t++) G[t] = std::min(G[t], cur[t]);
            }
        }
        return ng;
    }

    float bound() const { return std::min(best + P.beam, P.abs_bound); }

    void add_result(float path, float lex) {
        const float tot = path + lex;
        auto it = res.find(rd);
        if (it == res.end()) res.emplace(rd, std::make_pair(tot, path));
        else if (it->second.first > tot) it->second = {tot, path};
        best = std::min(best, tot);
    }

    void visit(int n, const float* D, int last, float lex, int words, int depth) {
        const auto& nd = d.node_[size_t(n)];
        if (nd.cost != Decoder::NOC && last >= 0) {
            const float l2 = lex + P.lex_w * float(nd.cost) + P.word_pen;
            if (D[N - 1] + l2 <= bound()) add_result(D[N - 1], l2);
            if (words + 1 < P.max_words) rec(0, D, last, l2, words + 1, depth + 1);
        }
        rec(n, D, last, lex, words, depth + 1);
    }

    void rec(int n, const float* D, int last, float lex, int words, int depth) {
        const int32_t ms = d.node_[size_t(n)].minsub;
        if (ms == Decoder::NOC) return;
        if (last >= 0) {
            float mn = INF;
            for (int t = 0; t < N; t++) mn = std::min(mn, D[t]);
            if (mn + lex + P.lex_w * float(ms) + P.word_pen > bound()) return;   // この先で語を終えるのに要る辞書のコストの下限も足す
        }
        if (rd.size() >= 24 || expanded >= P.max_expand || depth >= int(pool.size())) return;
        expanded++;
        float* buf = pool[size_t(depth)].data();
        int lastk[MAXG];
        for (int c = d.node_[size_t(n)].first; c >= 0; c = d.node_[size_t(c)].next) {
            const char16_t ch = d.node_[size_t(c)].ch;
            const int s1 = slot(ch);
            if (s1 < 0) continue;
            if (!d.sp1_[size_t(s1)].empty()) {
                const int ng = apply_all(d.sp1_[size_t(s1)], D, last, buf, lastk);
                rd.push_back(ch);
                for (int g = 0; g < ng; g++) visit(c, buf + g * N, lastk[g], lex, words, depth);
                rd.pop_back();
            }
            if (!d.has2_[size_t(s1)]) continue;
            auto tw = d.two_.find(c);
            if (tw == d.two_.end()) continue;
            for (int g2 : tw->second) {
                const int s2 = slot(d.node_[size_t(g2)].ch);
                auto it = d.sp2_.find(uint32_t(s1) << 10 | uint32_t(s2));
                if (it == d.sp2_.end()) continue;
                const int ng = apply_all(it->second, D, last, buf, lastk);
                rd.push_back(ch); rd.push_back(d.node_[size_t(g2)].ch);
                for (int g = 0; g < ng; g++) visit(g2, buf + g * N, lastk[g], lex, words, depth);
                rd.pop_back(); rd.pop_back();
            }
        }
    }
};

// 点の順に進めるビームサーチ。トークン = 「キー a から綴りの pos 字目のキーへ向かう途中」。
// 点 t ごとに、そのまま線の上を進む (線からのずれ) か、次のキーに着く (キーの中心からのずれ) かを選ぶ。
// 綴りを打ち終えたら木の節に着き、子の綴りへ向かうトークンを作る。語の終わりなら根からもう 1 語
struct Beam {
    struct Tok {
        float cost, lex;            // なぞりのコスト・辞書のコスト (語を終えた分)
        int32_t node, hist;         // 向かっている節・読みの履歴
        const std::vector<int8_t>* sp;
        int8_t pos, a, words;
        char16_t c1, c2;            // 節に着いたら読みに足す字 (c2 は 2 字の綴りのとき)
    };
    struct Hist { int32_t parent; char16_t c1, c2; };
    Search& S;
    const Decoder& d;
    const Params& P;
    std::vector<Tok> cur, nxt;
    std::vector<Hist> hist;
    std::unordered_map<uint64_t, int32_t> idx;   // 合流 (同じ状態は安い方だけ残す)
    std::unordered_map<int32_t, std::pair<float, float>> fin;   // 履歴 -> (合計, なぞり)
    uint64_t pushed = 0;
    int t = 0;

    Beam(Search& s) : S(s), d(s.d), P(s.P) {}

    float lex_lb(int32_t node) const {
        const int32_t ms = d.node_[size_t(node)].minsub;
        return ms == Decoder::NOC ? INF : P.lex_w * float(ms) + P.word_pen;
    }

    void push(const Tok& k) {
        pushed++;
        const uint64_t key = (uint64_t(uint32_t(k.node)) << 32) ^ (uint64_t(reinterpret_cast<uintptr_t>(k.sp)) * 0x9E3779B97F4A7C15ULL) ^
                             (uint64_t(uint8_t(k.pos)) << 8) ^ uint64_t(uint8_t(k.a)) ^ (uint64_t(uint8_t(k.words)) << 16);
        auto it = idx.find(key);
        if (it == idx.end()) { idx.emplace(key, int32_t(nxt.size())); nxt.push_back(k); return; }
        Tok& o = nxt[size_t(it->second)];
        if (k.cost + k.lex < o.cost + o.lex) o = k;
    }

    // 節 n にキー a で着いた (履歴 h)。子へ向かうトークンを作る。last = 最後の点なら結果に
    void arrive(int32_t n, int8_t a, float cost, float lex, int8_t words, int32_t h, int depth) {
        if (depth > 8) return;
        const auto& nd = d.node_[size_t(n)];
        if (n != 0 && nd.cost != Decoder::NOC) {
            const float l2 = lex + P.lex_w * float(nd.cost) + P.word_pen;
            if (t == S.N - 1) {
                auto it = fin.find(h);
                if (it == fin.end() || it->second.first > cost + l2) fin[h] = {cost + l2, cost};
            } else if (words + 1 < P.max_words) {
                arrive(0, a, cost, l2, int8_t(words + 1), h, depth + 1);
            }
        }
        if (t == S.N - 1) return;
        for (int c = nd.first; c >= 0; c = d.node_[size_t(c)].next) {
            const char16_t ch = d.node_[size_t(c)].ch;
            const int s1 = slot(ch);
            if (s1 < 0) continue;
            for (const auto& sp : d.sp1_[size_t(s1)]) start(c, &sp, a, cost, lex, words, h, ch, 0, depth);
            if (!d.has2_[size_t(s1)]) continue;
            auto tw = d.two_.find(c);
            if (tw == d.two_.end()) continue;
            for (int g2 : tw->second) {
                auto it = d.sp2_.find(uint32_t(s1) << 10 | uint32_t(slot(d.node_[size_t(g2)].ch)));
                if (it == d.sp2_.end()) continue;
                for (const auto& sp : it->second) start(g2, &sp, a, cost, lex, words, h, ch, d.node_[size_t(g2)].ch, depth);
            }
        }
    }

    // 綴り sp で節 n へ向かうトークン。最後のキーと同じキーで始まる字は、点を使わずに済ませる
    void start(int32_t n, const std::vector<int8_t>* sp, int8_t a, float cost, float lex, int8_t words, int32_t h, char16_t c1, char16_t c2, int depth) {
        if (hist[size_t(h)].c1 == u'っ' && hist[size_t(h)].c2 == 0 && (sp->empty() || d.vowel_[size_t((*sp)[0])])) return;   // っ の後ろは子音
        if (c1 == u'っ' && (h == 0 || hist[size_t(h)].c1 == u'ん' || hist[size_t(h)].c1 == u'っ' || hist[size_t(h)].c1 == u'ー')) return;   // ん・ー の後ろと先頭に っ は来ない
        int pos = 0;
        while (pos < int(sp->size()) && (*sp)[size_t(pos)] == a) { pos++; cost += P.same_key_pen; }   // 同じキーが続く (かあ・いい)
        if (cost + lex + lex_lb(n) > S.best + P.beam) return;
        if (pos == int(sp->size())) {   // っ・同じキーだけの綴り
            if (a < 0) return;
            hist.push_back({h, c1, c2});
            arrive(n, a, cost, lex, words, int32_t(hist.size() - 1), depth + 1);
            return;
        }
        push(Tok{cost, lex, n, h, sp, int8_t(pos), a, words, c1, c2});
    }

    void step(const Tok& k) {
        const int b = (*k.sp)[size_t(k.pos)];
        // 線の上を進む
        const float sc = k.a < 0 ? S.kc[size_t(b) * size_t(S.N) + size_t(t)] * S.wpt : seg(k.a, b);
        {
            Tok s = k; s.cost += sc;
            if (s.cost + s.lex + lex_lb(s.node) <= S.best + P.beam) push(s);
        }
        // キー b に着く
        const float c2 = k.cost + S.kc[size_t(b) * size_t(S.N) + size_t(t)];
        if (c2 + k.lex + lex_lb(k.node) > S.best + P.beam) return;
        int pos = k.pos + 1;
        float c2x = c2;
        while (pos < int(k.sp->size()) && (*k.sp)[size_t(pos)] == b) { pos++; c2x += P.same_key_pen; }
        if (pos < int(k.sp->size())) { Tok s = k; s.cost = c2x; s.pos = int8_t(pos); s.a = int8_t(b); push(s); return; }
        hist.push_back({k.hist, k.c1, k.c2});
        arrive(k.node, int8_t(b), c2x, k.lex, k.words, int32_t(hist.size() - 1), 0);
    }

    float seg(int a, int b) const {   // 点 t の a-b の線からのずれ (累積の差)
        const float* s = const_cast<Search&>(S).segpre(a, b);
        return t == 0 ? s[0] : s[t] - s[t - 1];
    }

    void run() {
        hist.push_back({-1, 0, 0});
        S.best = INF;
        // 最初のトークン: 根の子の綴りの最初のキーへ (a = -1)
        t = 0;
        nxt.clear(); idx.clear();
        arrive(0, -1, 0, 0, 0, 0, 0);
        for (t = 0; t < S.N; t++) {
            cur.swap(nxt);
            nxt.clear(); idx.clear();
            // 枝刈り: 1 位から beam 以内、多ければ上位 beam_size
            float b0 = INF;
            for (auto& k : cur) b0 = std::min(b0, k.cost + k.lex + lex_lb(k.node));
            S.best = b0;
            if (int(cur.size()) > P.beam_size) {
                std::nth_element(cur.begin(), cur.begin() + P.beam_size, cur.end(), [&](const Tok& x, const Tok& y) {
                    return x.cost + x.lex + lex_lb(x.node) < y.cost + y.lex + lex_lb(y.node); });
                cur.resize(size_t(P.beam_size));
            }
            for (const Tok& k : cur) step(k);
        }
    }

    std::u16string text(int32_t h) const {
        std::u16string r;
        for (; h > 0; h = hist[size_t(h)].parent) {
            if (hist[size_t(h)].c2) r.push_back(hist[size_t(h)].c2);
            r.push_back(hist[size_t(h)].c1);
        }
        std::reverse(r.begin(), r.end());
        return r;
    }
};

std::vector<Result> Decoder::decode(const std::vector<float>& pts, const Params& P) const {
    std::vector<Result> out;
    const int n0 = int(pts.size() / 2);
    if (n0 < 2 || keys_.empty() || node_.empty()) return out;
    Search S(*this, P);
    // 等間隔に取り直す
    std::vector<float> cum(size_t(n0), 0);
    for (int i = 1; i < n0; i++) cum[size_t(i)] = cum[size_t(i - 1)] + std::hypot(pts[size_t(2 * i)] - pts[size_t(2 * i - 2)], pts[size_t(2 * i + 1)] - pts[size_t(2 * i - 1)]);
    const float len = cum.back();
    if (len < 0.5f) return out;   // なぞりでなくタップ
    const float spacing = P.spacing;
    const int N = std::max(12, std::min(P.max_pts, int(len / spacing) + 1));
    const float wpt = (len / float(N - 1)) / 0.5f;
    S.N = N;
    S.wpt = wpt;
    S.px.resize(size_t(N)); S.py.resize(size_t(N));
    for (int t = 0, i = 0; t < N; t++) {
        const float at = len * float(t) / float(N - 1);
        while (i + 1 < n0 - 1 && cum[size_t(i + 1)] < at) i++;
        const float seg = cum[size_t(i + 1)] - cum[size_t(i)];
        const float u = seg > 0 ? std::min(1.0f, std::max(0.0f, (at - cum[size_t(i)]) / seg)) : 0;
        S.px[size_t(t)] = pts[size_t(2 * i)] + u * (pts[size_t(2 * i + 2)] - pts[size_t(2 * i)]);
        S.py[size_t(t)] = pts[size_t(2 * i + 1)] + u * (pts[size_t(2 * i + 3)] - pts[size_t(2 * i + 1)]);
    }
    S.tx.assign(size_t(N), 0.0f); S.ty.assign(size_t(N), 0.0f);
    for (int t = 0; t < N; t++) {
        const int a0 = std::max(0, t - 1), a1 = std::min(N - 1, t + 1);
        const float vx = S.px[size_t(a1)] - S.px[size_t(a0)], vy = S.py[size_t(a1)] - S.py[size_t(a0)], l = std::hypot(vx, vy);
        if (l > 0) S.tx[size_t(t)] = vx / l, S.ty[size_t(t)] = vy / l;
    }
    S.K = int(keys_.size());
    S.segc.assign(size_t(S.K) * size_t(S.K), {});
    S.pool.assign(64, std::vector<float>(size_t(N) * (Search::MAXG + 2)));
    S.kc.resize(size_t(S.K) * size_t(N));
    S.kcpre.resize(size_t(S.K) * size_t(N));
    const float inv = 1.0f / (P.sigma_key * P.sigma_key);
    for (int k = 0; k < S.K; k++) {
        float acc = 0;
        for (int t = 0; t < N; t++) {
            const float dx = S.px[size_t(t)] - keys_[size_t(k)].x, dy = S.py[size_t(t)] - keys_[size_t(k)].y;
            S.kc[size_t(k) * size_t(N) + size_t(t)] = (dx * dx + dy * dy) * inv;
            acc += (dx * dx + dy * dy) * inv * S.wpt;
            S.kcpre[size_t(k) * size_t(N) + size_t(t)] = acc;
        }
    }
    Beam B(S);
    B.run();
    const_cast<Decoder*>(this)->last_expanded = B.pushed;
    std::unordered_map<std::u16string, std::pair<float, float>> by;
    for (auto& f : B.fin) {
        std::u16string r = B.text(f.first);
        auto it = by.find(r);
        if (it == by.end() || it->second.first > f.second.first) by[r] = f.second;
    }
    for (auto& r : by) out.push_back({r.first, r.second.first, r.second.second});
    std::sort(out.begin(), out.end(), [](const Result& a, const Result& b) { return a.cost < b.cost; });
    if (int(out.size()) > P.nbest) out.resize(size_t(P.nbest));
    return out;
}

}  // namespace glide

namespace glide {
std::string romanize(const std::u16string& rd, unsigned (*rng)()) {
    const auto& SP = spell().m;
    std::string o;
    for (size_t i = 0; i < rd.size();) {
        const std::vector<std::string>* v = nullptr;
        size_t n = 0;
        if (i + 1 < rd.size()) { auto it = SP.find(rd.substr(i, 2)); if (it != SP.end()) v = &it->second, n = 2; }
        if (!v) { auto it = SP.find(rd.substr(i, 1)); if (it == SP.end()) return ""; v = &it->second; n = 1; }
        const size_t pick = (rng && v->size() > 1 && rng() % 10 < 3) ? 1 + rng() % (v->size() - 1) : 0;
        o += (*v)[pick];
        i += n;
    }
    return o;
}
}

namespace glide {
namespace {
inline float lse(float a, float b) {
    if (a < b) std::swap(a, b);
    return b <= -1e29f ? a : a + std::log1p(std::exp(b - a));
}
}  // namespace

std::vector<Result> Decoder::decode_ctc(const std::vector<float>& lp, int frames, int nkeys, float lm_w, float len_w, int beam, int nbest) const {
    struct Hyp { int32_t node; float pb, pnb; std::u16string s; };
    const float NEG = -1e30f;
    const int W = 1 + nkeys;
    auto key_of = [&](int32_t nd) { const char16_t c = node_[size_t(nd)].ch; return c < 128 ? kidx_[c] : -1; };
    std::vector<Hyp> cur{Hyp{0, 0.0f, NEG, u""}};
    std::unordered_map<int32_t, size_t> at;
    std::vector<Hyp> nxt;
    // 同じキーの字が続く所 (hello の ll) はなぞりでは見えないので、子へ放出なしで進める
    auto close = [&](std::vector<Hyp>& hs) {
        for (size_t i = 0; i < hs.size(); i++) {
            const int32_t nd = hs[i].node;
            if (nd == 0) continue;
            const int k = key_of(nd);
            for (int32_t c = node_[size_t(nd)].first; c >= 0; c = node_[size_t(c)].next) {
                if (key_of(c) != k) continue;
                auto it = at.find(c);
                if (it == at.end()) { at[c] = hs.size(); hs.push_back(Hyp{c, hs[i].pb, hs[i].pnb, hs[i].s + node_[size_t(c)].ch}); }
                else { Hyp& h = hs[it->second]; h.pb = lse(h.pb, hs[i].pb); h.pnb = lse(h.pnb, hs[i].pnb); }
            }
        }
    };
    for (int t = 0; t < frames; t++) {
        const float* r = &lp[size_t(t) * W];
        nxt.clear(); at.clear();
        auto add = [&](int32_t nd, const std::u16string& s, float pb, float pnb) {
            auto it = at.find(nd);
            if (it == at.end()) { at[nd] = nxt.size(); nxt.push_back(Hyp{nd, pb, pnb, s}); }
            else { Hyp& h = nxt[it->second]; h.pb = lse(h.pb, pb); h.pnb = lse(h.pnb, pnb); }
        };
        for (const Hyp& h : cur) {
            const float tot = lse(h.pb, h.pnb);
            add(h.node, h.s, tot + r[0], NEG);                                   // blank
            const int k = h.node ? key_of(h.node) : -1;
            if (k >= 0) add(h.node, h.s, NEG, h.pnb + r[1 + k]);                // 同じキーに居続ける
            for (int32_t c = node_[size_t(h.node)].first; c >= 0; c = node_[size_t(c)].next) {
                const int kc = key_of(c);
                if (kc < 0 || kc == k) continue;                                 // 同じキーの子は close で
                add(c, h.s + node_[size_t(c)].ch, NEG, tot + r[1 + kc]);
            }
        }
        close(nxt);
        // 残すのは CTC の点 + 木の先で取れる一番良い語の頻度 (先読み) の上位 beam
        auto rank = [&](const Hyp& h) { return lse(h.pb, h.pnb) - lm_w * float(node_[size_t(h.node)].minsub) / 500.0f; };
        if (int(nxt.size()) > beam) {
            std::nth_element(nxt.begin(), nxt.begin() + beam, nxt.end(), [&](const Hyp& a, const Hyp& b) { return rank(a) > rank(b); });
            nxt.resize(size_t(beam));
        }
        cur.swap(nxt);
        at.clear();
        for (size_t i = 0; i < cur.size(); i++) at[cur[i].node] = i;
    }
    std::vector<Result> out;
    for (const Hyp& h : cur) {
        const int16_t c = node_[size_t(h.node)].cost;
        if (h.node == 0 || c == NOC) continue;
        const float sc = lse(h.pb, h.pnb) - lm_w * float(c) / 500.0f + len_w * float(h.s.size());
        out.push_back(Result{h.s, -sc, lse(h.pb, h.pnb)});
    }
    std::sort(out.begin(), out.end(), [](const Result& a, const Result& b) { return a.cost < b.cost; });
    if (int(out.size()) > nbest) out.resize(size_t(nbest));
    return out;
}

}  // namespace glide


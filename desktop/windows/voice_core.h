// 声の入力の認識 (2026-11-16)。録音 (16 kHz・モノラル) → E1 (voice_e1.h) のかなの確率 → 辞書の読みの木 (voice_trie.bin) の上を
// CTC のままたどる語の網 → 上位の読みのうち 1 位を土台に、ほかを打ち間違いと同じ言い換えの辺 (kkc_set_voice_alts) にして 1 回変換する
//   VoiceRecognizer v; v.open(lex, model, trie, e1);  auto r = v.recognize(pcm, n);   // r.cands = 候補 (UTF-16)
// エンジンは engine.h の関数だけで使う (IME 本体と同じ engine.obj)
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "voice_e1.h"

namespace voice {

// E1 の列の並び: 0 = 空白、i = KANA[i - 1]
inline const char32_t* KANA = U"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん"
                              U"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽぁぃぅぇぉっゃゅょゎー";

// 辞書の読みを発音のかなにした木 (voice_trie.bin を書き出す道具が作る)。
// 形: "TRI1" + int32 [節の数 nn, 子の数, 終わりの数, 読みの数, 書くかなの字の数]
//     + int32 子の始まり [nn+1] + (int32 ラベル, int32 子) × 子の数 + int32 終わりの始まり [nn+1] + (int32 読み, int32 コスト) × 終わりの数
//     + int32 読みの始まり [読みの数+1] + uint16 書くかな
struct Trie {
    std::vector<int32_t> co, eo, ro;
    std::vector<std::pair<uint8_t, int32_t>> cp;
    std::vector<std::pair<int32_t, int32_t>> ep;
    std::vector<char32_t> rc;
    template <class P> struct Span { const P *b, *e; const P* begin() const { return b; } const P* end() const { return e; } };
    Span<std::pair<uint8_t, int32_t>> ch(int i) const { return {cp.data() + co[size_t(i)], cp.data() + co[size_t(i) + 1]}; }
    Span<std::pair<int32_t, int32_t>> ends(int i) const { return {ep.data() + eo[size_t(i)], ep.data() + eo[size_t(i) + 1]}; }
    std::u32string read(int i) const { return std::u32string(rc.data() + ro[size_t(i)], rc.data() + ro[size_t(i) + 1]); }
    bool load(const uint8_t* b, size_t sz) {
        if (sz < 24 || std::memcmp(b, "TRI1", 4)) return false;
        int32_t hd[5];
        std::memcpy(hd, b + 4, 20);
        for (int v : hd) if (v < 0) return false;
        size_t o = 24;
        const size_t need = 24 + 4 * ((size_t(hd[0]) + 1) * 2 + size_t(hd[1]) * 2 + size_t(hd[2]) * 2 + size_t(hd[3]) + 1) + 2 * size_t(hd[4]);
        if (need != sz) return false;
        auto ints = [&](std::vector<int32_t>& v, size_t k) { v.resize(k); std::memcpy(v.data(), b + o, k * 4); o += k * 4; };
        std::vector<int32_t> tmp;
        ints(co, size_t(hd[0]) + 1);
        ints(tmp, size_t(hd[1]) * 2);
        cp.resize(size_t(hd[1]));
        for (size_t i = 0; i < cp.size(); i++) cp[i] = {uint8_t(tmp[2 * i]), tmp[2 * i + 1]};
        ints(eo, size_t(hd[0]) + 1);
        ints(tmp, size_t(hd[2]) * 2);
        ep.resize(size_t(hd[2]));
        for (size_t i = 0; i < ep.size(); i++) ep[i] = {tmp[2 * i], tmp[2 * i + 1]};
        ints(ro, size_t(hd[3]) + 1);
        rc.resize(size_t(hd[4]));
        for (size_t i = 0; i < rc.size(); i++) { uint16_t c; std::memcpy(&c, b + o + 2 * i, 2); rc[i] = c; }
        return true;
    }
};

inline double lse(double a, double b) {
    if (a == -INFINITY) return b;
    if (b == -INFINITY) return a;
    return a > b ? a + std::log1p(std::exp(b - a)) : b + std::log1p(std::exp(a - b));
}

// 仮説: ここまでの書くかな + 木の中の位置。pb / pnb は CTC の「最後が空白 / 最後が字」の log p
struct Hyp {
    std::u32string w;
    int node = 0, last = 0, nw = 0;
    double pb = -INFINITY, pnb = -INFINITY, bonus = 0;   // bonus = 語の罰点と辞書のコスト (探索の順位づけだけに使う)
    double ac() const { return lse(pb, pnb); }
    double score() const { return ac() + bonus; }
};
struct Key {
    size_t operator()(const std::pair<std::u32string, int>& k) const { return std::hash<std::u32string>()(k.first) ^ (size_t(k.second) * 0x9E3779B97F4A7C15ULL); }
};
using Beam = std::unordered_map<std::pair<std::u32string, int>, Hyp, Key>;

inline void merge(Beam& B, const Hyp& h) {
    auto k = std::make_pair(h.w, h.node);
    auto it = B.find(k);
    if (it == B.end()) { B.emplace(k, h); return; }
    Hyp& o = it->second;
    // 語の区切り方が違うだけの道も同じ鍵に来るので、和でなく大きい方
    o.pb = std::max(o.pb, h.pb);
    o.pnb = std::max(o.pnb, h.pnb);
    if (h.bonus > o.bonus) { o.bonus = h.bonus; o.nw = h.nw; }
}

inline void prune(Beam& B, int W) {
    if (int(B.size()) <= W) return;
    std::vector<std::pair<double, std::pair<std::u32string, int>>> v;
    for (auto& kv : B) v.push_back({kv.second.score(), kv.first});
    std::nth_element(v.begin(), v.begin() + W, v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    Beam nb;
    for (int i = 0; i < W; i++) nb.emplace(v[size_t(i)].second, B[v[size_t(i)].second]);
    B.swap(nb);
}

// 語の網: 上位 N 個の読み (書くかな)。W = ビーム幅、wpen = 語ごとの罰点、dict_w = 辞書のコストの重み
inline std::vector<Hyp> search(const Trie& T, const std::vector<float>& lp, int TT, int V, int W = 128, double wpen = -1.0,
                               int N = 32, double dict_w = 1e-3) {
    Beam B;
    Hyp h0;
    h0.pb = 0;
    B.emplace(std::make_pair(h0.w, 0), h0);
    for (int t = 0; t < TT; t++) {
        const float* row = &lp[size_t(t) * V];
        prune(B, W);
        Beam nx;
        auto extend = [&](const Hyp& h, const std::u32string& w, int from, double bonus, int nw) {
            const double tot = h.ac();
            for (auto& c : T.ch(from)) {
                const float p = row[c.first];
                if (p < -10.0f) continue;
                Hyp g = h;
                g.w = w; g.node = c.second; g.last = c.first; g.bonus = bonus; g.nw = nw;
                g.pb = -INFINITY;
                g.pnb = (c.first == h.last ? h.pb : tot) + p;   // 同じ字の繰り返しは空白を挟んだときだけ
                merge(nx, g);
            }
        };
        for (auto& kv : B) {
            const Hyp& h = kv.second;
            Hyp s = h;   // 同じ所に留まる (空白、または同じ字の続き)
            s.pb = h.ac() + row[0];
            s.pnb = h.last ? h.pnb + row[h.last] : -INFINITY;
            merge(nx, s);
            extend(h, h.w, h.node, h.bonus, h.nw);   // 語の中を進む
            if (h.node != 0)
                for (auto& e : T.ends(h.node))   // 語を閉じて次の語の最初の字へ
                    extend(h, h.w + T.read(e.first), 0, h.bonus + wpen - e.second * dict_w, h.nw + 1);
        }
        B.swap(nx);
    }
    std::vector<Hyp> out;
    for (auto& kv : B) {
        const Hyp& h = kv.second;
        if (h.node == 0) continue;
        for (auto& e : T.ends(h.node)) {
            Hyp g = h;
            g.w += T.read(e.first); g.node = 0; g.bonus += wpen - e.second * dict_w; g.nw++;
            out.push_back(g);
        }
    }
    // 同じ読みは 1 つに (点は大きい方)
    std::sort(out.begin(), out.end(), [](const Hyp& a, const Hyp& b) { return a.w != b.w ? a.w < b.w : a.score() > b.score(); });
    out.erase(std::unique(out.begin(), out.end(), [](const Hyp& a, const Hyp& b) { return a.w == b.w; }), out.end());
    std::sort(out.begin(), out.end(), [](const Hyp& a, const Hyp& b) { return a.score() > b.score(); });
    if (int(out.size()) > N) out.resize(size_t(N));
    return out;
}

inline std::u16string to16(const std::u32string& s) {
    std::u16string o;
    for (char32_t c : s) {
        if (c >= 0x10000) { c -= 0x10000; o.push_back(char16_t(0xD800 + (c >> 10))); o.push_back(char16_t(0xDC00 + (c & 0x3FF))); }
        else o.push_back(char16_t(c));
    }
    return o;
}

struct Result {
    std::u16string base, greedy;       // 語の網の 1 位の読み・E1 の各フレームの 1 位をつないだかな
    std::vector<std::u16string> cands;  // 変換の候補
    struct Alt { std::u16string read; double ac, score; int32_t cost; };
    std::vector<Alt> alts;              // 語の網の上位の読み (音の log p・探索の点・言い換えのコスト。0 番が土台)
    double ac_ms = 0, conv_ms = 0;
};

class Recognizer {
public:
    // lex・model はエンジンが写さずに読むので、呼ぶ側が閉じるまで持っておく。trie・e1 は写すので返してよい
    bool open(const uint8_t* lex, size_t ls, const uint8_t* model, size_t ms, const uint8_t* trie, size_t ts, const uint8_t* e1, size_t es) {
        close();
        if (!trie_.load(trie, ts) || !e1_.load(e1, es)) return false;
        e_ = kkc_open(lex, ls, model, ms);
        return e_ != nullptr;
    }
    void close() { if (e_) kkc_close(e_); e_ = nullptr; }
    ~Recognizer() { close(); }

    // pcm = 16 kHz・モノラル・[-1, 1]。ctx = 左の文脈 (無ければ空)
    Result recognize(const float* pcm, int n, const std::u16string& ctx = {}) {
        Result R;
        if (!e_ || n <= 0) return R;
        auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        const double t0 = now();
        std::vector<float> lp;
        const int TT = e1_.run(pcm, n, lp), V = e1_.nc;
        const double t1 = now();
        R.ac_ms = t1 - t0;
        std::u32string greedy;
        for (int t = 0, prev = 0; t < TT; t++) {
            int b = 0;
            for (int j = 1; j < V; j++) if (lp[size_t(t) * V + j] > lp[size_t(t) * V + b]) b = j;
            if (b != prev && b != 0) greedy += KANA[b - 1];
            prev = b;
        }
        R.greedy = to16(greedy);
        if (TT <= 0) return R;
        const auto hyps = search(trie_, lp, TT, V);
        if (hyps.empty()) return R;
        R.base = to16(hyps[0].w);
        R.alts.push_back({R.base, hyps[0].ac(), hyps[0].score(), 0});
        // 言い換えのコスト = 0.5 × 1000 × (土台の音の log p − 言い換えの音の log p)
        std::vector<uint16_t> buf;
        std::vector<int32_t> lens, costs;
        for (size_t r = 1; r < hyps.size(); r++) {
            const std::u16string a = to16(hyps[r].w);
            buf.insert(buf.end(), a.begin(), a.end());
            lens.push_back(int32_t(a.size()));
            costs.push_back(int32_t(std::max(0.0, 0.5 * 1000 * (hyps[0].ac() - hyps[r].ac()))));
            R.alts.push_back({a, hyps[r].ac(), hyps[r].score(), costs.back()});
        }
        kkc_set_voice_alts(e_, (const uint16_t*)R.base.data(), int(R.base.size()), buf.data(), lens.data(), costs.data(), int(lens.size()));
        std::vector<uint16_t> out(1 << 15);
        const int m = kkc_convert(e_, (const uint16_t*)ctx.data(), int(ctx.size()), (const uint16_t*)R.base.data(), int(R.base.size()), 10, 1,
                                  out.data(), int(out.size()));
        kkc_set_voice_alts(e_, nullptr, 0, nullptr, nullptr, nullptr, 0);
        for (int i = 0, c = 0; c < m; c++) {
            std::u16string s;
            while (out[size_t(i)]) s.push_back(char16_t(out[size_t(i++)]));
            i++;
            R.cands.push_back(s);
        }
        R.conv_ms = now() - t1;
        return R;
    }

private:
    kkc_engine* e_ = nullptr;
    Trie trie_;
    E1 e1_;
};

}  // namespace voice

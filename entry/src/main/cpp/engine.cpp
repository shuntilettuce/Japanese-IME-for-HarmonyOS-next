// 端末で動くかな漢字変換エンジン。engine.h の説明を参照。
//
// 1 回の変換:
//   1. 網 (lattice.Lexicon.edges と同じ辺を同じ順に)
//   2. 下書き = 辞書のコストだけの 1 位 (best_path)
//   3. エンコーダ: [文脈 40 字] SEP [読み] SEP [下書き] を Transformer (pre-LN) に 1 回通す
//   4. 区間 q と語 k。どちらも最初の層は線形なので、区間は位置ごと・語は文字ごとに前もって掛けておき、
//      区間・語ごとには足し算 + 2 層目だけを計算する
//   5. u = q·k / √dk を辺の点数にして K-best Viterbi (lattice.nbest と同じ順・同じ浮動小数の計算)
//
// ファイル形式 (学習側の書き出しの道具が書く。各配列は 8 バイト境界から):
//   lex.bin   "KKL2" int32×13 (版, 名詞, 算用数字, 漢数字, 最長の読み, OOV, カタカナ, カタカナ/字, 素通し, 品詞数, conn16, 読み数, 語数)
//             int64×2 (読みの UTF-16 の総数, 表記の UTF-16 の総数)
//             conn[品詞数²] (int16 か int32)  読みの位置 u32[読み数+1]  語の範囲 u32[読み数+1]  語 {u32 表記の位置, u16 長さ, u16 左, u16 右, u16, i32 コスト}[語数]
//             読み u16[]  表記 u16[]
//   model.bin "KKM1" int32×12 (版, 語彙, d, 層, 頭, ff, dk, 最大長, 区分, 文脈, 下書きの最大, 下書きあり) float×2 (β, γ)
//             int32 文字数, u32 文字[] (語彙の 4 番から)、あとは float32 の重み (export_model の順)
//             版 3 は最後にダイヤル (辺ごとに Mozc の語のコストをどれだけ信じるか、学習の --cost-gate) の重み (dk, dk) と (dk) が付く
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// x86 (PC) では、AVX2 + FMA が使える CPU のときだけ速い版の計算 (engine_simd.h) を使う (起動時に CPU を見て選ぶ)。
// ビルドは古い命令のまま (どの x86 の CPU でも動く)。ARM (スマホ) は今までどおり (コンパイラが NEON にする)
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define KKC_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
// MSVC は engine_avx2.cpp (/arch:AVX2) にある
extern "C" void kkc_avx2_lin(const float* X, int T, int in, const float* WT, int ldw, const float* B, float* Y, int ldy, int o0, int o1);
extern "C" void kkc_avx2_gelu(float* v, size_t n);
extern "C" void kkc_avx2_attend(const float* qkv, int T, int d, int dh, int hh, float scale, float* sc, float* att);
extern "C" void kkc_avx2_quant_rows(const float* X, int T, int in, int inp, short* Xq, float* xs);
extern "C" void kkc_avx2_lin_q(const short* Xq, const float* xs, int T, int inp, const short* Wp, const float* ws,
                               const float* B, float* Y, int ldy, int o0, int o1);
#else
#define KKC_AVX2_FN __attribute__((target("avx2,fma")))
#include "engine_simd.h"
#define kkc_avx2_lin kkc_simd_lin
#define kkc_avx2_gelu kkc_simd_gelu
#define kkc_avx2_attend kkc_simd_attend
#define kkc_avx2_quant_rows kkc_simd_quant_rows
#define kkc_avx2_lin_q kkc_simd_lin_q
#endif
#else
#define KKC_X86 0
#endif

namespace {

typedef std::u32string ustr;
enum Kind { K_DICT = 0, K_KANA = 1, K_KATA = 2, K_PASS = 3, K_NUM = 4, K_TYPO = 5, K_LATIN = 6 };   // K_TYPO: 打ち間違いを直した語 (S7 から)。K_LATIN: ローマ字で打った英単語
const int UNK = 1, SEP = 2;

struct Ent { uint32_t soff; uint16_t slen, lid, rid, pad; int32_t cost; };
static_assert(sizeof(Ent) == 16, "Ent");
// 辞書の版 2 の語 (10 バイト): 表記の位置 = a | (b の下 10 ビット) << 16、表記の長さ = b の上 6 ビット
struct PEnt { uint16_t a, b, lid, rid; int16_t cost; };
static_assert(sizeof(PEnt) == 10, "PEnt");

struct Edge { int s, e; ustr surf; int32_t lid, rid, cost; Kind kind; int mark = 0; };   // mark: M5 の印 (使う人 + 32 × 書き方)

// 英単語の一覧の項目 (Lex::latin_words)。小文字の綴り -> 表記・品詞・コスト
struct LatinWord { ustr surf; int32_t lid, rid, cost; };

// 1 位の候補の語: 読みの終わりの位置・表記の長さ (UTF-16)・品詞の左右 ID
struct SegInfo { int e, l16, lid, rid; };
// 上位の経路 1 本 (候補の木の窓用): コストと語ごとの (読みの終わり, 表記)
struct PathInfo { double c; std::vector<std::pair<int, ustr>> w; };
// 直前の nbest (1 位の区切りを取ったもの) の上位の経路。kkc_convert の終わりで kkc_engine に写す
thread_local std::vector<PathInfo> tl_paths;

// ユーザー辞書の語 (読み・表記・品詞の左右 ID・コスト)。辞書の語と同じく網に入れ、モデルが文脈で採点する
struct UserWord { ustr r, s; int32_t lid, rid, cost; bool whole = false; };   // whole = 読み全体が一致したときだけ (短縮よみ)

inline bool is_kana(char32_t c) { return (c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x3094; }
inline char32_t kata(char32_t c) { return (c >= 0x3041 && c <= 0x3096) ? c + 0x60 : c; }
ustr to_kata(const ustr& s) { ustr o = s; for (auto& c : o) c = kata(c); return o; }

ustr from16(const uint16_t* p, size_t n) {
    ustr o;
    o.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = p[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (p[i + 1] - 0xDC00);
            i++;
        }
        o.push_back(char32_t(c));
    }
    return o;
}

void to16(const ustr& s, std::vector<uint16_t>& o) {
    for (char32_t c : s) {
        if (c >= 0x10000) {
            uint32_t v = uint32_t(c) - 0x10000;
            o.push_back(uint16_t(0xD800 + (v >> 10)));
            o.push_back(uint16_t(0xDC00 + (v & 0x3FF)));
        } else o.push_back(uint16_t(c));
    }
}

// ---------------------------------------------------------------- 辞書 (mmap したファイルを読むだけ)
// 辞書。版 1: 語は 16 バイト、読みはそのまま。版 2 (kkc/pack_lex.py): 語は 10 バイト、同じ表記は 1 つにまとめ、
// 読みは RB 個ごとの区切りの先頭だけそのままで、他は「前の読みと同じ先頭の字数 + 残りの字」(読みは並んでいる)。
// 中の語と答えは版 1 とまったく同じ (63MB -> 約 47MB)
struct Lex {
    int32_t noun, num_arabic, num_kanji, max_word, oov, kata, kata_per, pass, nconn, conn16, nread, nent;
    int32_t ver = 1;
    const int16_t* c16 = nullptr;
    const int32_t* c32 = nullptr;
    const uint32_t* r_off;
    const uint32_t* e_first;
    const Ent* ents = nullptr;     // 版 1
    const PEnt* pents = nullptr;   // 版 2
    const uint16_t* r_blob;
    const uint16_t* s_blob;
    bool typo = false;   // 打ち間違いの辺を作るか (モデルが辺の種類 K_TYPO を知っているとき。kkc_open が決める)
    bool latin = false;  // 英字の辺 (K_LATIN) を作るか (モデルが辺の種類 K_LATIN を知っているとき。kkc_open が決める)
    // 英字だけの語 (a-z A-Z と数字、2 字以上) の表: 小文字の綴り -> 表記・品詞・コスト (kkc_open が作って使い回す)
    std::unordered_map<std::string, LatinWord> latin_words;
    // 表記の頭の字 -> (語の番号, 読みの番号)。漢字を含む 2 字以上の語だけ。確定済みの漢字の語の続き (直滑 + こう → 降) を探すのに使う。
    // 初めて要るときに作る (kanji_continuation_edges)
    mutable std::unordered_map<char32_t, std::vector<std::pair<uint32_t, uint32_t>>> head_idx;
    mutable bool head_built = false;
    int32_t abbr_lid = -1, abbr_rid = -1;   // 略語 (読みのアルファベットから作る GDP・IME) の品詞 (kkc_open で辞書の GDP から写す)
    static constexpr int RB = 16;      // 版 2 の読みの区切り
    static constexpr int MAXR = 256;   // 読みの長さの上限 (pack_lex.py が確かめる)

    inline int conn(int a, int b) const { size_t i = size_t(a) * nconn + b; return c16 ? c16[i] : c32[i]; }

    // j 番目の語
    inline Ent ent(uint32_t j) const {
        if (ents) return ents[j];
        const PEnt& p = pents[j];
        return {uint32_t(p.a) | (uint32_t(p.b & 0x3FF) << 16), uint16_t(p.b >> 10), p.lid, p.rid, 0, int32_t(p.cost)};
    }

    // 版 2: buf に k-1 番目の読みが入っているとき (k が区切りの先頭なら何でもよい)、k 番目にする。長さを返す
    inline int next_reading(int k, uint16_t* buf) const {
        const uint16_t* p = r_blob + r_off[k];
        const int len = int(r_off[k + 1] - r_off[k]);
        if (k % RB == 0) { memcpy(buf, p, size_t(len) * 2); return len; }
        const int pre = p[0];
        memcpy(buf + pre, p + 1, size_t(len - 1) * 2);
        return pre + len - 1;
    }

    // i 番目の読み (長さを n に)。版 1 は辞書の中をそのまま指す。版 2 は buf (MAXR 字) に戻して返す
    const uint16_t* reading(int i, int& n, uint16_t* buf) const {
        if (ver == 1) { n = int(r_off[i + 1] - r_off[i]); return r_blob + r_off[i]; }
        for (int k = i - i % RB; k <= i; k++) n = next_reading(k, buf);
        return buf;
    }

    static int cmp(const uint16_t* r, int rn, const uint16_t* k, int n) {
        const int m = std::min(rn, n);
        for (int i = 0; i < m; i++) if (r[i] != k[i]) return int(r[i]) - int(k[i]);
        return rn - n;
    }

    // 読みが k 以上の最初の読みの番号 (読みは並んでいる)
    int lower_bound(const uint16_t* k, int n) const {
        if (ver == 1) {
            int lo = 0, hi = nread;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cmp(r_blob + r_off[mid], int(r_off[mid + 1] - r_off[mid]), k, n) < 0) lo = mid + 1; else hi = mid;
            }
            return lo;
        }
        // 版 2: 区切りの先頭 (そのまま入っている) で二分探索して、k 以上の先頭の最初の区切りを探し、その前の区切りの中を順に見る
        const int nb = (nread + RB - 1) / RB;
        int lo = 0, hi = nb;
        while (lo < hi) {
            int mid = (lo + hi) >> 1, h = mid * RB;
            if (cmp(r_blob + r_off[h], int(r_off[h + 1] - r_off[h]), k, n) < 0) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return 0;
        uint16_t buf[MAXR];
        const int end = std::min(nread, lo * RB);
        int i = (lo - 1) * RB;
        next_reading(i, buf);   // 先頭は k より前
        for (i++; i < end; i++)
            if (cmp(buf, next_reading(i, buf), k, n) >= 0) return i;
        return end;
    }

    // 読み (UTF-16 の並び) の語の範囲。無ければ false
    // 読みの集合のブルームフィルタ (打ち間違いの辺を作るときだけ作る。辞書に無い区間の二分探索をほぼ省ける)
    std::vector<uint64_t> bloom;
    static uint64_t hash16(const uint16_t* k, int n) {
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < n; i++) { h ^= k[i]; h *= 1099511628211ULL; }
        return h ^ (h >> 29);
    }
    // 読みを先頭から順に (版 2 は前の読みから戻すので、1 つずつ reading(i) で引くと区切りの頭から読み直して 16 倍遅い)
    template <class F> void each_reading(F f) const {
        uint16_t buf[MAXR];
        for (int i = 0; i < nread; i++) {
            int rn;
            const uint16_t* r = ver == 1 ? (rn = int(r_off[i + 1] - r_off[i]), r_blob + r_off[i]) : (rn = next_reading(i, buf), buf);
            f(r, rn);
        }
    }
    void build_bloom() {
        if (!bloom.empty() || nread <= 0) return;
        const size_t bits = size_t(nread) * 12;
        bloom.assign((bits + 63) / 64, 0);
        each_reading([&](const uint16_t* r, int rn) {
            const uint64_t h = hash16(r, rn), h2 = (h >> 32) | 1;
            for (int j = 0; j < 4; j++) { const size_t b = size_t((h + j * h2) % bits); bloom[b >> 6] |= 1ULL << (b & 63); }
        });
        build_pbloom();
    }
    // 読みの頭 (どこかの読みがこれで始まる) のブルームフィルタ。打ち間違いの辺づくりで、区間を後ろへ延ばしても語が無いと
    // 分かったら延ばすのをやめる (結果は同じ。頭の無い区間の引きを省く)。
    // 大きさは頭の延べ数 (読みの字数の合計) から決める (前は同じ頭を 1 つにする集合を作っていて、辞書を開くのに 2 秒かかった。
    // ブルームフィルタは「無い」の判定だけに使うので、大きさを変えても結果は同じ)
    std::vector<uint64_t> pbloom;
    size_t pbits = 0;
    void build_pbloom() {
        if (!pbloom.empty() || nread <= 0) return;
        size_t total = 0;
        each_reading([&](const uint16_t*, int rn) { total += size_t(rn); });
        pbits = std::max<size_t>(64, total * 6);
        pbloom.assign((pbits + 63) / 64, 0);
        each_reading([&](const uint16_t* r, int rn) {
            uint64_t h = 1469598103934665603ULL;
            for (int j = 0; j < rn; j++) {
                h ^= r[j]; h *= 1099511628211ULL;
                const uint64_t x = h ^ (h >> 29), x2 = (x >> 32) | 1;
                for (int t = 0; t < 3; t++) { const size_t b = size_t((x + t * x2) % pbits); pbloom[b >> 6] |= 1ULL << (b & 63); }
            }
        });
    }
    // h = 読みの頭の FNV (hash16 と同じ作り)。無ければ false (あるかもしれなければ true)
    bool maybe_head(uint64_t h) const {
        if (pbloom.empty()) return true;
        const uint64_t h2 = (h >> 32) | 1;
        for (int j = 0; j < 3; j++) { const size_t b = size_t((h + j * h2) % pbits); if (!(pbloom[b >> 6] >> (b & 63) & 1)) return false; }
        return true;
    }
    bool maybe(const uint16_t* k, int n) const { return maybe_hash(hash16(k, n)); }
    // h = hash16 の値
    bool maybe_hash(uint64_t h) const {
        if (bloom.empty()) return true;
        const size_t bits = bloom.size() * 64 > size_t(nread) * 12 ? size_t(nread) * 12 : bloom.size() * 64;
        const uint64_t h2 = (h >> 32) | 1;
        for (int j = 0; j < 4; j++) { const size_t b = size_t((h + j * h2) % bits); if (!(bloom[b >> 6] >> (b & 63) & 1)) return false; }
        return true;
    }

    bool find(const uint16_t* k, int n, uint32_t& lo_e, uint32_t& hi_e) const {
        if (!maybe(k, n)) return false;
        const int lo = lower_bound(k, n);
        if (lo >= nread) return false;
        uint16_t buf[MAXR];
        int rn;
        const uint16_t* r = reading(lo, rn, buf);
        if (rn != n || memcmp(r, k, size_t(n) * 2)) return false;
        lo_e = e_first[lo];
        hi_e = e_first[lo + 1];
        return true;
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    size_t off = 0;
    bool ok = true;
    template <class T> const T* arr(size_t n) {
        off = (off + 7) & ~size_t(7);
        if (off + n * sizeof(T) > size_t(end - p)) { ok = false; return nullptr; }
        const T* r = reinterpret_cast<const T*>(p + off);
        off += n * sizeof(T);
        return r;
    }
    template <class T> T val() {
        T v;
        if (off + sizeof(T) > size_t(end - p)) { ok = false; return T(); }
        memcpy(&v, p + off, sizeof(T));
        off += sizeof(T);
        return v;
    }
};

// ---------------------------------------------------------------- 数の読み (学習側と同じ規則)
struct Piece { ustr k; char kind; int64_t v; };
struct Sok { ustr k; int64_t v, unit; };
std::vector<Piece> PIECES;
std::vector<Sok> SOKUON;
const char32_t* KANJI = U"〇一二三四五六七八九";

void init_numbers() {
    if (!PIECES.empty()) return;
    std::vector<Piece> p = {
        {U"いち", 'd', 1}, {U"に", 'd', 2}, {U"さん", 'd', 3}, {U"よん", 'd', 4}, {U"し", 'd', 4}, {U"ご", 'd', 5},
        {U"ろく", 'd', 6}, {U"なな", 'd', 7}, {U"しち", 'd', 7}, {U"はち", 'd', 8}, {U"きゅう", 'd', 9}, {U"く", 'd', 9},
        {U"れい", 'd', 0}, {U"ぜろ", 'd', 0}, {U"いっ", 'd', 1}, {U"ろっ", 'd', 6}, {U"はっ", 'd', 8},
        {U"じゅう", 's', 10}, {U"ひゃく", 's', 100}, {U"びゃく", 's', 100}, {U"ぴゃく", 's', 100}, {U"せん", 's', 1000},
        {U"ぜん", 's', 1000}, {U"まん", 'b', 10000}, {U"おく", 'b', 100000000LL}, {U"ちょう", 'b', 1000000000000LL}};
    std::stable_sort(p.begin(), p.end(), [](const Piece& a, const Piece& b) { return a.k.size() > b.k.size(); });
    PIECES = p;
    SOKUON = {{U"いっ", 1, 1}, {U"ろっ", 6, 1}, {U"はっ", 8, 1}, {U"じゅっ", 10, 10}, {U"じっ", 10, 10},
              {U"ひゃっ", 100, 100}, {U"せっ", 1000, 1000}};
}

inline bool starts_with(const ustr& s, size_t i, const ustr& k) { return i + k.size() <= s.size() && s.compare(i, k.size(), k) == 0; }
inline bool ends_with(const ustr& s, const ustr& k) { return s.size() >= k.size() && s.compare(s.size() - k.size(), k.size(), k) == 0; }

bool parse(const ustr& r, int64_t& value, bool& sokuon) {
    if (r.size() < 2) return false;
    const Sok* sok = nullptr;
    for (auto& s : SOKUON) if (ends_with(r, s.k)) { sok = &s; break; }
    ustr body = sok ? r.substr(0, r.size() - sok->k.size()) : r;
    std::vector<std::pair<char, int64_t>> toks;
    size_t i = 0;
    ustr prev;
    while (i < body.size()) {
        bool hit = false;
        for (auto& p : PIECES)
            if (starts_with(body, i, p.k)) {
                // 連濁は決まった数のあとだけ: さんぜん・さんびゃく・ろっぴゃく・はっぴゃく (ごぜん は 午前 であって 5000 ではない)
                if ((p.k == U"ぜん" || p.k == U"びゃく") && prev != U"さん" && prev != U"なん") return false;
                if (p.k == U"ぴゃく" && prev != U"ろっ" && prev != U"はっ") return false;
                toks.push_back({p.kind, p.v});
                prev = p.k;
                i += p.k.size();
                hit = true;
                break;
            }
        if (!hit) return false;
    }
    if (sok) {
        if (sok->unit > 1 && sok->v == sok->unit) toks.push_back({'s', sok->unit});
        else toks.push_back({'d', sok->v});
    }
    if (toks.empty()) return false;
    int64_t total = 0, group = 0, last_small = 100000;
    bool has_digit = false; int64_t digit = 0;
    for (auto& t : toks) {
        if (t.first == 'd') {
            if (has_digit) return false;
            has_digit = true; digit = t.second;
        } else if (t.first == 's') {
            if (t.second >= last_small) return false;
            group += (has_digit ? digit : 1) * t.second;
            has_digit = false; last_small = t.second;
        } else {
            group += has_digit ? digit : 0;
            if (group == 0) return false;
            total += group * t.second;
            group = 0; has_digit = false; last_small = 100000;
        }
    }
    group += has_digit ? digit : 0;
    value = total + group;
    if (value == 0 && !(toks.size() == 1 && toks[0].first == 'd' && toks[0].second == 0)) return false;
    sokuon = sok != nullptr;
    return true;
}

ustr kanji(int64_t n) {
    if (n == 0) return U"〇";
    ustr out;
    const int64_t bigs[4] = {1000000000000LL, 100000000LL, 10000LL, 1};
    const char32_t* names[4] = {U"兆", U"億", U"万", U""};
    const int64_t units[4] = {1000, 100, 10, 1};
    const char32_t* un[4] = {U"千", U"百", U"十", U""};
    for (int b = 0; b < 4; b++) {
        int64_t g = n / bigs[b]; n %= bigs[b];
        if (!g) continue;
        ustr s;
        for (int u = 0; u < 4; u++) {
            int64_t d = g / units[u]; g %= units[u];
            if (d) { if (!(d == 1 && units[u] > 1)) s += KANJI[d]; s += un[u]; }
        }
        out += s + names[b];
    }
    return out;
}

ustr digits(int64_t v, bool full) {
    std::string a = std::to_string(v);
    ustr o;
    for (char c : a) o += full ? char32_t(0xFF10 + (c - '0')) : char32_t(c);
    return o;
}

// 3 桁ごとにカンマ (3500 -> 3,500)
ustr with_commas(const ustr& d) {
    ustr o;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) o.push_back(U',');
        o.push_back(d[i]);
    }
    return o;
}

// 万・億・兆の単位まじり (2000000000000000 -> 2000兆、120000000 -> 1億2000万、25000 -> 2万5000)。comma なら単位の中を 3 桁ごとに
ustr unit_mixed(int64_t v, bool comma) {
    static const std::pair<int64_t, const char32_t*> U[] = {{1000000000000LL, U"兆"}, {100000000LL, U"億"}, {10000LL, U"万"}, {1, U""}};
    ustr o;
    for (const auto& u : U) {
        const int64_t g = (v / u.first) % (u.first == 1000000000000LL ? 1000000 : 10000);
        if (!g) continue;
        const ustr d = digits(g, false);
        o += (comma ? with_commas(d) : d) + u.second;
    }
    return o;
}

// r[s..e) の数のすぐ後ろに単位 (じゅう・ひゃく・せん・まん・おく・ちょう) が続いて、もっと大きい数として読めるか
// (じゅうにおくさんぜん|まん の途中で切った数を候補にすると「十200003000万」のような断片ができた)
bool number_continues(const ustr& r, size_t s, size_t e) {
    for (auto& p : PIECES) {
        if (p.kind == 'd' || !starts_with(r, e, p.k)) continue;
        int64_t v;
        bool sk;
        if (parse(r.substr(s, e - s + p.k.size()), v, sk)) return true;
    }
    return false;
}

std::vector<size_t> boundaries(const ustr& r, size_t s) {
    std::vector<size_t> out;
    size_t i = s;
    while (i < r.size()) {
        bool hit = false;
        for (auto& p : PIECES) if (starts_with(r, i, p.k)) { i += p.k.size(); out.push_back(i); hit = true; break; }
        if (!hit) {
            for (auto& k : SOKUON) if (starts_with(r, i, k.k)) out.push_back(i + k.k.size());
            break;
        }
    }
    return out;
}

// 1 字の数 (ご・に・し・く)。parse は 2 字からなので、符号や式の後ろのときだけ使う
bool parse_num(const ustr& r, int64_t& v) {
    static const std::pair<char32_t, int> one[] = {{U'に', 2}, {U'し', 4}, {U'ご', 5}, {U'く', 9}};
    if (r.size() == 1) {
        for (auto& o : one) if (r[0] == o.first) { v = o.second; return true; }
        return false;
    }
    bool sk;
    return parse(r, v, sk) && !sk;
}

void number_edges(const Lex& L, const ustr& r, std::vector<Edge>& out) {
    const int base = 4000, per_char = -150;
    // 符号 + 1 字の数 (まいなすご -> -5、まいなすごど -> -5度)。2 字以上の数は下の決まりで符号を付ける
    for (const auto& sg : {std::pair<ustr, char32_t>{U"まいなす", U'-'}, std::pair<ustr, char32_t>{U"ぷらす", U'+'}}) {
        for (size_t i = r.find(sg.first); i != ustr::npos; i = r.find(sg.first, i + 1)) {
            const size_t s = i + sg.first.size();
            int64_t v;
            if (s < r.size() && parse_num(r.substr(s, 1), v))
                out.push_back({int(i), int(s + 1), ustr(1, sg.second) + digits(v, false), L.num_arabic, L.num_arabic, 3500, K_NUM});
        }
    }
    for (size_t s = 0; s < r.size(); s++) {
        for (size_t e : boundaries(r, s)) {
            if (e - s < 2 || e - s > 24) continue;
            int64_t v; bool sk;
            if (!parse(r.substr(s, e - s), v, sk)) continue;
            if (number_continues(r, s, e)) continue;   // まだ続く数の途中では切らない
            int c = std::max(3000, base + per_char * int(e - s));
            // 数の書き方を揃えて出す (にせんちょう -> 2000兆・2,000兆・2000000000000000・二千兆)。
            // 1 万以上は単位まじり (2000兆・1億2000万) を先に、数字だけ・カンマつきは後ろに
            const bool big = v >= 10000;
            const ustr d = digits(v, false);
            out.push_back({int(s), int(e), d, L.num_arabic, L.num_arabic, big ? c + 300 : c, K_NUM});
            if (v >= 1000) out.push_back({int(s), int(e), with_commas(d), L.num_arabic, L.num_arabic, big ? c + 400 : c + 150, K_NUM});
            if (big) {
                const ustr m = unit_mixed(v, false), mc = unit_mixed(v, true);
                out.push_back({int(s), int(e), m, L.num_arabic, L.num_arabic, c - 300, K_NUM});
                if (mc != m) out.push_back({int(s), int(e), mc, L.num_arabic, L.num_arabic, c - 100, K_NUM});
            }
            out.push_back({int(s), int(e), digits(v, true), L.num_arabic, L.num_arabic, c + 2000, K_NUM});
            out.push_back({int(s), int(e), kanji(v), L.num_kanji, L.num_kanji, c + 500, K_NUM});
            // 符号: まいなすごひゃく -> -500、ぷらすさん -> +3 (マイナス500 は「マイナス」+「500」の 2 語で出る)
            for (const auto& sg : {std::pair<ustr, char32_t>{U"まいなす", U'-'}, std::pair<ustr, char32_t>{U"ぷらす", U'+'}}) {
                const size_t k = sg.first.size();
                if (s >= k && r.compare(s - k, k, sg.first) == 0)
                    out.push_back({int(s - k), int(e), ustr(1, sg.second) + digits(v, false), L.num_arabic, L.num_arabic, c + 500, K_NUM});
            }
        }
    }
}

void special_edges(const Lex& L, const ustr& r, std::vector<Edge>& out) {
    const int cost = 4500;
    const std::vector<std::pair<ustr, ustr>> fixed = {{U"ひとり", U"1人"}, {U"ふたり", U"2人"}, {U"はたち", U"20歳"}};
    for (auto& f : fixed) {
        size_t i = r.find(f.first);
        while (i != ustr::npos) {
            out.push_back({int(i), int(i + f.first.size()), f.second, L.noun, L.noun, cost + 2000, K_NUM});
            i = r.find(f.first, i + 1);
        }
    }
    const std::vector<std::pair<ustr, int>> days = {
        {U"ついたち", 1}, {U"ふつか", 2}, {U"みっか", 3}, {U"よっか", 4}, {U"いつか", 5}, {U"むいか", 6}, {U"なのか", 7},
        {U"ようか", 8}, {U"ここのか", 9}, {U"とおか", 10}, {U"はつか", 20}};
    for (auto& w : days) {
        size_t e0 = r.find(w.first);
        while (e0 != ustr::npos) {
            size_t e = e0 + w.first.size();
            std::vector<std::pair<size_t, int64_t>> starts = {{e0, w.second}};
            if (w.second < 10) {
                for (size_t s = (e0 >= 12 ? e0 - 12 : 0); s < e0; s++) {
                    int64_t t; bool sk;
                    if (parse(r.substr(s, e0 - s), t, sk) && !sk && t % 10 == 0 && t > 0 && t <= 30)
                        starts.push_back({s, t + w.second});
                }
            }
            for (auto& sv : starts) {
                if (sv.second > 31) continue;
                int span = int(e - sv.first);
                out.push_back({int(sv.first), int(e), digits(sv.second, false) + U"日", L.noun, L.noun, cost + 200 * span, K_NUM});
                out.push_back({int(sv.first), int(e), digits(sv.second, true) + U"日", L.noun, L.noun, cost + 2000 + 200 * span, K_NUM});
                out.push_back({int(sv.first), int(e), kanji(sv.second) + U"日", L.noun, L.noun, cost + 300 + 200 * span, K_NUM});
            }
            e0 = r.find(w.first, e0 + 1);
        }
    }
}

void abbr_edges(const Lex& L, const ustr& r, std::vector<Edge>& out);
bool has_latin_word(const ustr& s);

// 辞書の英字の語 (略語でないもの: Zoom・Swift) のコストをこれだけ下げる。辞書のコストはカタカナが勝ちやすいため
// (英字交じりベンチ第 2 版・段階式で L 語 57.0 -> 61.2、K の英字にしなかった 82.5 -> 79.8、dev・AJIMEE・日常・入力ログは同じ)
constexpr int32_t LATIN_WORD_BONUS = 500;
// 英カタカナ辞書の印 (kkc/latin/add_words.py の eng)。このコスト以上の語は変換の網に入れず、読み全体がこの語のときだけ候補に足す
// (くらいあんと → client。網に入れると普通の外来語を乗っ取った)
constexpr int32_t ENG_COST = 32000;
constexpr int32_t ENG_LATIN_COST = 9000;   // 英カタカナ辞書の語を英字の辺に使うときのコスト (kkcnative2 と同じ)
// 英字の辺 (K_LATIN) のコストに上乗せする値。かな・カタカナの語より英字の語を出にくくする (日本語寄りにする)
constexpr int32_t LATIN_BIAS = 3000;
// 辞書に無い英字の並び (打ったまま・大文字・頭だけ大文字) の語のコスト (学習の網 kkcnative2 と同じ)
constexpr int32_t LATIN_RAW_COST = 6000;

// ---------------------------------------------------------------- 打ち間違いの辺 (S7、モデルが辺の種類 K_TYPO を知っているときだけ)
// 打った読みをローマ字に直し、打ち間違い 1 打ぶんを戻した読み (余分な 1 打を消す・隣のキーを正しいキーに・抜けた 1 打を足す・
// 隣り合う 2 打の入れ替え) を作る。直した読みの、変わった所を覆う辞書の語を、元の読みの区間に K_TYPO の辺として足す
// (コストに TYPO_COST を足す)。どれを選ぶかは文脈を読むモデルが決める。読み 1 つにつき安い順に TYPO_MAX 辺まで
namespace typo {
const int TYPO_COST = 3000, TYPO_MAX = 32, MAX_WORD_KANA = 8;
// 打ったとおりの網の一番よい道より、直した辺を通る一番よい道がこれ以上悪ければ捨てる (TYPO_COST 込み)
const int TYPO_KEEP = 2000;

// 打ったとおりの網の、辺ごとの「頭からその辺まで」「その辺から終わりまで」の一番よいコスト (辞書のコスト + つながり)
struct Ctx {
    int n = 0;
    std::vector<std::vector<int>> by_end, by_start;
    std::vector<double> F, B;
    std::vector<double> lmin, rmin;   // 位置ごとの「頭からここまで」「ここから終わりまで」の一番よいコスト (つながりを除いた下限)
    double best = 1e18;
};

void build_ctx(const Lex& L, const std::vector<Edge>& E, int n, Ctx& c) {
    c.n = n;
    c.by_end.assign(size_t(n + 1), {});
    c.by_start.assign(size_t(n + 1), {});
    for (int i = 0; i < int(E.size()); i++) { c.by_end[size_t(E[i].e)].push_back(i); c.by_start[size_t(E[i].s)].push_back(i); }
    c.F.assign(E.size(), 1e18);
    c.B.assign(E.size(), 1e18);
    for (int pos = 0; pos <= n; pos++)
        for (int i : c.by_start[size_t(pos)]) {
            double best = pos == 0 ? double(L.conn(0, E[i].lid)) : 1e18;
            for (int j : c.by_end[size_t(pos)]) best = std::min(best, c.F[size_t(j)] + L.conn(E[j].rid, E[i].lid));
            if (best < 1e17) c.F[size_t(i)] = best + E[i].cost;
        }
    for (int pos = n; pos >= 0; pos--)
        for (int i : c.by_end[size_t(pos)]) {
            double best = pos == n ? double(L.conn(E[i].rid, 0)) : 1e18;
            for (int k : c.by_start[size_t(pos)]) best = std::min(best, c.B[size_t(k)] + L.conn(E[i].rid, E[k].lid));
            if (best < 1e17) c.B[size_t(i)] = best + E[i].cost;
        }
    for (int j : c.by_end[size_t(n)]) c.best = std::min(c.best, c.F[size_t(j)] + L.conn(E[j].rid, 0));
    c.lmin.assign(size_t(n + 1), 1e18);
    c.rmin.assign(size_t(n + 1), 1e18);
    c.lmin[0] = 0;
    c.rmin[size_t(n)] = 0;
    for (int pos = 1; pos <= n; pos++) for (int j : c.by_end[size_t(pos)]) c.lmin[size_t(pos)] = std::min(c.lmin[size_t(pos)], c.F[size_t(j)]);
    for (int pos = 0; pos < n; pos++) for (int k : c.by_start[size_t(pos)]) c.rmin[size_t(pos)] = std::min(c.rmin[size_t(pos)], c.B[size_t(k)]);
}

// 区間 [s, e) に (lid, rid, cost) の辺を置いたときの、それを通る一番よい道のコスト
double through(const Lex& L, const std::vector<Edge>& E, const Ctx& c, int s, int e, int lid, int rid, int cost) {
    double left = s == 0 ? double(L.conn(0, lid)) : 1e18;
    for (int j : c.by_end[size_t(s)]) left = std::min(left, c.F[size_t(j)] + L.conn(E[j].rid, lid));
    double right = e == c.n ? double(L.conn(rid, 0)) : 1e18;
    for (int k : c.by_start[size_t(e)]) right = std::min(right, L.conn(rid, E[k].lid) + c.B[size_t(k)]);
    if (left >= 1e17 || right >= 1e17) return 1e18;
    return left + cost + right;
}
std::unordered_map<std::string, ustr> R2K;    // ローマ字 -> かな (IME の表の主なもの)
std::unordered_map<char32_t, std::string> K2R;          // かな 1 字 -> ローマ字
std::unordered_map<ustr, std::string> K2R2;             // かな 2 字 (きゃ) -> ローマ字
const char* ROWS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};

void init() {
    if (!R2K.empty()) return;
    const char* V = "aiueo";
    const std::pair<const char32_t*, const char*> rows[] = {
        {U"あいうえお", ""}, {U"かきくけこ", "k"}, {U"さしすせそ", "s"}, {U"たちつてと", "t"}, {U"なにぬねの", "n"},
        {U"はひふへほ", "h"}, {U"まみむめも", "m"}, {U"らりるれろ", "r"}, {U"がぎぐげご", "g"}, {U"ざじずぜぞ", "z"},
        {U"だぢづでど", "d"}, {U"ばびぶべぼ", "b"}, {U"ぱぴぷぺぽ", "p"}, {U"ぁぃぅぇぉ", "x"}};
    for (auto& rw : rows)
        for (int i = 0; i < 5; i++) {
            std::string k = std::string(rw.second) + V[i];
            R2K[k] = ustr(1, rw.first[i]);
            K2R[rw.first[i]] = k;
        }
    const std::pair<const char32_t*, const char*> extra[] = {
        {U"や", "ya"}, {U"ゆ", "yu"}, {U"よ", "yo"}, {U"わ", "wa"}, {U"を", "wo"}, {U"ゃ", "xya"}, {U"ゅ", "xyu"}, {U"ょ", "xyo"},
        {U"っ", "xtu"}, {U"ゎ", "xwa"}, {U"ー", "-"}, {U"し", "shi"}, {U"ち", "chi"}, {U"つ", "tsu"}, {U"ふ", "fu"}, {U"じ", "ji"},
        {U"ゔ", "vu"}};
    for (auto& x : extra) R2K[x.second] = x.first;
    const std::pair<char32_t, const char*> single[] = {
        {U'や', "ya"}, {U'ゆ', "yu"}, {U'よ', "yo"}, {U'わ', "wa"}, {U'を', "wo"}, {U'ゃ', "xya"}, {U'ゅ', "xyu"}, {U'ょ', "xyo"},
        {U'ゎ', "xwa"}, {U'ー', "-"}, {U'ゔ', "vu"}};
    for (auto& x : single) K2R[x.first] = x.second;
    // 拗音 (きゃ -> kya)
    const std::pair<char32_t, const char*> yoon[] = {
        {U'き', "ky"}, {U'し', "sy"}, {U'ち', "ty"}, {U'に', "ny"}, {U'ひ', "hy"}, {U'み', "my"}, {U'り', "ry"}, {U'ぎ', "gy"},
        {U'じ', "zy"}, {U'び', "by"}, {U'ぴ', "py"}, {U'ぢ', "dy"}};
    const char32_t small[3] = {U'ゃ', U'ゅ', U'ょ'};
    const char* yv = "auo";
    for (auto& y : yoon)
        for (int i = 0; i < 3; i++) {
            ustr k2 = ustr(1, y.first) + small[i];
            std::string r = std::string(y.second) + yv[i];
            R2K[r] = k2;
            K2R2[k2] = r;
        }
    const std::pair<const char*, const char32_t*> more[] = {
        {"sha", U"しゃ"}, {"shu", U"しゅ"}, {"sho", U"しょ"}, {"cha", U"ちゃ"}, {"chu", U"ちゅ"}, {"cho", U"ちょ"},
        {"ja", U"じゃ"}, {"ju", U"じゅ"}, {"jo", U"じょ"}, {"she", U"しぇ"}, {"che", U"ちぇ"}, {"je", U"じぇ"},
        {"fa", U"ふぁ"}, {"fi", U"ふぃ"}, {"fe", U"ふぇ"}, {"fo", U"ふぉ"}, {"thi", U"てぃ"}, {"dhi", U"でぃ"},
        {"wi", U"うぃ"}, {"we", U"うぇ"}, {"tsa", U"つぁ"}, {"tso", U"つぉ"}, {"va", U"ゔぁ"}, {"vi", U"ゔぃ"},
        {"ve", U"ゔぇ"}, {"vo", U"ゔぉ"}};
    for (auto& x : more) {
        R2K[x.first] = x.second;
        if (ustr(x.second).size() == 2 && !K2R2.count(x.second)) K2R2[x.second] = x.first;   // つぉ -> tso (tsuxo でなく)
    }
}

bool is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// かな 1 字の置き換えの候補 (打った字 c -> 打とうとした字): フリックの打ち間違い、清濁・半濁 (か <-> が、は <-> ば・ぱ)、大小 (や <-> ゃ)。
// フリックは 3 つ (typo_sampler.flick_typos と同じ。ユーザー 2026-10-09): フリックが出ずにあ段 (こ -> か)、考えている向きの
// 隣のキー (こ -> の)、隣のキーでフリックも出ない (こ -> な)。向きの間違い (こ -> き) は手慣れた人はまずしないので直さない
// (ローマ字の母音の打ち間違いは、ローマ字の単位の直しが受け持つ)。
// FLICK_INV は typo_sampler.flick_typos から作った逆引き: 「打った字 + 打とうとした字…」を空白で区切って並べたもの
std::vector<char32_t> kana_alts(char32_t c) {
    static const std::u32string_view FLICK_INV =
        U"あいうえおかがきぎけげただつづとど いきぎ うつづ えけげ おとど かあいえきくけこさしせなぬの がぎぐげござじぜ きいし ぎじ くぬ けえせ げぜ この さかきけしすせそはふほ ざがぎげじずぜぞばぱぶぷぼぽ しき じぎ すふ ずぶぷ せけ ぜげ そほ ぞぼぽ たあうおちつてとなにねまむも だぢづでど ちに つうむ てね とおも なかがくぐこごただちぢてでにぬねのはばぱひびぴへべぺやゆよ にちぢひびぴ ぬくぐゆ ねてでへべぺ のこごよ はさすそなにねひふへほらるろ ばざずぞびぶべぼ ぱぴぷぺぽ ひに ふする ぶず へね ほそろ ぼぞ まただつづとどみむめもや むつづ もとど やなぬのまみめゆよらりれわん ゆぬん よの らはばぱふぶぷほぼぽやりるれろ るふぶぷ ろほぼぽ わやゆよをん んゆ";
    static const char32_t* DAKU[] = {U"かが", U"きぎ", U"くぐ", U"けげ", U"こご", U"さざ", U"しじ", U"すず", U"せぜ", U"そぞ", U"ただ",
                                     U"ちぢ", U"つづ", U"てで", U"とど", U"はばぱ", U"ひびぴ", U"ふぶぷ", U"へべぺ", U"ほぼぽ", U"うゔ"};
    static const char32_t* SMALL[] = {U"あぁ", U"いぃ", U"うぅ", U"えぇ", U"おぉ", U"つっ", U"やゃ", U"ゆゅ", U"よょ", U"わゎ"};
    std::vector<char32_t> out;
    auto add = [&](char32_t x) { if (x != c && std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x); };
    for (size_t i = 0; i < FLICK_INV.size();) {
        size_t e = FLICK_INV.find(U' ', i);
        if (e == std::u32string_view::npos) e = FLICK_INV.size();
        if (FLICK_INV[i] == c)
            for (size_t j = i + 1; j < e; j++) add(FLICK_INV[j]);
        i = e + 1;
    }
    for (const char32_t** g : {DAKU, SMALL}) {
        const size_t n = g == DAKU ? sizeof(DAKU) / sizeof(*DAKU) : sizeof(SMALL) / sizeof(*SMALL);
        for (size_t k = 0; k < n; k++)
            if (std::u32string_view(g[k]).find(c) != std::u32string_view::npos)
                for (const char32_t* x = g[k]; *x; x++) add(*x);
    }
    return out;
}

// 助詞だけ (空も含む) か
bool particles_only(const ustr& s) {
    for (char32_t c : s)
        if (std::u32string_view(U"のにをがとはでへもや").find(c) == std::u32string_view::npos) return false;
    return true;
}

// かな -> ローマ字。かな以外があれば false
bool to_romaji(const ustr& k, std::string& rom) {
    rom.clear();
    for (size_t i = 0; i < k.size();) {
        if (i + 1 < k.size()) {
            auto it = K2R2.find(k.substr(i, 2));
            if (it != K2R2.end()) { rom += it->second; i += 2; continue; }
        }
        if (k[i] == U'ん') { rom += "nn"; i++; continue; }
        if (k[i] == U'っ') {
            // 次の字の子音を重ねる (次が母音・無しなら xtu)
            std::string nx;
            if (i + 1 < k.size()) {
                auto it2 = K2R2.find(i + 2 <= k.size() ? k.substr(i + 1, 2) : ustr());
                if (it2 != K2R2.end()) nx = it2->second;
                else { auto it = K2R.find(k[i + 1]); if (it != K2R.end()) nx = it->second; }
            }
            if (!nx.empty() && !is_vowel(nx[0]) && nx[0] != 'n' && nx[0] != '-') rom += nx[0];
            else rom += "xtu";
            i++;
            continue;
        }
        auto it = K2R.find(k[i]);
        if (it == K2R.end()) return false;
        rom += it->second;
        i++;
    }
    return true;
}

// ローマ字 -> かな (IME と同じ決まりの主なもの)。かなにならない所が残れば false
bool to_kana(const std::string& r, ustr& out) {
    out.clear();
    for (size_t i = 0; i < r.size();) {
        const char c = r[i];
        if (c == 'n') {   // IME と同じ: n + 母音・y は な行、nn + 母音は ん + な行 (konnichiha)、nn は ん、n + 子音は ん
            if (i + 1 < r.size() && r[i + 1] == 'n') {
                const bool nv = i + 2 < r.size() && (is_vowel(r[i + 2]) || r[i + 2] == 'y');
                out += U'ん';
                i += nv ? 1 : 2;
                continue;
            }
            if (i + 1 == r.size() || (!is_vowel(r[i + 1]) && r[i + 1] != 'y')) { out += U'ん'; i += 1; continue; }
        }
        if (i + 1 < r.size() && c == r[i + 1] && !is_vowel(c) && c != 'n' && c != '-') { out += U'っ'; i += 1; continue; }
        bool hit = false;
        for (int L = 3; L >= 1; L--) {
            if (i + size_t(L) > r.size()) continue;
            auto it = R2K.find(r.substr(i, size_t(L)));
            if (it != R2K.end()) { out += it->second; i += size_t(L); hit = true; break; }
        }
        if (!hit) return false;
    }
    return true;
}

// QWERTY で隣のキー (同じ段の左右と、半キーずれた上下の段の 2 つずつ)
std::string neighbors(char c) {
    std::string out;
    for (int rr = 0; rr < 3; rr++) {
        const char* row = ROWS[rr];
        for (int j = 0; row[j]; j++) {
            if (row[j] != c) continue;
            if (j > 0) out += row[j - 1];
            if (row[j + 1]) out += row[j + 1];
            if (rr > 0) {   // 上の段: 同じ位置と右
                const char* o = ROWS[rr - 1];
                int n = int(strlen(o));
                if (j < n) out += o[j];
                if (j + 1 < n) out += o[j + 1];
            }
            if (rr < 2) {   // 下の段: 左と同じ位置
                const char* o = ROWS[rr + 1];
                int n = int(strlen(o));
                if (j - 1 >= 0 && j - 1 < n) out += o[j - 1];
                if (j < n) out += o[j];
            }
        }
    }
    return out;
}

// かなだけの区間 r (読み全体の off 文字目から) に、打ち間違いを直した語の辺を足す
double T_[6] = {0, 0, 0, 0, 0, 0};   // 計測用: 候補づくり・かなへ直す・1 語・2 語・道のコスト・残り (ミリ秒の合計)
inline double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// 読み s の [a, e) の頭の FNV を、e を延ばしながら作る (Lex::maybe_head で「その頭で始まる読みがあるか」を見る)
struct Head {
    const ustr& s;
    int a, e;
    uint64_t h = 1469598103934665603ULL;
    Head(const ustr& s0, int a0) : s(s0), a(a0), e(a0) {}
    bool to(int upto, const Lex& L) {
        while (e < upto) { h ^= uint64_t(s[size_t(e)]); h *= 1099511628211ULL; e++; }
        return L.maybe_head(h ^ (h >> 29));
    }
};

// rom_in: 打った鍵の列 (PC、kkc_set_keys)。あれば読みからローマ字を作らずにこれを使う (読みに英字が残った打ち間違い
// ありgsとう も、鍵の列 arigstou から 1 打戻せば ありがとう になる)
void add_edges(const Lex& L, const ustr& r, int off, const std::vector<Edge>& base, const Ctx& ctx, std::vector<Edge>& out,
               const std::string* rom_in = nullptr) {
    init();
    double t0 = now_ms();
    std::string rom;
    if (rom_in) rom = *rom_in;
    else if (r.size() < 2 || !to_romaji(r, rom)) return;
    if (r.size() < 2) return;
    std::vector<std::string> vars;
    const int R = int(rom.size());
    for (int i = 0; i < R; i++) {
        vars.push_back(rom.substr(0, size_t(i)) + rom.substr(size_t(i + 1)));                         // 余分な 1 打
        for (char c : neighbors(rom[size_t(i)])) {                                                      // 隣のキー
            std::string v = rom;
            v[size_t(i)] = c;
            vars.push_back(v);
        }
        if (i + 1 < R && rom[size_t(i)] != rom[size_t(i + 1)]) {                                        // 入れ替え
            std::string v = rom;
            std::swap(v[size_t(i)], v[size_t(i + 1)]);
            vars.push_back(v);
        }
    }
    // 抜けた 1 打: JWTD でローマ字 1 打の抜けに多い字 (i x a u t s r k n o y h e …) と、隣の字の重ね
    for (int i = 0; i <= R; i++) {
        std::string ins = "aiueonxtsrkhy";
        if (i > 0) ins += rom[size_t(i - 1)];
        for (char c : ins) vars.push_back(rom.substr(0, size_t(i)) + c + rom.substr(size_t(i)));
    }
    // かなの単位 (docs/S7_TYPO.md の分析から): 入れ替え・フリックの打ち間違い (あ段・隣のキー)・清濁・大小 (フリック・かな入力の誤り)、
    // かな 1 字の余分を消す、よく抜けるかな (助詞を除く) を足す
    std::vector<ustr> kvars;
    for (size_t i = 0; i < r.size(); i++) {
        const char32_t c = r[i];
        if (i + 1 < r.size() && c != r[i + 1]) { ustr v = r; std::swap(v[i], v[i + 1]); kvars.push_back(v); }
        for (char32_t a : kana_alts(c)) { ustr v = r; v[i] = a; kvars.push_back(v); }
        if (r.size() > 2) { ustr v = r; v.erase(i, 1); kvars.push_back(v); }
    }
    for (size_t i = 0; i <= r.size(); i++)
        // かな 1 字の抜けは ー・っ・ん だけ (JWTD のかなの抜けの上位は助詞 = 推敲。ほかのかなを丸ごと抜かすのは
        // ローマ字では 2 打の抜けでまれ。っ・ん・ー は打ち飛ばしやすい: がこう -> がっこう)
        for (const char32_t* m = U"ーっん"; *m; m++) {
            ustr v = r;
            v.insert(i, 1, *m);
            kvars.push_back(v);
        }
    double t1 = now_ms(); T_[0] += t1 - t0;
    std::unordered_set<std::string> tried;
    std::unordered_set<ustr> base_keys;   // 打ったとおりの読みの網にある (区間, 表記)
    for (const Edge& ed : base) {
        ustr k = ed.surf;
        k += char32_t(0x110000 + ed.s);
        k += char32_t(0x110000 + ed.e);
        base_keys.insert(k);
    }
    struct Cand { int s, e; ustr surf; int32_t lid, rid, cost; double total; };
    std::unordered_map<ustr, Cand> cand;
    ustr k2;
    std::vector<uint16_t> key;
    const int n1 = int(r.size());
    // 辞書の引きの使い回し (打ち直しの候補どうしで同じ区間を何度も引く)。値は (最初の語, 終わり)、無ければ (0, 0)
    // 鍵は読みの 64 ビットのハッシュ (文字列を作らずに引く。かなの読みどうしの衝突は実際上起きない)
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> memo;
    auto look = [&](const ustr& s, int a0, int a1, uint32_t& a, uint32_t& b) -> bool {
        // まず読みのブルームフィルタ (hash16 と同じハッシュ) で、辞書に無い区間を覚えずに捨てる (ほとんどの区間は無い)
        uint64_t h = 1469598103934665603ULL;
        for (int i = a0; i < a1; i++) h = (h ^ uint64_t(s[size_t(i)])) * 1099511628211ULL;
        h ^= h >> 29;
        if (!L.maybe_hash(h)) return false;
        h ^= uint64_t(a1 - a0) << 56;   // 覚える鍵は長さも混ぜる
        auto it = memo.find(h);
        if (it == memo.end()) {
            std::u16string k16(s.begin() + a0, s.begin() + a1);
            uint32_t x = 0, y = 0;
            if (!L.find(reinterpret_cast<const uint16_t*>(k16.data()), int(k16.size()), x, y)) x = y = 0;
            it = memo.emplace(h, std::make_pair(x, y)).first;
        }
        a = it->second.first;
        b = it->second.second;
        return a != b;
    };
    // 区間の語のうちコストの安い 6 個 (2 語の辺の組み合わせを全部は作らない。品詞違いのコスト 0 の項目を拾える数)
    auto cheapest = [&](uint32_t a, uint32_t b, std::vector<Ent>& out3) {
        out3.clear();
        for (uint32_t j = a; j < b; j++) {
            const Ent x = L.ent(j);
            if (x.cost < ENG_COST) out3.push_back(x);
        }
        if (out3.size() > 6) {
            std::partial_sort(out3.begin(), out3.begin() + 6, out3.end(), [](const Ent& u, const Ent& v) { return u.cost < v.cost; });
            out3.resize(6);
        }
    };
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Ent>> cheap_memo;   // 同じ語の範囲の「安い 6 個」を使い回す
    auto cheapest_m = [&](uint32_t a, uint32_t b) -> const std::vector<Ent>& {
        auto it = cheap_memo.find({a, b});
        if (it == cheap_memo.end()) {
            std::vector<Ent> v;
            cheapest(a, b, v);
            std::stable_sort(v.begin(), v.end(), [](const Ent& u, const Ent& w) { return u.cost < w.cost; });   // [0] が一番安い
            it = cheap_memo.emplace(std::make_pair(a, b), std::move(v)).first;
        }
        return it->second;
    };
    // 道のコスト = 左 (位置・左の品詞で決まる) + 語のコスト + 右 (位置・右の品詞で決まる)。左右を覚えて使い回す
    std::unordered_map<int64_t, double> lmemo, rmemo;
    auto leftc = [&](int s, int lid) -> double {
        const int64_t k = int64_t(s) * 65536 + lid;
        auto it = lmemo.find(k);
        if (it != lmemo.end()) return it->second;
        double v = s == 0 ? double(L.conn(0, lid)) : 1e18;
        for (int j : ctx.by_end[size_t(s)]) v = std::min(v, ctx.F[size_t(j)] + L.conn(base[j].rid, lid));
        lmemo[k] = v;
        return v;
    };
    auto rightc = [&](int e, int rid) -> double {
        const int64_t k = int64_t(e) * 65536 + rid;
        auto it = rmemo.find(k);
        if (it != rmemo.end()) return it->second;
        double v = e == ctx.n ? double(L.conn(rid, 0)) : 1e18;
        for (int kk : ctx.by_start[size_t(e)]) v = std::min(v, L.conn(rid, base[kk].lid) + ctx.B[size_t(kk)]);
        rmemo[k] = v;
        return v;
    };
    auto thru = [&](int s, int e, int lid, int rid, int c) -> double {
        const double l = leftc(s, lid), r2 = rightc(e, rid);
        return (l >= 1e17 || r2 >= 1e17) ? 1e18 : l + c + r2;
    };
    std::unordered_set<ustr> seen_k;
    std::vector<ustr> all;
    for (const std::string& v : vars)
        if (tried.insert(v).second && to_kana(v, k2) && k2 != r && seen_k.insert(k2).second) all.push_back(k2);
    for (auto& v : kvars)
        if (seen_k.insert(v).second) all.push_back(v);
    double t2 = now_ms(); T_[1] += t2 - t1;
    double t_one = 0, t_two = 0;
    auto add = [&](int s, int e, const ustr& sf, int32_t lid, int32_t rid, int32_t c) -> bool {   // 基準を通れば true
        ustr bk = sf;
        bk += char32_t(0x110000 + s + off);
        bk += char32_t(0x110000 + e + off);
        if (base_keys.count(bk)) return false;   // 打ったとおりの読みでも同じ語がある
        ustr ck = sf;
        ck += char32_t(0x110000 + s);
        ck += char32_t(0x110000 + e);
        auto it = cand.find(ck);
        if (it != cand.end() && c >= it->second.cost) return true;
        // 下限 (つながりのコストを 0 とみなす) で先に捨てる
        if (ctx.lmin[size_t(s + off)] + c + ctx.rmin[size_t(e + off)] - 2000 > ctx.best + TYPO_KEEP) return false;
        const double t = thru(s + off, e + off, lid, rid, c);
        if (t > ctx.best + TYPO_KEEP) return false;   // 打ったとおりより十分によくならない直しは足さない
        cand[ck] = {s, e, sf, lid, rid, c, t};
        return true;
    };
    for (const ustr& kv : all) {
        k2 = kv;
        const int n2 = int(k2.size());
        int p = 0;
        while (p < n1 && p < n2 && r[size_t(p)] == k2[size_t(p)]) p++;
        int q = 0;
        while (q < n1 - p && q < n2 - p && r[size_t(n1 - 1 - q)] == k2[size_t(n2 - 1 - q)]) q++;
        // 助詞だけを変える直し (の -> に、が -> を、の を足す・消す) は作らない (推敲であって打ち間違いではない)
        if (particles_only(r.substr(size_t(p), size_t(n1 - q - p))) && particles_only(k2.substr(size_t(p), size_t(n2 - q - p)))) continue;
        bool single_hit = false;
        double ta = now_ms();
        // 直した読みで、変わった所 [p, n2 - q) を覆う語 (2 字以上)
        for (int s2 = std::max(0, p - MAX_WORD_KANA + 1); s2 <= std::min(p, n2 - 1); s2++) {
            const int emin = std::max(n2 - q, s2 + 2), emax = std::min(n2, s2 + MAX_WORD_KANA);
            if (emin > emax) continue;
            Head hd(k2, s2);
            for (int e2 = emin; e2 <= emax; e2++) {
                if (!hd.to(e2, L)) break;   // k2[s2, e2) で始まる読みが無い: 延ばしても語は無い
                const int s = s2, e = e2 - n2 + n1;   // 元の読みの区間 (変わった所より前は同じ、後ろは長さの差だけずれる)
                if (e <= s || e > n1 || e < n1 - q) continue;
                // 区間の下限 (語のコスト 0・つながり 0) で引く前に捨てる
                if (ctx.lmin[size_t(s + off)] + TYPO_COST + ctx.rmin[size_t(e + off)] - 2000 > ctx.best + TYPO_KEEP) continue;
                uint32_t a, b;
                if (!look(k2, s2, e2, a, b)) continue;
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = L.ent(j);
                    if (en.cost >= ENG_COST) continue;
                    // 表記の文字列を作る前に、add と同じ基準 (下限・道のコスト) で捨てる (結果は変わらない)
                    const int32_t c0 = en.cost + TYPO_COST;
                    if (ctx.lmin[size_t(s + off)] + c0 + ctx.rmin[size_t(e + off)] - 2000 > ctx.best + TYPO_KEEP) continue;
                    if (thru(s + off, e + off, int32_t(en.lid), int32_t(en.rid), c0) > ctx.best + TYPO_KEEP) continue;
                    if (add(s, e, from16(L.s_blob + en.soff, en.slen), int32_t(en.lid), int32_t(en.rid), en.cost + TYPO_COST))
                        single_hit = true;
                }
            }
        }
        // 変わった所が 2 語にまたがるとき (つぉあ -> と|される): 前の語 [s2, m) と後ろの語 [m, e2) をつないだ辺。
        // 境目 m は変わった所の中か端。前の語は 1 字でもよい (と・を などの付属語)
        double tb = now_ms(); t_one += tb - ta;
        (void)single_hit;   // 1 語の直しがあっても 2 語の直しも作る (つぉあれる: 土佐 より と|される が正しい)
        const int lo = p, hi = n2 - q;
        for (int m = std::max(lo, 1); m <= std::min(hi, n2 - 1); m++) {
            for (int s2 = std::max(0, m - MAX_WORD_KANA); s2 <= std::min(lo, m - 1); s2++) {
                uint32_t a1, b1;
                if (!look(k2, s2, m, a1, b1)) continue;
                const std::vector<Ent>& c1 = cheapest_m(a1, b1);
                if (c1.empty()) continue;
                Head hd2(k2, m);
                for (int e2 = std::max(hi, m + 1); e2 <= std::min(n2, m + MAX_WORD_KANA); e2++) {
                    if (!hd2.to(e2, L)) break;   // 後ろの語 k2[m, e2) で始まる読みが無い
                    if (m == lo && e2 == hi && lo == hi) continue;
                    uint32_t a2, b2;
                    const int s = s2, e = e2 - n2 + n1;
                    if (e <= s || e > n1 || e < n1 - q) continue;
                    // 区間の下限 (前の語の一番安いコスト・つながり 0) で引く前に捨てる
                    if (ctx.lmin[size_t(s + off)] + c1[0].cost + TYPO_COST + ctx.rmin[size_t(e + off)] - 2000 > ctx.best + TYPO_KEEP) continue;
                    if (!look(k2, m, e2, a2, b2)) continue;
                    const std::vector<Ent>& c2 = cheapest_m(a2, b2);
                    if (c2.empty()) continue;
                    // 前後それぞれ安い 3 語の組のうち、安い 3 組を辺に (1 組だけだと 賭+さ が と+さ(れる) を押しのけた。
                    // どれがよいかは文脈を読むモデルが決める)
                    // 組は「その辺を通る一番よい道」のコスト (前後のつながり込み) で選ぶ。同じ表記の組は一番よいものだけ。
                    // 2 語の間だけのコストで選ぶと、と・さ のように品詞違いのコスト 0 の項目が多い語で、
                    // 後ろの れる につながらない品詞の組を選んでいた
                    struct Pair { double t; int32_t c; const Ent* x; const Ent* y; ustr sf; };
                    std::vector<Pair> ps;
                    const double lr = ctx.lmin[size_t(s + off)] + ctx.rmin[size_t(e + off)] - 2000 + TYPO_COST;
                    for (const Ent& x : c1) {
                        if (lr + x.cost + c2[0].cost > ctx.best + TYPO_KEEP) break;   // 安い順なので、以降も届かない
                        for (const Ent& y : c2) {
                            if (lr + x.cost + y.cost > ctx.best + TYPO_KEEP) break;
                            const int32_t c = x.cost + y.cost + L.conn(x.rid, y.lid) + TYPO_COST;
                            const double t = thru(s + off, e + off, x.lid, y.rid, c);
                            if (t > ctx.best + TYPO_KEEP) continue;
                            ustr sf = from16(L.s_blob + x.soff, x.slen) + from16(L.s_blob + y.soff, y.slen);
                            bool dup = false;
                            for (auto& p2 : ps)
                                if (p2.sf == sf) { dup = true; if (t < p2.t) p2 = {t, c, &x, &y, sf}; break; }
                            if (!dup) ps.push_back({t, c, &x, &y, std::move(sf)});
                        }
                    }
                    std::sort(ps.begin(), ps.end(), [](const Pair& u, const Pair& v) { return u.t < v.t; });
                    for (size_t k = 0; k < std::min<size_t>(ps.size(), 3); k++)
                        add(s, e, ps[k].sf, int32_t(ps[k].x->lid), int32_t(ps[k].y->rid), ps[k].c);
                }
            }
        }
        t_two += now_ms() - tb;
    }
    T_[2] += t_one; T_[3] += t_two;
    // よくある誤読 (げっきょく -> 月極・はじょう -> 破綻): 辞書の、読みの頭に印 U+0001 をつけた項目 (kkc/make_misread_list.py。
    // JMdict の ik と教材の「〜と読んでた」から)。打ち間違いの直しと同じ辺にして、直すか (その読みの別の語か) は採点器が文脈で決める
    {
        ustr mk;
        for (int s = 0; s < n1; s++)
            for (int e = s + 2; e <= std::min(n1, s + MAX_WORD_KANA); e++) {
                if (ctx.lmin[size_t(s + off)] + TYPO_COST + ctx.rmin[size_t(e + off)] - 2000 > ctx.best + TYPO_KEEP) continue;
                mk.assign(1, char32_t(1));
                mk.append(r, size_t(s), size_t(e - s));
                uint32_t a, b;
                if (!look(mk, 0, int(mk.size()), a, b)) continue;
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = L.ent(j);
                    add(s, e, from16(L.s_blob + en.soff, en.slen), int32_t(en.lid), int32_t(en.rid), en.cost + TYPO_COST);
                }
            }
    }
    double t3 = now_ms();
    std::vector<Cand> cs;
    for (auto& kv : cand) cs.push_back(kv.second);
    std::sort(cs.begin(), cs.end(), [](const Cand& x, const Cand& y) {
        if (x.total != y.total) return x.total < y.total;
        if (x.s != y.s) return x.s < y.s;
        if (x.e != y.e) return x.e < y.e;
        return x.surf < y.surf;
    });
    if (int(cs.size()) > TYPO_MAX) cs.resize(TYPO_MAX);
    for (auto& c : cs) out.push_back({c.s + off, c.e + off, c.surf, c.lid, c.rid, c.cost, K_TYPO});
    T_[5] += now_ms() - t3;
}
}  // namespace typo

std::vector<Edge> edges(const Lex& L, const ustr& r, const std::vector<UserWord>* user = nullptr) {
    std::vector<Edge> out;
    int n = int(r.size());
    std::vector<uint16_t> key;
    for (int s = 0; s < n; s++) {
        if (!is_kana(r[s])) { out.push_back({s, s + 1, r.substr(s, 1), L.noun, L.noun, L.pass, K_PASS}); continue; }
        key.clear();
        for (int e = s + 1; e <= std::min(n, s + L.max_word); e++) {
            if (!is_kana(r[e - 1])) break;
            key.push_back(uint16_t(r[e - 1]));   // かなは BMP
            uint32_t a, b;
            if (L.find(key.data(), int(key.size()), a, b)) {
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = L.ent(j);
                    if (en.cost >= ENG_COST) continue;   // 英カタカナ辞書の語は網に入れない
                    ustr sf = from16(L.s_blob + en.soff, en.slen);
                    // 読みが「、」の手前で切れた作品名 (それは → それは、突然、嵐のように…・あした → 明日、キミと手をつなぐよ) は使わない。
                    // 辞書を作るときに題名の読みを途中までしか取れていなかった (約 1000 語。ユーザー 2026-10-17「固有名詞が強すぎる」)
                    if (sf.size() > size_t(e - s) + 1 &&
                        std::any_of(sf.begin(), sf.end(), [](char32_t ch) { return ch == U'、' || ch == U'。' || ch == U'…'; }))
                        continue;
                    // 辞書の数字 (に -> 2) は、後ろに単位が続いて大きい数になる所 (に|せんちょう) では使わない。
                    // そこは数の辺 (2000兆・二千兆) に任せる。使うと採点器が「2千兆」「2千長円」のつなぎを選んでいた
                    if (std::all_of(sf.begin(), sf.end(), [](char32_t ch) { return ch >= U'0' && ch <= U'9'; }) &&
                        number_continues(r, size_t(s), size_t(e)))
                        continue;
                    const int32_t c = has_latin_word(sf) ? en.cost - LATIN_WORD_BONUS : en.cost;
                    out.push_back({s, e, std::move(sf), en.lid, en.rid, c, K_DICT});
                }
            }
            if (e - s == 1) {
                out.push_back({s, e, r.substr(s, 1), L.noun, L.noun, L.oov, K_KANA});
                if (r[s] != 0x30FC) out.push_back({s, e, ustr(1, kata(r[s])), L.noun, L.noun, L.kata + L.kata_per, K_KATA});
            } else if (e - s <= 12) {
                out.push_back({s, e, to_kata(r.substr(s, e - s)), L.noun, L.noun, L.kata + L.kata_per * (e - s), K_KATA});
            }
        }
    }
    // 英字のまま打った語 (英数字キーボードで iphone・github): 英数字の続きを小文字にして辞書を引き、正しい書き方 (iPhone・GitHub) の辺に。
    // 辞書には make_case_list.py が読み = 小文字の形で入れてある
    for (int s = 0; s < n;) {
        auto al = [](char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9'); };
        if (!al(r[s])) { s++; continue; }
        int e = s;
        bool letter = false;
        while (e < n && (al(r[e]) || ((r[e] == U'.' || r[e] == U'-' || r[e] == U'&' || r[e] == U'+') && e + 1 < n && al(r[e + 1])))) {
            letter = letter || (r[e] >= U'A' && (r[e] <= U'Z' || r[e] >= U'a'));
            e++;
        }
        if (letter && e - s >= 2 && e - s <= 30) {
            key.clear();
            for (int i = s; i < e; i++) {
                const char32_t c = r[i];
                if (c != U' ') key.push_back(uint16_t(c >= U'A' && c <= U'Z' ? c + 32 : c));
            }
            uint32_t a, b;
            if (L.find(key.data(), int(key.size()), a, b))
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = L.ent(j);
                    if (en.cost >= ENG_COST) continue;
                    out.push_back({s, e, from16(L.s_blob + en.soff, en.slen), en.lid, en.rid, en.cost, K_DICT});
                }
        }
        s = e;
    }
    // ユーザー辞書: 読みが現れる所すべてに辞書の語として置く
    if (user) {
        for (const UserWord& w : *user) {
            if (w.r.empty()) continue;
            // 短縮よみ (よ → よろしくお願いします) は、その読みだけを打ったときだけ。文の途中・打っている途中の変換に混ぜない
            // (MS-IME と同じ。ブログの報告 2026-10-21: 「よろこんで」が「よろしくお願いします。ろこんで」に)
            if (w.whole) {
                if (r == w.r) out.push_back({0, n, w.s, w.lid, w.rid, w.cost, K_DICT});
                continue;
            }
            for (size_t i = r.find(w.r); i != ustr::npos; i = r.find(w.r, i + 1))
                out.push_back({int(i), int(i + w.r.size()), w.s, w.lid, w.rid, w.cost, K_DICT});
        }
    }
    // 数字のまま打った数 (数字パッドの 2000) にも、カンマ・漢数字・全角・単位まじりの辺 (2,000・二千・２０００・2万5000)。
    // 打ったとおりの数字は 'pass' の辺で出る
    for (int s = 0; s < n;) {
        if (r[s] < U'0' || r[s] > U'9') { s++; continue; }
        int e = s;
        while (e < n && r[e] >= U'0' && r[e] <= U'9') e++;
        if (e - s <= 16 && (r[s] != U'0' || e - s == 1)) {
            int64_t v = 0;
            for (int i = s; i < e; i++) v = v * 10 + int64_t(r[i] - U'0');
            // 文の中では打ったままの数字を先に (2000えん -> 2000円)、カンマ・単位まじりを次に。
            // 漢数字・全角は読み全体が数字のときだけ候補に足す (下の kkc_convert の差し込み)
            const int c = 3000;
            out.push_back({s, e, r.substr(size_t(s), size_t(e - s)), L.num_arabic, L.num_arabic, c, K_NUM});
            // 年 (2026ねん) にはカンマを付けない
            if (v >= 1000 && !(e - s == 4 && starts_with(r, size_t(e), U"ねん")))
                out.push_back({s, e, with_commas(r.substr(size_t(s), size_t(e - s))), L.num_arabic, L.num_arabic, c + 600, K_NUM});
            if (v >= 10000) {
                const ustr m = unit_mixed(v, false), mc = unit_mixed(v, true);
                out.push_back({s, e, m, L.num_arabic, L.num_arabic, c + 700, K_NUM});
                if (mc != m) out.push_back({s, e, mc, L.num_arabic, L.num_arabic, c + 800, K_NUM});
            }
        }
        s = e;
    }
    std::vector<Edge> num;
    number_edges(L, r, num);
    special_edges(L, r, num);
    abbr_edges(L, r, num);
    for (auto& ed : num) {
        bool ok = true;
        for (int i = ed.s; i < ed.e; i++) if (!is_kana(r[i])) { ok = false; break; }
        if (ok) out.push_back(std::move(ed));
    }
    if (L.typo) {   // かなだけの区間ごとに、打ち間違いを直した語の辺
        typo::Ctx ctx;
        typo::build_ctx(L, out, n, ctx);
        std::vector<Edge> ty;
        for (int s = 0; s < n;) {
            if (!is_kana(r[s])) { s++; continue; }
            int e = s;
            while (e < n && is_kana(r[e])) e++;
            // 長い区間は 12 字の窓 (6 字ずつ重ねる) に分ける (打ち直しの候補は字数の 2 乗で増えるので)
            const int W = 12, STEP = 6;
            if (e - s <= W) typo::add_edges(L, r.substr(size_t(s), size_t(e - s)), s, out, ctx, ty);
            else
                for (int w = s; w < e; w += STEP) {
                    const int we = std::min(e, w + W);
                    typo::add_edges(L, r.substr(size_t(w), size_t(we - w)), w, out, ctx, ty);
                    if (we == e) break;
                }
            s = e;
        }
        // 窓の重なりで同じ辺ができたら 1 つに
        std::sort(ty.begin(), ty.end(), [](const Edge& x, const Edge& y) {
            if (x.s != y.s) return x.s < y.s;
            if (x.e != y.e) return x.e < y.e;
            if (x.surf != y.surf) return x.surf < y.surf;
            return x.cost < y.cost;
        });
        ty.erase(std::unique(ty.begin(), ty.end(), [](const Edge& x, const Edge& y) { return x.s == y.s && x.e == y.e && x.surf == y.surf; }), ty.end());
        for (auto& ed : ty) out.push_back(std::move(ed));
    }
    return out;
}

// 英字の語 (略語でないもの) を含むか。略語 = 英大文字・数字だけで 5 字までの並び (GDP・IPO)
bool has_latin_word(const ustr& s) {
    auto is_lat = [](char32_t c) { return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z'); };
    for (size_t i = 0; i < s.size();) {
        if (!is_lat(s[i])) { i++; continue; }
        size_t j = i;
        bool lower = false;
        while (j < s.size() && (is_lat(s[j]) || (s[j] >= U'0' && s[j] <= U'9'))) { lower |= s[j] >= U'a' && s[j] <= U'z'; j++; }
        if (lower || j - i > 5) return true;
        i = j;
    }
    return false;
}

// ---------------------------------------------------------------- 確定済みのカタカナ語の続き
// 左の文脈がカタカナで終わっていて (確定したところで止まった「フリッ」「スマー」)、それに読みの頭をカタカナにして足すと辞書の
// カタカナ語になる (フリッ+く = フリック、スマー+とふぉん = スマートフォン) なら、読みの頭をカタカナにした辺を安く足す。
// 教材は語の切れ目でしか例を切っていないので、モデルは語の途中で切れた左の文脈を知らない (ユーザー 2026-10-20)
constexpr int32_t KATA_CONT_COST = 0;
void kata_continuation_edges(const Lex& L, const ustr& ctx, const ustr& r, std::vector<Edge>& out) {
    auto is_kata = [](char32_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; };
    size_t t = 0;   // 左の文脈の終わりのカタカナの字数
    while (t < ctx.size() && t < 12 && is_kata(ctx[ctx.size() - 1 - t])) t++;
    if (t < 2) return;
    const int n = int(r.size());
    int best = 0;
    for (int j = 1; j <= std::min(n, 12); j++) {
        if (!((r[j - 1] >= 0x3041 && r[j - 1] <= 0x3096) || r[j - 1] == 0x30FC)) break;
        const ustr tail_r = to_kata(r.substr(0, size_t(j)));
        for (size_t k = 2; k <= t; k++) {   // 左のカタカナの後ろ k 字 + 読みの頭 j 字 = 辞書のカタカナ語か
            const ustr word = ctx.substr(ctx.size() - k) + tail_r;
            std::vector<uint16_t> key;
            for (char32_t c : word) key.push_back(uint16_t(c >= 0x30A1 && c <= 0x30F6 ? c - 0x60 : c));
            uint32_t a, b;
            if (!L.find(key.data(), int(key.size()), a, b)) continue;
            bool hit = false;
            for (uint32_t q = a; q < b && !hit; q++) {
                const Ent en = L.ent(q);
                hit = en.cost < ENG_COST && from16(L.s_blob + en.soff, en.slen) == word;
            }
            if (hit) { best = j; break; }
        }
    }
    if (best > 0) out.push_back({0, best, to_kata(r.substr(0, size_t(best))), L.noun, L.noun, KATA_CONT_COST, K_KATA});
}

// 確定済みの漢字の語の続き: 左の文脈の終わり (直滑) で始まり、読みが今の読みの頭 (こう) で終わる辞書の語 (直滑降 ちょっかっこう) があれば、
// 残りの表記 (降) の辺を足す。左の文脈が語の切れ目で終わっている (東京 + とおおさか → 東京都) こともあるので、どちらにするかは採点器が文脈で決める
constexpr int32_t KANJI_CONT_COST = 3000;   // 続きの辺のコストの上限 (珍しい語 直滑降 は辞書のコストが高く、そのままでは出なかった)
void kanji_continuation_edges(const Lex& L, const ustr& ctx, const ustr& r, std::vector<Edge>& out) {
    auto is_kanji = [](char32_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005; };
    if (ctx.empty() || !is_kanji(ctx.back()) || r.empty()) return;
    if (!L.head_built) {   // 表記の頭の字の索引を作る (よく使う語だけ: 辞書のコスト 7000 未満)
        L.head_built = true;
        uint16_t rb[Lex::MAXR];
        for (int i = 0; i < L.nread; i++) {
            if (L.ver != 1) L.next_reading(i, rb);
            for (uint32_t j = L.e_first[i]; j < L.e_first[i + 1]; j++) {
                const Ent en = L.ent(j);
                if (en.slen < 1 || en.cost >= 7000) continue;   // 1 字の語も入れる (左の文脈の終わりが完成した語かを見るため)
                const uint16_t* q = L.s_blob + en.soff;
                bool kan = false;
                for (int x = 0; x < en.slen && !kan; x++) kan = is_kanji(q[x]);
                if (kan) L.head_idx[char32_t(q[0])].push_back({j, uint32_t(i)});
            }
        }
    }
    const int n = int(r.size());
    int best_j = 0;
    Edge best{};
    uint16_t rb[Lex::MAXR];
    for (size_t k = 1; k <= std::min<size_t>(4, ctx.size()); k++) {   // 左の文脈の後ろ k 字が語の頭
        const ustr P = ctx.substr(ctx.size() - k);
        bool kan = false;
        for (char32_t c : P) kan = kan || is_kanji(c);
        if (!kan) continue;
        auto it = L.head_idx.find(P[0]);
        if (it == L.head_idx.end()) continue;
        // 左の文脈の終わりがそれだけでよく使う語 (東京・会社) なら、語の切れ目で終わっているとみて続きを探さない (dev が 0.5 点落ちた)
        bool complete = false;
        for (const auto& [j, ri] : it->second) {
            const Ent en = L.ent(j);
            if (en.slen != int(k) || en.cost >= 6000) continue;
            const uint16_t* q = L.s_blob + en.soff;
            bool same = true;
            for (size_t x = 0; x < k && same; x++) same = q[x] == P[x];
            if (same) { complete = true; break; }
        }
        if (complete) continue;
        for (const auto& [j, ri] : it->second) {
            const Ent en = L.ent(j);
            if (en.slen <= int(k)) continue;
            const uint16_t* q = L.s_blob + en.soff;
            bool head = true;
            for (size_t x = 0; x < k && head; x++) head = q[x] == P[x];
            if (!head) continue;
            int rn = 0;
            const uint16_t* rd = L.reading(int(ri), rn, rb);
            // 読みの終わりが今の読みの頭 jj 字と同じ (前の k 字に読みが 1 字以上は残る)
            for (int jj = std::min(n, rn - int(k)); jj >= 1; jj--) {
                bool tail = true;
                for (int x = 0; x < jj && tail; x++) tail = rd[rn - jj + x] == r[size_t(x)];
                if (!tail) continue;
                if (jj > best_j || (jj == best_j && std::min(en.cost, KANJI_CONT_COST) < best.cost))
                    best_j = jj, best = {0, jj, from16(q + k, size_t(en.slen) - k), int32_t(en.lid), int32_t(en.rid), std::min(en.cost, KANJI_CONT_COST), K_DICT};
                break;
            }
        }
    }
    if (best_j > 0) out.push_back(best);
}

// ---------------------------------------------------------------- 英字の辺 (ローマ字で打った英単語。K_LATIN)
// 打った鍵の列 (kkc_set_keys) の、かなの区間 [s, e) に対応する鍵の区間 [span_start(s), span_end[e-1]) が英字だけのとき、
// 小文字にして英単語の一覧 (Lex::latin_words) を引く。当たれば正しい大文字小文字の表記の辺を足す。かなにするか
// 英字のままにするかは採点器 (モデル) が文脈で決める (K_TYPO と同じ考え方)。コストに LATIN_BIAS を上乗せして日本語寄りにする
// user_latin: ユーザー辞書の英字の語 (小文字・空白なしの綴り -> 語。ShuntiLettuce)。空白を挟んで打った区間 (shunti lettuce) も当てる
// ローマ字としてありえない鍵の並びがあるか (大文字、または js・ml・ck のような子音の続き)。かなだけに読める区間でも、こういう並びなら
// 英字を打ったとみなして打ったままの辺を作る (json・yaml・AzuYahi が網に入らなかった。2026-10-24)。普通のローマ字には出ないので網は重くならない
bool unromaji(const uint16_t* k, int n) {
    auto cons = [](uint16_t c) { return c >= u'a' && c <= u'z' && c != u'a' && c != u'i' && c != u'u' && c != u'e' && c != u'o'; };
    for (int i = 0; i < n; i++) {
        if (k[i] >= u'A' && k[i] <= u'Z') return true;
        if (i + 1 < n && cons(k[i]) && cons(k[i + 1])) {
            const uint16_t a = k[i], b = k[i + 1];
            if (a == b || a == u'n' || b == u'y' || (b == u'h' && (a == u's' || a == u'c' || a == u'd' || a == u't' || a == u'w')) ||
                (a == u't' && b == u's') || (b == u'w' && (a == u'k' || a == u'g')) || (a == u'x' || a == u'l'))
                continue;   // ローマ字にある並び (っ・ん・きゃ・しゃ・つ・くゎ・小さい字)
            return true;
        }
    }
    return false;
}

// left_latin・right_latin: 読みの前 (確定した左の文脈の終わり)・後ろ (後ろの文脈の頭) が英字か空白 = 英語の文の続き
void latin_edges(const Lex& L, const std::vector<uint16_t>& keys, const std::vector<int32_t>& span_end, int n,
                 const ustr& r, const std::unordered_map<std::string, LatinWord>& user_latin, std::vector<Edge>& out,
                 bool left_latin = false, bool right_latin = false) {
    const int nkeys = int(keys.size());
    // かなの区間 [s, e) に、鍵の区間 [ks, ke) が英字だけの英単語に当たる辺を足す
    auto add = [&](int ks, int ke, int s, int e) {
        if (ke - ks < 2) {   // 1 字の語は英語の文の中の i・a だけ (前後が空白か英語の文脈。学習の網と同じ)
            if (ke - ks != 1) return;
            const uint16_t c = keys[size_t(ks)];
            const bool sp = (s > 0 ? r[size_t(s - 1)] == U' ' : left_latin) || (e < n ? r[size_t(e)] == U' ' : right_latin);
            // 大文字 1 字は Shift で打った = その字のつもり (リカルド・A・、東京D)
            if ((sp && (c == u'i' || c == u'a' || c == u'I' || c == u'A')) || (c >= u'A' && c <= u'Z'))
                out.push_back({s, e, ustr(1, char32_t(c)), L.noun, L.noun, LATIN_RAW_COST, K_LATIN});
            return;
        }
        std::string lower;
        lower.reserve(size_t(ke - ks));
        bool spaced = false;
        for (int i = ks; i < ke; i++) {
            const uint16_t c = keys[size_t(i)];
            if (c >= u'a' && c <= u'z') lower.push_back(char(c));
            else if (c >= u'A' && c <= u'Z') lower.push_back(char(c + 32));
            else if (c == u' ' && i > ks && i + 1 < ke) spaced = true;   // 間の空白はユーザーの英字の語を引くときだけ飛ばす
            else return;   // 英字だけの区間だけ (記号は見ない)
        }
        if (lower.size() < 2) return;
        auto ut = user_latin.find(lower);
        if (ut != user_latin.end()) {   // ユーザー辞書の英字の語 (いちばん強い)
            out.push_back({s, e, ut->second.surf, ut->second.lid, ut->second.rid, ut->second.cost, K_LATIN});
            return;
        }
        if (spaced) return;
        // 読みが全部かなの区間 (おぺない) には、2 字目以降に大文字があり小文字も混ざる語 (OpenAI・YouTube) だけ。
        // 全部大文字 (ZETTON・AKASHI)・頭だけ大文字 (Radon)・小文字 (same) は日本語の読みとぶつかって誤爆した (2026-10-23)
        // ただし空白・英字と隣り合う区間 (空白で区切って打った英語の文: hello my name) は英語の続きなので、辞書の英字の語を全部使う
        const auto latin_side = [&](int i) {
            if (i < 0) return left_latin;    // 読みの頭: 左の文脈が英語なら英語の文の続き (英語の文を区切って打ったとき)
            if (i >= n) return right_latin;
            return r[size_t(i)] == U' ' || (r[size_t(i)] < 0x80 && std::isalpha(int(r[size_t(i)])));
        };
        const bool english = latin_side(s - 1) || latin_side(e);
        const bool kana_span = std::all_of(r.begin() + s, r.begin() + e, is_kana) && !english;
        auto camel = [](const ustr& w) {
            bool up = false, low = false;
            for (size_t i = 0; i < w.size(); i++) {
                if (w[i] >= U'A' && w[i] <= U'Z') { if (i > 0) up = true; }
                else if (w[i] >= U'a' && w[i] <= U'z') low = true;
            }
            return up && low;
        };
        auto it = L.latin_words.find(lower);
        // Shift で大文字を打った区間は、打った書き方のつもり (辞書の書き方で止めず、打ったままの辺に回す。（Luke）
        const bool upper = std::any_of(keys.begin() + ks, keys.begin() + ke, [](uint16_t c) { return c >= u'A' && c <= u'Z'; });
        if (it != L.latin_words.end() && kana_span && !camel(it->second.surf)) {
            if (!upper) return;
            it = L.latin_words.end();
        }
        if (it != L.latin_words.end()) {
            ustr surf = it->second.surf;
            // 英語の文の中 (空白・英字と隣り合う) では、大文字小文字は打った通り (the・you・it。辞書の The・YOU・IT にしない)。
            // 2 字目以降に大文字があり小文字も混ざる書き方 (OpenAI・iPhone) だけは辞書の表記。日本語の文の中では辞書の表記 (OS・IT)。
            // ただし普通の英単語 (SCOWL の語、辞書の読み U+0002 + 綴り。will・call) は、辞書に変わった書き方 (WiLL) があっても打った通り
            auto common = [&] {
                std::u16string k16(1, u'');
                for (char ch : lower) k16.push_back(char16_t(ch));
                uint32_t a0, b0;
                return L.find(reinterpret_cast<const uint16_t*>(k16.data()), int(k16.size()), a0, b0);
            };
            if (upper || (english && (!camel(surf) || common()))) {   // Shift で大文字を打ったら打った書き方
                surf.clear();
                for (int i = ks; i < ke; i++) surf.push_back(char32_t(keys[size_t(i)]));
            }
            out.push_back({s, e, surf, it->second.lid, it->second.rid, it->second.cost + LATIN_BIAS, K_LATIN});
            return;
        }
        // 辞書に無い英字の並び (略語・新しい語): 打ったまま (大文字小文字も打った通り) を、辞書の語より高い点で (名詞として)。
        // 大文字・頭だけ大文字も出すと、free shipping → Free Shipping のように勝手に大文字にした (2026-10-18 英字まじりベンチ)。
        // 大文字にしたい人は Shift で打つ
        // かなだけの区間には作らない (M5 で鍵の列を常に渡すと、あらゆる区間に打ったままの辺ができて網が数倍に重くなった。2026-10-23)。
        // 読みに英字が残った所 (辞書に無い英字を打った: minecraft) にだけ
        if (std::all_of(r.begin() + s, r.begin() + e, is_kana) && !unromaji(keys.data() + ks, ke - ks)) return;
        ustr typed;
        for (int i = ks; i < ke; i++) typed.push_back(char32_t(keys[size_t(i)]));
        out.push_back({s, e, typed, L.noun, L.noun, LATIN_RAW_COST + LATIN_BIAS, K_LATIN});
    };
    for (int s = 0; s < n; s++) {
        const int ks = s == 0 ? 0 : span_end[size_t(s - 1)];
        for (int e = s + 1; e <= n; e++) {
            const int ke = span_end[size_t(e - 1)];
            add(ks, ke, s, e);
            // 読みの最後の区間は、読みにならなかった鍵 (余り、語末の子音) も含めて試す
            if (e == n && nkeys > ke) add(ks, nkeys, s, e);
        }
    }
}

// ---------------------------------------------------------------- 略語 (あいえむいー -> IME)
// アルファベットの読み。辞書の 1 字の英字 (A〜Z) の読みから取り出した (kkc/latin/README の手順)
const std::pair<const char16_t*, char32_t> LETTERS[] = {
    {u"えー", U'A'}, {u"えい", U'A'}, {u"びー", U'B'}, {u"びい", U'B'}, {u"びぃ", U'B'}, {u"しー", U'C'}, {u"でぃー", U'D'}, {u"でー", U'D'},
    {u"いー", U'E'}, {u"えふ", U'F'}, {u"じー", U'G'}, {u"えいち", U'H'}, {u"えっち", U'H'}, {u"あい", U'I'}, {u"じぇー", U'J'}, {u"じぇい", U'J'},
    {u"けー", U'K'}, {u"けい", U'K'}, {u"える", U'L'}, {u"えむ", U'M'}, {u"えぬ", U'N'}, {u"おー", U'O'}, {u"ぴー", U'P'}, {u"きゅー", U'Q'},
    {u"きゅう", U'Q'}, {u"あーる", U'R'}, {u"えす", U'S'}, {u"てぃー", U'T'}, {u"てぃ", U'T'}, {u"てー", U'T'}, {u"ゆー", U'U'}, {u"ぶい", U'V'},
    {u"ゔぃ", U'V'}, {u"ぶぃ", U'V'}, {u"だぶりゅー", U'W'}, {u"だぶる", U'W'}, {u"だぶるー", U'W'}, {u"えっくす", U'X'}, {u"わい", U'Y'}, {u"ぜっと", U'Z'},
};
constexpr int ABBR_MIN = 3;      // この数以上続いたら略語の候補にする (2 字は「いい」「ええ」などと紛れる)
constexpr int ABBR_MAX = 8;
constexpr int32_t ABBR_COST = 7120;   // 辞書の 3〜5 字の略語のコストの上位 4 分の 1 (中央値 5786)。辞書にある略語より少し出にくく

// r[s..] から続くアルファベットの読みを全部たどり、ABBR_MIN 字以上なら (終わり, 英字) を out に
void abbr_spans(const ustr& r, int s, ustr& cur, std::vector<std::pair<int, ustr>>& out) {
    if (int(cur.size()) >= ABBR_MIN) out.push_back({s, cur});
    if (int(cur.size()) >= ABBR_MAX) return;
    for (const auto& lt : LETTERS) {
        int k = 0;
        while (lt.first[k] && s + k < int(r.size()) && char32_t(lt.first[k]) == r[size_t(s + k)]) k++;
        if (lt.first[k]) continue;
        cur.push_back(lt.second);
        abbr_spans(r, s + k, cur, out);
        cur.pop_back();
    }
}

void abbr_edges(const Lex& L, const ustr& r, std::vector<Edge>& out) {
    if (L.abbr_lid < 0) return;
    std::vector<std::pair<int, ustr>> sp;
    ustr cur;
    for (int s = 0; s < int(r.size()); s++) {
        sp.clear();
        abbr_spans(r, s, cur, sp);
        for (auto& p : sp) out.push_back({s, p.first, p.second, L.abbr_lid, L.abbr_rid, ABBR_COST, K_DICT});
    }
}

// 読み全体がアルファベットの読み ABBR_MIN 字以上だけでできていれば、その英字 (いちばん字の少ない読み方)。無ければ空
ustr whole_abbr(const ustr& r) {
    std::vector<std::pair<int, ustr>> sp;
    ustr cur, best;
    abbr_spans(r, 0, cur, sp);
    for (auto& p : sp)
        if (p.first == int(r.size()) && (best.empty() || p.second.size() < best.size())) best = p.second;
    return best;
}

// ---------------------------------------------------------------- 数式 (えっくすのにじょうぷらすいち -> x²+1)
// 読み全体が式として読めるときだけ、式の候補を 2 位までに置く (AI の 1 位は動かさない)。記号の対応は事実だけ (例文は使わない)
namespace mathx {
const std::pair<const char32_t*, const char32_t*> VARS[] = {
    {U"えっくす", U"x"}, {U"わい", U"y"}, {U"ぜっと", U"z"}, {U"えー", U"a"}, {U"びー", U"b"}, {U"しー", U"c"}, {U"えぬ", U"n"},
    {U"えむ", U"m"}, {U"けー", U"k"}, {U"てぃー", U"t"}, {U"ぴー", U"p"}, {U"きゅー", U"q"}, {U"あーる", U"r"}, {U"ぱい", U"π"},
    {U"しーた", U"θ"}, {U"あるふぁ", U"α"}, {U"べーた", U"β"}};
const std::pair<const char32_t*, const char32_t*> OPS[] = {
    {U"ぷらす", U"+"}, {U"たす", U"+"}, {U"まいなす", U"-"}, {U"ひく", U"-"}, {U"かける", U"×"}, {U"わる", U"÷"}, {U"いこーる", U"="},
    {U"のっといこーる", U"≠"}, {U"だいなりいこーる", U"≧"}, {U"しょうなりいこーる", U"≦"}, {U"だいなり", U">"}, {U"しょうなり", U"<"}};
const char32_t* SUP = U"⁰¹²³⁴⁵⁶⁷⁸⁹";

bool at(const ustr& r, size_t i, const char32_t* k) {
    size_t n = std::char_traits<char32_t>::length(k);
    return r.compare(i, n, k) == 0;
}
size_t len(const char32_t* k) { return std::char_traits<char32_t>::length(k); }

// 数: いちばん長く読める所まで (式の中では 1 字の数も)
void nums(const ustr& r, size_t i, std::vector<std::pair<size_t, ustr>>& out) {
    for (size_t e = std::min(r.size(), i + 12); e > i; e--) {
        int64_t v;
        if (parse_num(r.substr(i, e - i), v)) { out.push_back({e, digits(v, false)}); return; }
    }
}

// 項 = [係数の数] 変数 | 数 | るーと 項 | 数 ぶんの 数 (分数)、の後に [の] にじょう / さんじょう / N じょう
void terms(const ustr& r, size_t i, std::vector<std::pair<size_t, ustr>>& out, int depth = 0) {
    std::vector<std::pair<size_t, ustr>> base;
    std::vector<std::pair<size_t, ustr>> n0;
    nums(r, i, n0);
    for (auto& a : n0) {
        base.push_back(a);
        for (auto& v : VARS) if (at(r, a.first, v.first)) base.push_back({a.first + len(v.first), a.second + v.second});   // 2x
        if (at(r, a.first, U"ぶんの")) {   // にぶんのいち -> 1/2
            std::vector<std::pair<size_t, ustr>> n1;
            nums(r, a.first + 3, n1);
            for (auto& b : n1) base.push_back({b.first, b.second + U"/" + a.second});
        }
    }
    for (auto& v : VARS) if (at(r, i, v.first)) base.push_back({i + len(v.first), v.second});
    // 変数を並べた掛け算 (ぱいあーる -> πr、えっくすわい -> xy、にえっくすわい -> 2xy)。3 つまで
    for (size_t k = 0, n0b = base.size(); k < n0b; k++) {
        std::vector<std::pair<size_t, ustr>> cur = {base[k]};
        for (int rep = 0; rep < 2; rep++) {
            std::vector<std::pair<size_t, ustr>> nxt;
            for (auto& c : cur)
                if (!c.second.empty() && (c.second.back() < U'0' || c.second.back() > U'9'))
                    for (auto& v : VARS) if (at(r, c.first, v.first)) nxt.push_back({c.first + len(v.first), c.second + v.second});
            for (auto& x : nxt) base.push_back(x);
            cur.swap(nxt);
        }
    }
    // ルート: るーと〜 / へいほうこん〜 (前に付く)、〜のへいほうこん (後ろに付く: にのへいほうこん -> √2)
    for (const char32_t* rt : {U"るーと", U"へいほうこん"}) {
        if (depth < 2 && at(r, i, rt)) {
            std::vector<std::pair<size_t, ustr>> t;
            terms(r, i + len(rt), t, depth + 1);
            for (auto& x : t) base.push_back({x.first, U"√" + x.second});
        }
    }
    for (size_t k = 0, nb = base.size(); k < nb; k++)
        if (at(r, base[k].first, U"のへいほうこん")) base.push_back({base[k].first + 7, U"√" + base[k].second});
    for (auto& b : base) {
        out.push_back(b);
        for (size_t j : {b.first, b.first + (at(r, b.first, U"の") ? 1 : 0)}) {
            // べきの読みの揺れ: じじょう (自乗) = 2 乗 (りっぽう は 3 乗の意味では打たないので入れない)
            if (at(r, j, U"じじょう")) out.push_back({j + 4, b.second + U"²"});
            std::vector<std::pair<size_t, ustr>> ex;
            nums(r, j, ex);
            for (auto& e : ex) {
                if (!at(r, e.first, U"じょう")) continue;
                ustr sup;
                for (char32_t c : e.second) sup.push_back(SUP[c - U'0']);
                out.push_back({e.first + 3, b.second + sup});
            }
        }
    }
}

// 読み全体が「項 (演算子 項)*」で、演算子・べき・分数・ルートのどれかを含むなら、その式 (いちばん短い書き方)。無ければ空
ustr whole(const ustr& r) {
    ustr best;
    std::function<void(size_t, ustr, bool)> go = [&](size_t i, ustr acc, bool mathy) {
        std::vector<std::pair<size_t, ustr>> t;
        terms(r, i, t);
        for (auto& x : t) {
            const ustr a = acc + x.second;
            const bool m = mathy || x.second.find_first_of(U"/√⁰¹²³⁴⁵⁶⁷⁸⁹") != ustr::npos;
            if (x.first == r.size()) {
                if (m && (best.empty() || a.size() < best.size())) best = a;
                continue;
            }
            for (auto& o : OPS) if (at(r, x.first, o.first)) go(x.first + len(o.first), a + o.second, true);
        }
    };
    go(0, ustr(), false);
    // 数だけの式 (いちたすいち -> 1+1) は残す。数の読み (ご、にぶんのいち) だけで演算子の無いものは分数・べきのときだけ
    return best;
}

// 読み 1 つの記号 (文字どおりの対応だけ)。AI の候補の上位 5 に無ければ 5 位に足す
const std::pair<const char32_t*, const char32_t*> SYMBOLS[] = {
    {U"かける", U"×"}, {U"わる", U"÷"}, {U"たす", U"+"}, {U"にじょう", U"²"}, {U"さんじょう", U"³"}, {U"じじょう", U"²"},
    {U"へいほうこん", U"√"}};
}  // namespace mathx

ustr best_path(const Lex& L, const std::vector<Edge>& E, int n) {
    int M = int(E.size());
    if (!M) return ustr();
    std::vector<int> order(M);
    for (int i = 0; i < M; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return E[a].s < E[b].s; });
    std::vector<int> ptr(n + 2, 0), by_end(M);
    for (auto& e : E) ptr[e.e + 1]++;
    for (int p = 0; p <= n; p++) ptr[p + 1] += ptr[p];
    std::vector<int> fill(ptr);
    for (int i = 0; i < M; i++) by_end[fill[E[i].e]++] = i;
    std::vector<double> best(M, 1e18);
    std::vector<int> prev(M, -1);
    for (int t = 0; t < M; t++) {
        int i = order[t], s = E[i].s;
        if (s == 0) { best[i] = double(L.conn(0, E[i].lid)) + E[i].cost; continue; }
        for (int q = ptr[s]; q < ptr[s + 1]; q++) {
            int j = by_end[q];
            if (best[j] >= 1e17) continue;
            double v = best[j] + L.conn(E[j].rid, E[i].lid) + E[i].cost;
            if (v < best[i]) { best[i] = v; prev[i] = j; }
        }
    }
    int bi = -1; double bv = 1e18;
    for (int q = ptr[n]; q < ptr[n + 1]; q++) {
        int j = by_end[q];
        double v = best[j] + L.conn(E[j].rid, 0);
        if (v < bv) { bv = v; bi = j; }
    }
    std::vector<int> path;
    while (bi >= 0) { path.push_back(bi); bi = prev[bi]; }
    ustr out;
    for (auto it = path.rbegin(); it != path.rend(); ++it) out += E[*it].surf;
    return out;
}

// ---------------------------------------------------------------- 採点器
struct LayerW { const float *ln1w, *ln1b, *inw, *inb, *outw, *outb, *ln2w, *ln2b, *l1w, *l1b, *l2w, *l2b; };

// 文字ごとの表 (V 行 × d)。版 2・3 は fp32、版 4 は行ごとの倍率つきの int8 (モデルの 3/4 がこの表なので、ファイルが半分以下になる。
// 1 行ずつ引くだけの表なので、計算の側は変えずに、引くときに戻す)
struct Tab {
    const float* f = nullptr;
    const int8_t* q = nullptr;
    const float* s = nullptr;
    int d = 0;
    // i 行目。int8 のときは buf (d 個) に戻して返す
    const float* row(int i, float* buf) const {
        if (f) return f + size_t(i) * d;
        const int8_t* r = q + size_t(i) * d;
        const float k = s[i];
        for (int j = 0; j < d; j++) buf[j] = k * float(r[j]);
        return buf;
    }
};

struct Model {
    int n_kind = 5;   // 辺の種類の数 (5、打ち間違いの辺つきは 6、英字の辺つきは 7)
    int V, d, layers, heads, ff, dk, max_len, n_seg, ctx, draft_max, draft;
    float beta, gamma;
    std::unordered_map<char32_t, int> vocab;
    Tab emb, Pa, Pb, Pc;
    // 版 5: Pa・Pb・Pc を持たず、語の k の最初の全結合 (3d → d、(入力, 出力)) をそのまま持つ。語ごとに計算してキャッシュする
    const float* kW = nullptr;
    std::vector<std::vector<float>> own;   // 版 5 の int8 から戻した全結合の重み (読み込んだときに作る)
    const float *seg, *pos;
    std::vector<LayerW> L;
    const float *normw, *normb, *end, *q1w, *q1b, *q2w, *q2b, *Kkind, *Klen, *k1b, *k2w, *k2b;
    const float *gw = nullptr, *gb = nullptr;   // ダイヤル (版 3 のときだけ)
    // 版 6 (M5): 使う人の印 (n_user 行) と書き方の印 (n_style 行) の表。k の全結合の真ん中の区切り (先頭の文字 + 種別) を掛けたもの
    int n_user = 0, n_style = 0;
    const float *Kuser = nullptr, *Kstyle = nullptr;
    int vid(char32_t c) const { auto it = vocab.find(c); return it == vocab.end() ? UNK : it->second; }
};

inline float gelu(float x) { return 0.5f * x * (1.0f + std::erf(x * 0.70710678118654752f)); }

// 使い回すスレッド (打鍵ごとにスレッドを作ると、その分だけ遅い)
struct Pool {
    std::vector<std::thread> th;
    std::mutex mu;
    std::condition_variable cv, done;
    std::function<void(int)> job;
    int n = 1, gen = 0, pending = 0;
    bool stop = false;
    void resize(int k) {
        shutdown();
        n = std::max(1, k);
        stop = false;
        for (int i = 1; i < n; i++)
            th.emplace_back([this, i] {
                int seen = 0;
                for (;;) {
                    std::function<void(int)> f;
                    {
                        std::unique_lock<std::mutex> lk(mu);
                        cv.wait(lk, [&] { return stop || gen != seen; });
                        if (stop) return;
                        seen = gen;
                        f = job;
                    }
                    f(i);
                    std::lock_guard<std::mutex> lk(mu);
                    if (--pending == 0) done.notify_one();
                }
            });
    }
    // f(0..n-1) を全スレッドで 1 回ずつ
    void run(const std::function<void(int)>& f) {
        if (n <= 1) { f(0); return; }
        {
            std::lock_guard<std::mutex> lk(mu);
            job = f;
            pending = n - 1;
            gen++;
        }
        cv.notify_all();
        f(0);
        std::unique_lock<std::mutex> lk(mu);
        done.wait(lk, [&] { return pending == 0; });
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : th) t.join();
        th.clear();
    }
    ~Pool() { shutdown(); }
};

// Y[t][o] = Σ_i X[t][i] · WT[i][o] + B[o] の、出力 [o0, o1) の分。WT は (入力, 出力) の向き (出力方向に連続)。
// 出力 16 本 × 行 4 本を一度に計算する (NEON / AVX で 16 本がベクトル 4 本 / 2 本になる)。
// 出力のかたまりを外側に回すので、重みはメモリから 1 回だけ読まれる (行のほうは L1/L2 に載っている)。
void lin_part(const float* X, int T, int in, const float* WT, int ldw, const float* B, float* Y, int ldy, int o0, int o1) {
    int o = o0;
    for (; o + 16 <= o1; o += 16) {
        for (int t = 0; t < T; t += 4) {
            const int tn = std::min(4, T - t);
            const float* x0 = X + size_t(t) * in;
            const float* x1 = tn > 1 ? x0 + in : x0;
            const float* x2 = tn > 2 ? x0 + 2 * size_t(in) : x0;
            const float* x3 = tn > 3 ? x0 + 3 * size_t(in) : x0;
            float a0[16], a1[16], a2[16], a3[16];
            for (int j = 0; j < 16; j++) a0[j] = a1[j] = a2[j] = a3[j] = B ? B[o + j] : 0.0f;
            for (int i = 0; i < in; i++) {
                const float* w = WT + size_t(i) * ldw + o;
                const float v0 = x0[i], v1 = x1[i], v2 = x2[i], v3 = x3[i];
                for (int j = 0; j < 16; j++) {
                    const float wj = w[j];
                    a0[j] += v0 * wj; a1[j] += v1 * wj; a2[j] += v2 * wj; a3[j] += v3 * wj;
                }
            }
            float* y = Y + size_t(t) * ldy + o;
            for (int j = 0; j < 16; j++) y[j] = a0[j];
            if (tn > 1) for (int j = 0; j < 16; j++) y[ldy + j] = a1[j];
            if (tn > 2) for (int j = 0; j < 16; j++) y[2 * ldy + j] = a2[j];
            if (tn > 3) for (int j = 0; j < 16; j++) y[3 * ldy + j] = a3[j];
        }
    }
    for (; o < o1; o++)   // 16 で割り切れない残り (今のモデルでは起きない)
        for (int t = 0; t < T; t++) {
            float s = B ? B[o] : 0.0f;
            for (int i = 0; i < in; i++) s += X[size_t(t) * in + i] * WT[size_t(i) * ldw + o];
            Y[size_t(t) * ldy + o] = s;
        }
}

#if KKC_X86
// この CPU で AVX2 と FMA が使えるか (OS がレジスタの保存に対応しているかも見る)
bool cpu_has_avx2() {
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!fma || !osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}
#endif

// 環境変数 KKC_NO_SIMD=1 で、速い版を使わない (比べるため)
bool use_avx2() {
#if KKC_X86
    static const bool on = cpu_has_avx2() && !getenv("KKC_NO_SIMD");
    return on;
#else
    return false;
#endif
}

void lin_any(const float* X, int T, int in, const float* WT, int ldw, const float* B, float* Y, int ldy, int o0, int o1) {
#if KKC_X86
    if (use_avx2()) {
        const int o16 = o0 + (o1 - o0) / 16 * 16;
        kkc_avx2_lin(X, T, in, WT, ldw, B, Y, ldy, o0, o16);
        if (o16 < o1) lin_part(X, T, in, WT, ldw, B, Y, ldy, o16, o1);   // 16 で割り切れない残り
        return;
    }
#endif
    lin_part(X, T, in, WT, ldw, B, Y, ldy, o0, o1);
}

void gelu_all(float* v, size_t n) {
#if KKC_X86
    if (use_avx2()) { kkc_avx2_gelu(v, n); return; }
#endif
    for (size_t i = 0; i < n; i++) v[i] = gelu(v[i]);
}

// 整数に丸めた全結合の重み (engine_simd.h の詰め方)。読み込むときに float の重みから作る
struct QW {
    int in = 0, inp = 0, out = 0;
    std::vector<short> w;     // 出力 16 本 × 入力 2 つずつの組
    std::vector<float> s;     // 出力ごとの倍率 (元の重み = 整数 × 倍率)
};

#if KKC_X86
#ifndef KKC_QW
#define KKC_QW 511    // engine_simd.h と同じ
#endif
// WT (入力, 出力) を出力ごとに ±KKC_QW に丸めて詰める
QW quantize(const float* WT, int in, int out) {
    QW q;
    q.in = in; q.inp = (in + 1) & ~1; q.out = out;
    q.s.assign(size_t(out), 1.0f);
    for (int o = 0; o < out; o++) {
        float m = 0;
        for (int i = 0; i < in; i++) m = std::max(m, std::fabs(WT[size_t(i) * out + o]));
        if (m > 0) q.s[o] = m / KKC_QW;
    }
    q.w.assign(size_t(out / 16) * q.inp * 16, 0);
    for (int o = 0; o < out; o++) {
        short* tile = q.w.data() + size_t(o / 16) * q.inp * 16;
        const int lane = (o % 16) / 8, j = o % 8;
        for (int i = 0; i < in; i++) {
            const float v = WT[size_t(i) * out + o] / q.s[o];
            tile[size_t(i / 2) * 32 + lane * 16 + j * 2 + (i & 1)] = short(std::lround(v));
        }
    }
    return q;
}
#endif

// 整数版を使うか (AVX2 のときだけ。環境変数 KKC_NO_QUANT=1 で使わない)
bool use_quant() {
#if KKC_X86
    static const bool on = use_avx2() && !getenv("KKC_NO_QUANT");
    return on;
#else
    return false;
#endif
}

// 出力を 16 本単位でスレッドに分ける
void linear(Pool* P, const float* X, int T, int in, const float* WT, int ldw, const float* B, int out, float* Y) {
    const int nth = P ? P->n : 1;
    if (nth <= 1 || out < 64 || T * out < 4096) { lin_any(X, T, in, WT, ldw, B, Y, out, 0, out); return; }
    const int tiles = (out + 15) / 16, per = (tiles + nth - 1) / nth;
    P->run([&](int k) {
        const int a = std::min(out, k * per * 16), b = std::min(out, (k + 1) * per * 16);
        if (a < b) lin_any(X, T, in, WT, ldw, B, Y, out, a, b);
    });
}

// linear と同じ計算を整数で。入力は最初に 1 回だけ丸める (xq・xs は作業場所)
void linear_q(Pool* P, const float* X, int T, const QW& q, const float* B, float* Y, std::vector<short>& xq, std::vector<float>& xs) {
#if KKC_X86
    xq.resize(size_t(T) * q.inp);
    xs.resize(size_t(T));
    kkc_avx2_quant_rows(X, T, q.in, q.inp, xq.data(), xs.data());
    const int out = q.out, nth = P ? P->n : 1;
    if (nth <= 1 || out < 64 || T * out < 4096) { kkc_avx2_lin_q(xq.data(), xs.data(), T, q.inp, q.w.data(), q.s.data(), B, Y, out, 0, out); return; }
    const int tiles = out / 16, per = (tiles + nth - 1) / nth;
    P->run([&](int k) {
        const int a = std::min(out, k * per * 16), b = std::min(out, (k + 1) * per * 16);
        if (a < b) kkc_avx2_lin_q(xq.data(), xs.data(), T, q.inp, q.w.data(), q.s.data(), B, Y, out, a, b);
    });
#else
    (void)P; (void)X; (void)T; (void)q; (void)B; (void)Y; (void)xq; (void)xs;
#endif
}

void layernorm(const float* x, int T, int d, const float* w, const float* b, float* y) {
    for (int t = 0; t < T; t++) {
        const float* v = x + size_t(t) * d;
        float m = 0, s = 0;
        for (int i = 0; i < d; i++) m += v[i];
        m /= d;
        for (int i = 0; i < d; i++) { float z = v[i] - m; s += z * z; }
        const float r = 1.0f / std::sqrt(s / d + 1e-5f);
        float* o = y + size_t(t) * d;
        for (int i = 0; i < d; i++) o[i] = (v[i] - m) * r * w[i] + b[i];
    }
}

}  // namespace

struct kkc_engine {
    Lex lex;
    Model m;
    std::vector<uint8_t> own_lex, own_model;   // kkc_open_files のときだけ中身を持つ
    Pool pool;
    std::unordered_map<int, std::vector<float>> prow[3];   // 版 5 の Pa・Pb・Pc の行 (使った文字だけ計算して覚える)
    double times[5] = {0, 0, 0, 0, 0};
    float typo_bias = 0;
    // 直しを候補の 1 位にする差 (1 字あたりのコスト)。kkc_set_typo_promote。200 で web の文 400 のうち 1 位を替えたのは 0、
    // キーの打ち間違いの 53%・フリックの隣の 43% を 1 位で直す (0 なら 62%・54% だが web で 9 文を替えた。2026-10-14 測定)
    double typo_promote = 200;
    // 英字の辺 (K_LATIN) を 1 位にするのに要る差 (1 字あたり、12 字まで)。英字の辺の点数から引いておくので、際どいときは日本語が 1 位、
    // 英字は 2 位以下 (候補の木の別の表記・別の区切り) に出る (ユーザー 2026-10-23「誤爆不安」「2 番目に文節分けを変えてでも英語」)。
    // KKC_LATIN_PROMOTE で調整 (測定用)
    double latin_promote = [] { const char* v = getenv("KKC_LATIN_PROMOTE"); return v ? atof(v) : 0.0; }();   // S8 では 0 (日本語の誤爆は増えず英文だけ落ちた、10/24 測定)。M5 で網を広げたら測り直す
    double typo_margin = 0;      // 直前の変換の差 (調整用)   // 打ち間違いの辺の点数から引く値 (kkc_set_typo。大きいほど直しが控えめ)
    // 提案 (kkc_set_suggest でオン): 変換の域を超えるもの (打ち間違いの直し・日付・郵便番号の住所) を候補に混ぜず、
    // 別の出口 (kkc_last_suggest) に種類つきで出す。候補は打ったとおりの変換だけになる
    bool suggest_mode = false;
    bool latin = true;   // 英字の辺 (ローマ字で打った英単語)。kkc_set_latin でオフにすると英字の打ち分けをしない (暴発が嫌な人向け)
    // 豆腐になる字の除外 (kkc_set_char_filter)。既定は全部除外 (HarmonyOS)。Android は Paint.hasGlyph、PC は字形の有無で
    // 端末側が調べるので切る (Android 15 から AOSP に Noto Serif Hentaigana が入り、変体仮名も出る端末がある)
    int char_filter = 7;   // 1 = 変体仮名・かなの拡張、2 = ヒエログリフ、4 = 麻雀牌・ドミノ・トランプ
    std::vector<std::pair<int, ustr>> suggest;   // (種類 KKC_SUGGEST_*, 表記)
    std::vector<int> typo_spans;   // 直前の変換の打ち間違いの直しの道で、直した辺の読みの区間 (始め, 終わり) の並び (kkc_typo_spans)
    // 語の k は文脈に依らないので、打鍵をまたいで取っておく (表記+種別 -> K の行)。多くなったら捨てる
    std::unordered_map<ustr, int> kcache;
    std::vector<float> K;
    std::vector<float> last_u;
    std::vector<SegInfo> last_segs;   // 直前の変換の 1 位の語の区切り
    std::vector<PathInfo> last_paths; // 直前の変換の上位の経路 (kkc_last_paths)
    double last_cost[2] = {0, 0};     // 直前の変換の 1 位・2 位 (表記が違う最初の 2 つ) の経路のコスト。2 位が無ければ大きな値
    std::vector<UserWord> user;                   // ユーザー辞書の語 (kkc_user_add_like で足す)
    // M5 の個人化 (版 6 のモデル): 書き方の対の表 (kkc_set_variants)・表記ごとの使う人の印 (kkc_set_marks)・
    // 書き方の記録 (kkc_set_style: 語ごと・種類ごとの、漢字とかなで書いた回数)
    struct VarForm { ustr other; int lemma, cls; bool kanji; };
    std::unordered_map<ustr, VarForm> var;
    std::unordered_map<ustr, int> umarks;
    std::unordered_map<int64_t, int> fam;   // 語 × 2 + 漢字か → 回数
    int cls_n[8][2] = {};                   // 種類 → (かな, 漢字) の回数
    std::unordered_map<std::string, LatinWord> user_latin;   // ユーザー辞書の英字の語 (表記が英字だけ。小文字・空白なしの綴り -> 語)
    // 打った鍵の列 (kkc_set_keys)。直後の 1 回の変換で英字の辺 (K_LATIN) を作るのに使う
    std::vector<uint16_t> keys;
    std::vector<int32_t> span_end;
    // カーソルの後ろの文 (kkc_set_right_ctx)。直後の 1 回の変換で、後ろの文脈を学んだモデル (区分 4 つ) に渡す
    std::vector<uint16_t> rctx;
    // 整数に丸めた重み (層ごとに inw, outw, l1w, l2w)。空なら float のまま
    std::vector<QW> qw;
    // 作業用
    std::vector<float> x, xn, qkv, att, tmp, ffb, h, xs;
    std::vector<short> xq;
};

namespace {

bool load_lex(Lex& L, const void* data, size_t size) {
    Reader r{static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size};
    if (size < 4 || memcmp(data, "KKL2", 4)) return false;
    r.off = 4;
    int32_t ver = r.val<int32_t>();
    if (ver != 1 && ver != 2) return false;
    L.ver = ver;
    L.noun = r.val<int32_t>(); L.num_arabic = r.val<int32_t>(); L.num_kanji = r.val<int32_t>(); L.max_word = r.val<int32_t>();
    L.oov = r.val<int32_t>(); L.kata = r.val<int32_t>(); L.kata_per = r.val<int32_t>(); L.pass = r.val<int32_t>();
    L.nconn = r.val<int32_t>(); L.conn16 = r.val<int32_t>(); L.nread = r.val<int32_t>(); L.nent = r.val<int32_t>();
    int64_t nr16 = r.val<int64_t>(), ns16 = r.val<int64_t>();
    size_t nc = size_t(L.nconn) * L.nconn;
    if (L.conn16) L.c16 = r.arr<int16_t>(nc); else L.c32 = r.arr<int32_t>(nc);
    L.r_off = r.arr<uint32_t>(L.nread + 1);
    L.e_first = r.arr<uint32_t>(L.nread + 1);
    if (ver == 1) L.ents = r.arr<Ent>(L.nent); else L.pents = r.arr<PEnt>(L.nent);
    L.r_blob = r.arr<uint16_t>(size_t(nr16));
    L.s_blob = r.arr<uint16_t>(size_t(ns16));
    return r.ok;
}

bool load_model(Model& M, const void* data, size_t size) {
    Reader r{static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size};
    if (size < 4 || memcmp(data, "KKM1", 4)) return false;
    r.off = 4;
    const int32_t ver = r.val<int32_t>();
    // 版 2: 全結合の重みは (入力, 出力)。版 3: + ダイヤル。版 4: 版 3 の文字ごとの表を int8 に。
    // 版 5: 版 4 + 層の大きな全結合も int8 (出力ごとの倍率)、Pa・Pb・Pc の代わりに k の全結合 (配る大きさ 24.8MB -> 約 6MB)
    // 版 6: 版 5 + 最後に 使う人の印・書き方の印の表 (M5)
    if (ver < 2 || ver > 6) return false;
    M.V = r.val<int32_t>(); M.d = r.val<int32_t>(); M.layers = r.val<int32_t>(); M.heads = r.val<int32_t>();
    M.ff = r.val<int32_t>(); M.dk = r.val<int32_t>(); M.max_len = r.val<int32_t>(); M.n_seg = r.val<int32_t>();
    M.ctx = r.val<int32_t>(); M.draft_max = r.val<int32_t>(); M.draft = r.val<int32_t>();
    // 下書きの欄の上の桁 = 辺の種類の数 (0 なら 5。S7 から 6 = 打ち間違いの辺つき)
    M.n_kind = (M.draft >> 8) ? (M.draft >> 8) : 5;
    M.draft &= 0xFF;
    M.beta = r.val<float>(); M.gamma = r.val<float>();
    int nch = r.val<int32_t>();
    for (int i = 0; i < nch; i++) M.vocab[char32_t(r.val<uint32_t>())] = 4 + i;
    const int d = M.d, ff = M.ff, dk = M.dk, V = M.V;
    // 文字ごとの表: 版 4 は倍率 (V 個の fp32) と int8 (V × d)
    auto tab = [&](Tab& t) {
        t.d = d;
        if (ver >= 4) { t.s = r.arr<float>(size_t(V)); t.q = r.arr<int8_t>(size_t(V) * d); }
        else t.f = r.arr<float>(size_t(V) * d);
    };
    tab(M.emb); M.seg = r.arr<float>(size_t(M.n_seg) * d); M.pos = r.arr<float>(size_t(M.max_len) * d);
    for (int l = 0; l < M.layers; l++) {
        LayerW w;
        w.ln1w = r.arr<float>(d); w.ln1b = r.arr<float>(d);
        // 版 5 の大きな全結合: 出力ごとの倍率 (out 個) + int8 (入力 × 出力)。float に戻して持つ
        auto big = [&](int in, int out) -> const float* {
            if (ver < 5) return r.arr<float>(size_t(in) * out);
            const float* sc = r.arr<float>(size_t(out));
            const int8_t* q = r.arr<int8_t>(size_t(in) * out);
            if (!r.ok) return nullptr;
            std::vector<float> f(size_t(in) * out);
            for (int i = 0; i < in; i++)
                for (int o = 0; o < out; o++) f[size_t(i) * out + o] = sc[o] * float(q[size_t(i) * out + o]);
            M.own.push_back(std::move(f));
            return M.own.back().data();
        };
        w.inw = big(d, 3 * d); w.inb = r.arr<float>(3 * d);
        w.outw = big(d, d); w.outb = r.arr<float>(d);
        w.ln2w = r.arr<float>(d); w.ln2b = r.arr<float>(d);
        w.l1w = big(d, ff); w.l1b = r.arr<float>(ff);
        w.l2w = big(ff, d); w.l2b = r.arr<float>(d);
        M.L.push_back(w);
    }
    M.normw = r.arr<float>(d); M.normb = r.arr<float>(d); M.end = r.arr<float>(d);
    M.q1w = r.arr<float>(size_t(d) * 3 * d); M.q1b = r.arr<float>(d); M.q2w = r.arr<float>(size_t(dk) * d); M.q2b = r.arr<float>(dk);
    if (ver >= 5) M.kW = r.arr<float>(size_t(3) * d * d);
    else { tab(M.Pa); tab(M.Pb); tab(M.Pc); }
    M.Kkind = r.arr<float>(size_t(M.n_kind) * d); M.Klen = r.arr<float>(size_t(17) * d);
    M.k1b = r.arr<float>(d); M.k2w = r.arr<float>(size_t(dk) * d); M.k2b = r.arr<float>(dk);
    if (ver >= 3) { M.gw = r.arr<float>(size_t(dk) * dk); M.gb = r.arr<float>(dk); }
    if (ver >= 6) {
        M.n_user = r.val<int32_t>(); M.n_style = r.val<int32_t>();
        if (M.n_user > 0) M.Kuser = r.arr<float>(size_t(M.n_user) * d);
        if (M.n_style > 0) M.Kstyle = r.arr<float>(size_t(M.n_style) * d);
    }
    return r.ok;
}

// エンコーダ。tokens/segs (T) → h (T×d)
void encode(kkc_engine* E, const std::vector<int>& tok, const std::vector<int>& seg) {
    const Model& M = E->m;
    const int T = int(tok.size()), d = M.d, H = M.heads, dh = d / H, ff = M.ff;
    auto& x = E->x; auto& xn = E->xn; auto& qkv = E->qkv; auto& att = E->att; auto& tmp = E->tmp; auto& fb = E->ffb;
    x.assign(size_t(T) * d, 0); xn.resize(size_t(T) * d); qkv.resize(size_t(T) * 3 * d); att.resize(size_t(T) * d);
    tmp.resize(size_t(T) * d); fb.resize(size_t(T) * ff);
    std::vector<float> ebuf(size_t(d), 0.0f);   // int8 の表 (版 4) の行を戻す場所
    for (int t = 0; t < T; t++) {
        const float *a = M.emb.row(tok[t], ebuf.data()), *b = M.seg + size_t(seg[t]) * d, *c = M.pos + size_t(t) * d;
        for (int i = 0; i < d; i++) x[size_t(t) * d + i] = a[i] + b[i] + c[i];
    }
    std::vector<float> sc(size_t(T) + 8);   // AVX2 版は 8 本単位で使う
    const float scale = 1.0f / std::sqrt(float(dh));
    for (int li = 0; li < M.layers; li++) {
        const LayerW& w = M.L[size_t(li)];
        layernorm(x.data(), T, d, w.ln1w, w.ln1b, xn.data());
        const QW* q = E->qw.empty() ? nullptr : &E->qw[size_t(li) * 4];
        if (q) linear_q(&E->pool, xn.data(), T, q[0], w.inb, qkv.data(), E->xq, E->xs);
        else linear(&E->pool, xn.data(), T, d, w.inw, 3 * d, w.inb, 3 * d, qkv.data());
#if KKC_X86
        if (use_avx2() && dh % 8 == 0) {
            for (int hh = 0; hh < H; hh++) kkc_avx2_attend(qkv.data(), T, d, dh, hh, scale, sc.data(), att.data());
        } else
#endif
        for (int hh = 0; hh < H; hh++) {
            for (int t = 0; t < T; t++) {
                const float* q = qkv.data() + size_t(t) * 3 * d + hh * dh;
                float mx = -1e30f;
                for (int u = 0; u < T; u++) {
                    const float* k = qkv.data() + size_t(u) * 3 * d + d + hh * dh;
                    float s = 0;
                    for (int i = 0; i < dh; i++) s += q[i] * k[i];
                    sc[u] = s * scale;
                    mx = std::max(mx, sc[u]);
                }
                float z = 0;
                for (int u = 0; u < T; u++) { sc[u] = std::exp(sc[u] - mx); z += sc[u]; }
                float* o = att.data() + size_t(t) * d + hh * dh;
                for (int i = 0; i < dh; i++) o[i] = 0;
                for (int u = 0; u < T; u++) {
                    const float p = sc[u] / z;
                    const float* v = qkv.data() + size_t(u) * 3 * d + 2 * d + hh * dh;
                    for (int i = 0; i < dh; i++) o[i] += p * v[i];
                }
            }
        }
        if (q) linear_q(&E->pool, att.data(), T, q[1], w.outb, tmp.data(), E->xq, E->xs);
        else linear(&E->pool, att.data(), T, d, w.outw, d, w.outb, d, tmp.data());
        for (size_t i = 0; i < x.size(); i++) x[i] += tmp[i];
        layernorm(x.data(), T, d, w.ln2w, w.ln2b, xn.data());
        if (q) linear_q(&E->pool, xn.data(), T, q[2], w.l1b, fb.data(), E->xq, E->xs);
        else linear(&E->pool, xn.data(), T, d, w.l1w, ff, w.l1b, ff, fb.data());
        gelu_all(fb.data(), fb.size());
        if (q) linear_q(&E->pool, fb.data(), T, q[3], w.l2b, tmp.data(), E->xq, E->xs);
        else linear(&E->pool, fb.data(), T, ff, w.l2w, d, w.l2b, d, tmp.data());
        for (size_t i = 0; i < x.size(); i++) x[i] += tmp[i];
    }
    E->h.resize(size_t(T) * d);
    layernorm(x.data(), T, d, M.normw, M.normb, E->h.data());
}

// 辺ごとの u (学習側の forward_u と同じ)
void scores(kkc_engine* E, int rstart, int n, const std::vector<Edge>& ed, std::vector<float>& u) {
    const Model& M = E->m;
    const int d = M.d, dk = M.dk;
    const float* h = E->h.data();
    // 境界 p (0..n) の状態と、読みの各文字の状態
    std::vector<float> Bd(size_t(n + 1) * d), QA(size_t(n + 1) * d), QB(size_t(n + 1) * d), QC(size_t(std::max(n, 1)) * d);
    for (int p = 0; p <= n; p++) {
        const float* left = h + size_t(rstart + p - 1) * d;
        const float* right = p < n ? h + size_t(rstart + p) * d : M.end;
        for (int i = 0; i < d; i++) Bd[size_t(p) * d + i] = (left[i] + right[i]) * 0.5f;
    }
    // q の最初の層 (3d, d) を [境界 s | 境界 e | 区間の平均] の 3 つに分けて、位置ごとに掛けておく
    linear(nullptr, Bd.data(), n + 1, d, M.q1w, d, nullptr, d, QA.data());
    linear(nullptr, Bd.data(), n + 1, d, M.q1w + size_t(d) * d, d, nullptr, d, QB.data());
    linear(nullptr, h + size_t(rstart) * d, n, d, M.q1w + 2 * size_t(d) * d, d, nullptr, d, QC.data());
    std::vector<double> CQ(size_t(n + 1) * d, 0.0);   // QC の累積和
    for (int i = 0; i < n; i++)
        for (int j = 0; j < d; j++) CQ[size_t(i + 1) * d + j] = CQ[size_t(i) * d + j] + QC[size_t(i) * d + j];
    // 区間ごとの q
    std::unordered_map<int, int> span_id;
    std::vector<float> Q;
    std::vector<float> hid(d), tbuf(size_t(2) * d);   // tbuf: int8 の表の行を戻す場所 (版 4)
    auto span_q = [&](int s, int e) {
        int key = s * 4096 + e;
        auto it = span_id.find(key);
        if (it != span_id.end()) return it->second;
        const float inv = 1.0f / float(std::max(e - s, 1));
        for (int j = 0; j < d; j++)
            hid[j] = QA[size_t(s) * d + j] + QB[size_t(e) * d + j] + float(CQ[size_t(e) * d + j] - CQ[size_t(s) * d + j]) * inv + M.q1b[j];
        gelu_all(hid.data(), size_t(d));
        int id = int(Q.size() / dk);
        Q.resize(Q.size() + dk);
        linear(nullptr, hid.data(), 1, d, M.q2w, dk, M.q2b, dk, Q.data() + size_t(id) * dk);
        span_id[key] = id;
        return id;
    };
    // 語ごとの k (表記と種別で 1 つ。打鍵をまたいで取っておく)
    auto& word_id = E->kcache;
    auto& K = E->K;
    if (word_id.size() > 30000) { word_id.clear(); K.clear(); }
    auto word_k = [&](const Edge& e) {
        ustr key = e.surf;
        key.push_back(char32_t(0x110000 + e.kind));
        if (e.mark) key.push_back(char32_t(0x120000 + e.mark));
        auto it = word_id.find(key);
        if (it != word_id.end()) return it->second;
        const int nc = int(e.surf.size());
        const float inv = 1.0f / float(std::max(nc, 1));
        const float* kk = M.Kkind + size_t(e.kind) * d;
        const float* kl = M.Klen + size_t(std::min(nc, 16)) * d;
        if (M.kW) {
            // 版 5: Pa・Pb・Pc の行 (文字の表の行 × k の全結合の 3 つの区切り) を、使った文字だけ計算して覚えておく。
            // 前もって掛けて配っていた表と同じ値 (語ごとに 3d × d を掛けると PC で 2.5 倍遅かった)
            auto prow = [&](int part, int v) -> const float* {
                auto& cache = E->prow[part];
                auto it = cache.find(v);
                if (it != cache.end()) return it->second.data();
                std::vector<float> out(size_t(d), 0.0f);
                const float* er = M.emb.row(v, tbuf.data());
                const float* W0 = M.kW + size_t(part) * d * d;
                for (int i = 0; i < d; i++) {
                    const float x = er[i];
                    if (x == 0.0f) continue;
                    const float* wr = W0 + size_t(i) * d;
                    for (int j = 0; j < d; j++) out[size_t(j)] += x * wr[j];
                }
                return cache.emplace(v, std::move(out)).first->second.data();
            };
            for (int j = 0; j < d; j++) hid[j] = 0;
            for (char32_t c : e.surf) {
                const float* p = prow(0, M.vid(c));
                for (int j = 0; j < d; j++) hid[j] += p[j];
            }
            const float* pb = prow(1, M.vid(e.surf.front()));
            const float* pc = prow(2, M.vid(e.surf.back()));
            for (int j = 0; j < d; j++) hid[j] = hid[j] * inv + pb[j] + kk[j] + pc[j] + kl[j] + M.k1b[j];
            if (M.Kuser) {   // M5: 印 (使う人・書き方) の表の行を足す。印の無い辺も 0 番の行を足す (学習と同じ)
                const int um = std::min(e.mark % 32, M.n_user - 1), sm = e.mark / 32;
                const float* ku = M.Kuser + size_t(um) * d;
                for (int j = 0; j < d; j++) hid[j] += ku[j];
                if (M.Kstyle) {
                    const float* ks = M.Kstyle + size_t(std::min(sm, M.n_style - 1)) * d;
                    for (int j = 0; j < d; j++) hid[j] += ks[j];
                }
            }
        } else {
            for (int j = 0; j < d; j++) hid[j] = 0;
            for (char32_t c : e.surf) {
                const float* p = M.Pa.row(M.vid(c), tbuf.data());
                for (int j = 0; j < d; j++) hid[j] += p[j];
            }
            const float* pb = M.Pb.row(M.vid(e.surf.front()), tbuf.data());
            const float* pc = M.Pc.row(M.vid(e.surf.back()), tbuf.data() + d);
            for (int j = 0; j < d; j++) hid[j] = hid[j] * inv + pb[j] + kk[j] + pc[j] + kl[j] + M.k1b[j];
        }
        gelu_all(hid.data(), size_t(d));
        int id = int(K.size() / dk);
        K.resize(K.size() + dk);
        linear(nullptr, hid.data(), 1, d, M.k2w, dk, M.k2b, dk, K.data() + size_t(id) * dk);
        word_id[key] = id;
        return id;
    };
    u.resize(ed.size());
    const float sc = 1.0f / std::sqrt(float(dk));
    // ダイヤル: 辺ごとに g = 2·sigmoid(gate(q)·k / √dk) (0〜2)。経路の語のコストを β·g·cost にするため、
    // nbest が足す β·cost との差 β·(g-1)·cost を u の側で引く (学習側の forward_u と同じ)。gate(q) は区間ごとに 1 回
    std::vector<float> GQ;
    std::vector<char> gq_done;
    for (size_t i = 0; i < ed.size(); i++) {
        const int a = span_q(ed[i].s, ed[i].e), b = word_k(ed[i]);
        const float *q = Q.data() + size_t(a) * dk, *k = K.data() + size_t(b) * dk;
        float s = 0;
        for (int j = 0; j < dk; j++) s += q[j] * k[j];
        float ui = s * sc;
        if (M.gw) {
            if (size_t(a) >= gq_done.size()) { gq_done.resize(size_t(a) + 1, 0); GQ.resize((size_t(a) + 1) * dk); }
            if (!gq_done[size_t(a)]) { linear(nullptr, q, 1, dk, M.gw, dk, M.gb, dk, GQ.data() + size_t(a) * dk); gq_done[size_t(a)] = 1; }
            const float* gq = GQ.data() + size_t(a) * dk;
            float t = 0;
            for (int j = 0; j < dk; j++) t += gq[j] * k[j];
            const float g = 2.0f / (1.0f + std::exp(-t * sc));
            ui -= M.beta * (g - 1.0f) * float(ed[i].cost) / 1000.0f;
        }
        u[i] = ui;
    }
}

// 上位 k (lattice.nbest / kkcnative の kkc_nbest と同じ)
int nbest(const Lex& L, const std::vector<Edge>& E, int n, const float* u, double beta, double gamma, int k, int maxout,
          std::vector<ustr>& res, std::vector<SegInfo>* segs = nullptr, double* top2 = nullptr) {
    const int M = int(E.size());
    std::vector<std::vector<int>> by_start(n + 1), by_end(n + 1);
    for (int i = 0; i < M; i++) { by_start[E[i].s].push_back(i); by_end[E[i].e].push_back(i); }
    struct Cand { double c; int j, r; };
    std::vector<std::vector<Cand>> best(M);
    std::vector<char> has(M, 0);
    std::vector<Cand> cands;
    std::vector<int> idx;
    for (int pos = 0; pos < n; pos++) {
        for (int i : by_start[pos]) {
            const Edge& ed = E[i];
            double own = u ? double(float(beta * ed.cost) - 1000.0f * u[i]) : beta * ed.cost;
            cands.clear();
            if (pos == 0) {
                cands.push_back({gamma * L.conn(0, ed.lid) + own, -1, 0});
            } else {
                for (int j : by_end[pos]) {
                    if (!has[j]) continue;
                    double t = gamma * L.conn(E[j].rid, ed.lid) + own;
                    for (int r = 0; r < int(best[j].size()); r++) cands.push_back({best[j][r].c + t, j, r});
                }
                if (cands.empty()) continue;
            }
            // 上位 k だけを選ぶ (全部を並べ替えない)。同点は元の順 = 安定ソートして先頭 k 個と同じ結果
            const int nc = int(cands.size());
            if (nc > k) {
                idx.resize(nc);
                for (int q = 0; q < nc; q++) idx[q] = q;
                std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [&](int x, int y) {
                    return cands[x].c < cands[y].c || (cands[x].c == cands[y].c && x < y);
                });
                auto& dst = best[i];
                dst.resize(k);
                for (int q = 0; q < k; q++) dst[q] = cands[idx[q]];
            } else {
                std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.c < b.c; });
                best[i] = cands;
            }
            has[i] = 1;
        }
    }
    std::vector<Cand> fin;
    for (int j : by_end[n])
        for (int r = 0; r < int(best[j].size()); r++) fin.push_back({best[j][r].c + gamma * L.conn(E[j].rid, 0), j, r});
    std::stable_sort(fin.begin(), fin.end(), [](const Cand& a, const Cand& b) { return a.c < b.c; });
    std::unordered_set<ustr> seen;
    std::vector<int> path;
    res.clear();
    if (top2) { top2[0] = 0; top2[1] = 1e18; }
    for (auto& f : fin) {
        path.clear();
        int j = f.j, r = f.r;
        while (j >= 0) { path.push_back(j); const Cand& c = best[j][r]; j = c.j; r = c.r; }
        ustr surf;
        for (auto it = path.rbegin(); it != path.rend(); ++it) surf += E[*it].surf;
        if (!seen.insert(surf).second) continue;
        if (segs) {
            if (res.empty()) tl_paths.clear();
            PathInfo pi{f.c, {}};
            for (auto it = path.rbegin(); it != path.rend(); ++it) pi.w.push_back({E[*it].e, E[*it].surf});
            tl_paths.push_back(std::move(pi));
        }
        if (res.empty() && segs) {   // 1 位の語の区切り: (読みの終わりの位置, 表記の長さ UTF-16)
            segs->clear();
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                int l16 = 0;
                for (char32_t c : E[*it].surf) l16 += c >= 0x10000 ? 2 : 1;
                segs->push_back({E[*it].e, l16, E[*it].lid, E[*it].rid});
            }
        }
        if (top2 && res.size() < 2) top2[res.size()] = f.c;
        res.push_back(surf);
        if (int(res.size()) >= maxout) break;
    }
    return int(res.size());
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

KKC_API kkc_engine* kkc_open(const void* lex, size_t lex_size, const void* model, size_t model_size) {
    init_numbers();
    auto* e = new kkc_engine();
    if (!load_lex(e->lex, lex, lex_size) || !load_model(e->m, model, model_size)) { delete e; return nullptr; }
    e->lex.typo = e->m.n_kind > K_TYPO;   // モデルが打ち間違いの辺を知っていれば作る
    if (e->lex.typo) e->lex.build_bloom();
    e->lex.latin = e->m.n_kind > K_LATIN;   // モデルが英字の辺を知っていれば作る
    if (e->lex.latin) {
        // 英字だけの語を、小文字の綴り -> 表記 の表にする (英字の辺づくりで引く)
        for (uint32_t j = 0; j < uint32_t(e->lex.nent); j++) {
            const Ent en = e->lex.ent(j);
            if (en.slen < 2) continue;
            const uint16_t* s = e->lex.s_blob + en.soff;
            std::string lower;
            lower.reserve(size_t(en.slen));
            bool ok = true;
            for (int k = 0; k < en.slen; k++) {
                const uint16_t c = s[k];
                if (c >= u'a' && c <= u'z') lower.push_back(char(c));
                else if (c >= u'A' && c <= u'Z') lower.push_back(char(c + 32));
                else if (c >= u'0' && c <= u'9') lower.push_back(char(c));
                else { ok = false; break; }
            }
            if (!ok) continue;
            // 英カタカナ辞書の印 (ENG_COST) の語 (カタカナの題名の英字の綴り: ぱり → Paris・ゆにくろ → UNIQLO) も、ローマ字で英字のまま
            // 打ったときは英単語として使う (ユーザー 2026-10-23)。コストは辞書に無い英字よりは安く。同じ綴りはコストの安い表記
            const int32_t cost = en.cost >= ENG_COST ? ENG_LATIN_COST : en.cost;
            auto it = e->lex.latin_words.find(lower);
            if (it == e->lex.latin_words.end() || cost < it->second.cost)
                e->lex.latin_words[lower] = LatinWord{from16(s, en.slen), int32_t(en.lid), int32_t(en.rid), cost};
        }
    }
    {   // 略語の品詞は、辞書の「じーでぃーぴー → GDP」から写す (無ければ略語の候補は作らない)
        const std::u16string k = u"じーでぃーぴー";
        uint32_t a, b;
        if (e->lex.find(reinterpret_cast<const uint16_t*>(k.data()), int(k.size()), a, b))
            for (uint32_t j = a; j < b; j++) {
                const Ent en = e->lex.ent(j);
                if (en.slen == 3 && e->lex.s_blob[en.soff] == u'G') { e->lex.abbr_lid = en.lid; e->lex.abbr_rid = en.rid; break; }
            }
    }
#if KKC_X86
    // 整数版の重み (16 出力単位・入力の数が 2048 以下のときだけ。合計が 32 ビットに収まる範囲)
    const Model& M = e->m;
    if (use_quant() && M.d % 16 == 0 && M.ff % 16 == 0 && M.ff <= 2048 && M.d <= 2048) {
        for (const LayerW& w : M.L) {
            e->qw.push_back(quantize(w.inw, M.d, 3 * M.d));
            e->qw.push_back(quantize(w.outw, M.d, M.d));
            e->qw.push_back(quantize(w.l1w, M.d, M.ff));
            e->qw.push_back(quantize(w.l2w, M.ff, M.d));
        }
    }
#endif
    return e;
}

KKC_API kkc_engine* kkc_open_files(const char* lex_path, const char* model_path) {
    auto rd = [](const char* p, std::vector<uint8_t>& buf) {
        FILE* f = fopen(p, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf.resize(size_t(n));
        bool ok = fread(buf.data(), 1, size_t(n), f) == size_t(n);
        fclose(f);
        return ok;
    };
    std::vector<uint8_t> a, b;
    if (!rd(lex_path, a) || !rd(model_path, b)) return nullptr;
    kkc_engine* e = kkc_open(a.data(), a.size(), b.data(), b.size());
    if (!e) return nullptr;
    // 中身のポインタは a, b を指しているので、エンジンに持たせる (vector の移動ではバッファの位置は変わらない)
    e->own_lex = std::move(a);
    e->own_model = std::move(b);
    return e;
}

KKC_API void kkc_close(kkc_engine* e) { delete e; }
KKC_API void kkc_set_threads(kkc_engine* e, int n) { if (e) e->pool.resize(n); }
// 打ち間違いの直し (S7 から。モデルが辺の種類 typo を知っているときだけ効く): on = 0 で作らない、
// bias = 打ち間違いの辺の点数から引く値 (0 で学習どおり。大きいほど余計な直しが減り、直せる誤字も減る)
KKC_API void kkc_set_suggest(kkc_engine* e, int on) { if (e) e->suggest_mode = on != 0; }
KKC_API void kkc_set_char_filter(kkc_engine* e, int mask) { if (e) e->char_filter = mask; }

KKC_API void kkc_set_typo(kkc_engine* e, int on, float bias) {
    if (!e) return;
    e->lex.typo = on != 0 && e->m.n_kind > K_TYPO;
    if (e->lex.typo) e->lex.build_bloom();
    e->typo_bias = bias;
}

// カーソルの後ろの文 (後ろの文脈) を渡す。直後の 1 回の kkc_convert にだけ効く。後ろの文脈を学んだモデル (S8 から) だけが使い、
// それ以前のモデルでは何もしない。長さはモデルの位置の表に合わせて切る (呼ぶ側は 80 字ほどを渡せばよい)
KKC_API void kkc_set_right_ctx(kkc_engine* e, const uint16_t* text, int n) {
    if (!e) return;
    if (n > 0) e->rctx.assign(text, text + n); else e->rctx.clear();
}

KKC_API int kkc_has_right_ctx(kkc_engine* e) { return e && e->m.n_seg > 3 ? 1 : 0; }

// 打った鍵の列 (ローマ字の英字) を渡す。英字のまま打った語 (github) を候補にできるようにする。
// keys = 打った鍵 (英字・記号そのまま)。span_end[i] = 読みの i 文字目が鍵の何文字目までで決まったか (単調増加)。
// 直後の 1 回の kkc_convert にだけ効く (その変換の読みの長さが nk と合わなければ使わない)
KKC_API void kkc_set_keys(kkc_engine* e, const uint16_t* keys, int nkeys, const int32_t* span_end, int nk) {
    if (!e) return;
    // 区間が鍵の列からはみ出す・逆戻りするものは使わない (範囲外を読まないため)
    for (int i = 0; i < nk; i++)
        if (span_end[i] < (i ? span_end[i - 1] : 0) || span_end[i] > nkeys) { e->keys.clear(); e->span_end.clear(); return; }
    if (nkeys > 0) e->keys.assign(keys, keys + nkeys); else e->keys.clear();
    if (nk > 0) e->span_end.assign(span_end, span_end + nk); else e->span_end.clear();
}

// 段階式のしきい値 (読み 1 字あたりのコストの差)。dev・AJIMEE・日常・入力ログで測った (kkc/engine/cascade.cpp)。
// S6 で 300 のとき、正解率は毎回モデルを通すのとほぼ同じ (dev 86.1→85.6、ほかは同じ) で、
// 普段の入力 (入力ログ) ではモデルを呼ぶのが 4 割ほどに減る
constexpr double CASCADE_PER_CHAR = 300.0;
constexpr int CASCADE_K = 3;   // 判定に使う上位の数
// 短い読みの候補の並べ方 (下の kkc_convert)。この字数までの読みは、採点器の上位 SHORT_KEEP_MODEL 個の後ろを辞書の順にする
constexpr int SHORT_READING = 3;
constexpr int SHORT_KEEP_MODEL = 3;
// これより長い読みは辞書だけで決まることがない (測った 1,590 例で 16 字以上は 0 例) ので、判定をせずにモデルへ
constexpr int CASCADE_MAX_LEN = 16;

// 文の中の 4 桁以上の数字の並びを 3 桁ごとにカンマで区切ったもの (5000兆円 -> 5,000兆円)。変わらなければ空。
// 年 (2026年) と、もとからカンマ・小数点の付いた並びはそのまま
ustr with_commas_in(const ustr& t) {
    ustr out;
    bool changed = false;
    for (size_t i = 0; i < t.size();) {
        auto dig = [](char32_t c) { return c >= U'0' && c <= U'9'; };
        if (!dig(t[i])) { out.push_back(t[i++]); continue; }
        size_t j = i;
        while (j < t.size() && dig(t[j])) j++;
        const ustr run = t.substr(i, j - i);
        const bool edge = (i > 0 && (t[i - 1] == U',' || t[i - 1] == U'.')) || (j < t.size() && (t[j] == U',' || t[j] == U'.' || t[j] == U'年'));
        if (run.size() >= 4 && !edge) { out += with_commas(run); changed = true; }
        else out += run;
        i = j;
    }
    return changed ? out : ustr();
}

// 1 位の漢数字をアラビア数字にした候補 (二千兆円 -> 2000兆円、六月二十日 -> 6月20日)。無ければ空。
// モデルの教材では半角数字の読みが数字のまま (かなでない) だったので、モデルは かなの数 -> アラビア数字 を学んでおらず、
// 数をいつも漢数字にしていた。語の区切り (segs) の頭で、読みが数として読めて、表記がその漢数字で始まる所だけ変える。
// 変えるのは 100 以上で読み 3 字以上の数 (にせんにじゅうろく) か、2 以上の数のすぐ後ろに助数詞が続く所 (さんにん・ろくがつ)。
// 1 (一度・一番・一日) と、ど・ばん・だい (二度と・二晩・二代)、助数詞の無い 2 桁 (十一) は変えない
ustr arabicize(const ustr& r, const ustr& top, const std::vector<SegInfo>& segs) {
    // 助数詞の読みと、その表記の頭の字 (読みだけで決めると 二次元 -> 2次元 になる)
    static const std::pair<ustr, ustr> COUNTERS[] = {
        {U"がつ", U"月"}, {U"にち", U"日"}, {U"じ", U"時"}, {U"ふん", U"分"}, {U"ぷん", U"分"}, {U"びょう", U"秒"},
        {U"ねん", U"年"}, {U"えん", U"円"}, {U"にん", U"人"}, {U"かい", U"回階"}, {U"こ", U"個"}, {U"ほん", U"本"},
        {U"ぼん", U"本"}, {U"ぽん", U"本"}, {U"まい", U"枚"}, {U"さい", U"歳才"}, {U"けん", U"件軒"}, {U"ばい", U"倍"},
        {U"ぱーせんと", U"パ%"}, {U"ぺーじ", U"ペ頁"}, {U"めーとる", U"メm"}, {U"きろ", U"キk"}, {U"せんち", U"センc"},
        {U"ぐらむ", U"グg"}, {U"どる", U"ド"}, {U"しゅうかん", U"週"}, {U"かげつ", U"かカ ヶヵ箇"}, {U"ひき", U"匹"},
        {U"びき", U"匹"}, {U"ぴき", U"匹"}, {U"さつ", U"冊"}, {U"とう", U"頭"}, {U"わ", U"羽話"}, {U"てん", U"点"}};
    static const ustr NUMK = U"〇一二三四五六七八九十百千万億兆";
    // 日の特別な読み。月のすぐ後ろのときだけ (5月三日 と混ざらないように。月の無い 一日 (ついたち) はそのまま)
    static const std::pair<ustr, int> DAYS[] = {
        {U"ついたち", 1}, {U"ふつか", 2}, {U"みっか", 3}, {U"よっか", 4}, {U"いつか", 5}, {U"むいか", 6}, {U"なのか", 7},
        {U"ようか", 8}, {U"ここのか", 9}, {U"とおか", 10}, {U"じゅうよっか", 14}, {U"はつか", 20}, {U"にじゅうよっか", 24}};
    ustr out;
    bool changed = false;
    size_t rs = 0, ss = 0;   // 区切りの頭: 読みの位置、表記の位置 (char32)
    auto u16len = [](char32_t c) { return c >= 0x10000 ? 2 : 1; };
    std::vector<std::pair<size_t, size_t>> heads;   // (読み, 表記) の区切りの頭
    for (const SegInfo& g : segs) {
        heads.push_back({rs, ss});
        int l = 0;
        while (ss < top.size() && l < g.l16) l += u16len(top[ss++]);
        rs = size_t(g.e);
    }
    size_t cur = 0;   // top の、ここまで out に写した位置
    for (auto& h : heads) {
        const size_t r0 = h.first, s0 = h.second;
        if (s0 < cur || s0 >= top.size()) continue;
        if (NUMK.find(top[s0]) == ustr::npos) continue;
        if (s0 > 0 && top[s0 - 1] == U'月') {
            bool hit = false;
            for (const auto& d : DAYS) {
                const ustr kd = kanji(d.second) + U"日";
                if (starts_with(r, r0, d.first) && top.compare(s0, kd.size(), kd) == 0) {
                    out += top.substr(cur, s0 - cur);
                    out += digits(d.second, false) + U"日";
                    cur = s0 + kd.size();
                    changed = hit = true;
                    break;
                }
            }
            if (hit) continue;
        }
        // 読みの頭から数として読める所のうち、表記がその漢数字で始まる、いちばん長い所
        // (にじゅうにち: 「にじゅうに」も数として読めるが、表記 二十日 と合うのは「にじゅう」)
        int64_t v = 0;
        size_t pe = 0;
        ustr kj;
        for (size_t b : boundaries(r, r0)) {
            int64_t x;
            bool sk;   // 促音で終わる数 (さんじゅっ|ぷん) も、後ろの助数詞の表記を確かめるのでよい
            if (!parse_num(r.substr(r0, b - r0), x) && !(b - r0 >= 2 && parse(r.substr(r0, b - r0), x, sk))) continue;
            const ustr k = kanji(x);
            if (top.compare(s0, k.size(), k) != 0) continue;
            if (s0 + k.size() < top.size() && NUMK.find(top[s0 + k.size()]) != ustr::npos) continue;
            v = x; pe = b; kj = k;
        }
        if (!pe || v < 2) continue;
        // 助数詞の無い 3 桁以上の数は、すぐ後ろが漢字でないときだけ (百貨店 -> 100貨店 にしない)
        const char32_t nx = s0 + kj.size() < top.size() ? top[s0 + kj.size()] : 0;
        bool ok = v >= 100 && pe - r0 >= 3 && !(nx >= 0x4E00 && nx <= 0x9FFF);
        if (!ok)
            for (const auto& c : COUNTERS)
                if (starts_with(r, pe, c.first) && s0 + kj.size() < top.size() &&
                    c.second.find(top[s0 + kj.size()]) != ustr::npos) { ok = true; break; }
        if (!ok) continue;
        out += top.substr(cur, s0 - cur);
        out += v >= 10000 ? unit_mixed(v, false) : digits(v, false);
        cur = s0 + kj.size();
        changed = true;
    }
    if (!changed) return ustr();
    out += top.substr(cur);
    return out;
}

// 絵文字か (🐈・❤️・🇯🇵。顔文字 (^^) や記号 ♡ は入れない: かな・漢字・英数字を含まず、絵文字の範囲の字を含む)
bool is_emoji(const ustr& s) {
    bool hit = false;
    for (char32_t c : s) {
        if ((c >= 0x3040 && c <= 0x9FFF) || (c < 0x2000 && c != 0x200D && c != 0x20E3)) return false;
        if ((c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF)) hit = true;
    }
    return hit;
}

// 日付・時刻の候補 (きょう -> 2026/10/07・2026年10月7日・10月7日(水)・令和8年10月7日、いま -> 14:40・14時40分)。
// 端末の時計 (localtime) を使う。テスト用に環境変数 KKC_NOW (UNIX 時刻) で今を決められる。読みが当たらなければ空
std::vector<ustr> date_candidates(const ustr& r) {
    static const std::pair<const char32_t*, int> DAYS[] = {
        {U"きょう", 0}, {U"ほんじつ", 0}, {U"あした", 1}, {U"あす", 1}, {U"みょうにち", 1}, {U"あさって", 2},
        {U"みょうごにち", 2}, {U"しあさって", 3}, {U"やのあさって", 3}, {U"きのう", -1}, {U"さくじつ", -1}, {U"おととい", -2},
        {U"おとつい", -2}, {U"いっさくじつ", -2}, {U"さきおととい", -3}};
    static const std::pair<const char32_t*, int> YEARS[] = {
        {U"ことし", 0}, {U"ほんねん", 0}, {U"こんねん", 0}, {U"らいねん", 1}, {U"みょうねん", 1}, {U"きょねん", -1}, {U"さくねん", -1}};
    static const std::pair<const char32_t*, int> MONTHS[] = {{U"こんげつ", 0}, {U"らいげつ", 1}, {U"せんげつ", -1}};
    static const char32_t* WD = U"日月火水木金土";
    std::vector<ustr> out;
    time_t now = std::time(nullptr);
    if (const char* s = std::getenv("KKC_NOW")) {   // 試験用の今の時刻 (strtoll は使わない: glibc 2.38 を要求して Linux 版の動く範囲が狭まる)
        long long v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        now = time_t(v);
    }
    auto local = [](time_t t) {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return tm;
    };
    auto num = [](int v) { return digits(v, false); };
    auto two = [](int v) { return ustr(1, char32_t(U'0' + v / 10)) + char32_t(U'0' + v % 10); };
    // 令和は 2019 年 5 月 1 日から
    auto era = [&](const std::tm& t) -> ustr {
        const int y = t.tm_year + 1900;
        if (y > 2019 || (y == 2019 && t.tm_mon >= 4)) return U"令和" + (y == 2019 ? ustr(U"元") : num(y - 2018)) + U"年";
        return ustr();
    };
    for (const auto& d : DAYS) {
        if (r != d.first) continue;
        const std::tm t = local(now + time_t(d.second) * 86400);
        const int y = t.tm_year + 1900, m = t.tm_mon + 1, dd = t.tm_mday;
        // 提案の帯に並べる順 (2026-10-09 ユーザー: 2026/10/9 2026年10月9日 10/9 10月9日 十月九日)
        out.push_back(num(y) + U"/" + num(m) + U"/" + num(dd));
        out.push_back(num(y) + U"年" + num(m) + U"月" + num(dd) + U"日");
        out.push_back(num(m) + U"/" + num(dd));
        out.push_back(num(m) + U"月" + num(dd) + U"日");
        out.push_back(num(m) + U"月" + num(dd) + U"日(" + ustr(1, WD[t.tm_wday]) + U")");
        out.push_back(kanji(m) + U"月" + kanji(dd) + U"日");
        const ustr e = era(t);
        if (!e.empty()) out.push_back(e + num(m) + U"月" + num(dd) + U"日");
        out.push_back(num(y) + U"/" + two(m) + U"/" + two(dd));
        return out;
    }
    if (r == U"いま" || r == U"げんざい") {
        const std::tm t = local(now);
        out.push_back(two(t.tm_hour) + U":" + two(t.tm_min));
        out.push_back(num(t.tm_hour) + U"時" + num(t.tm_min) + U"分");
        const int h12 = t.tm_hour % 12;
        out.push_back(ustr(t.tm_hour < 12 ? U"午前" : U"午後") + num(h12) + U"時" + num(t.tm_min) + U"分");
        return out;
    }
    for (const auto& d : YEARS) {
        if (r != d.first) continue;
        std::tm t = local(now);
        t.tm_year += d.second;
        t.tm_mon = 11;   // 和暦はその年の年末で (2019 年は 令和元年)
        out.push_back(num(t.tm_year + 1900) + U"年");
        const ustr e = era(t);
        if (!e.empty()) out.push_back(e);
        return out;
    }
    for (const auto& d : MONTHS) {
        if (r != d.first) continue;
        const std::tm t = local(now);
        int y = t.tm_year + 1900, m = t.tm_mon + 1 + d.second;
        if (m == 0) { m = 12; y--; }
        if (m == 13) { m = 1; y++; }
        out.push_back(num(m) + U"月");
        out.push_back(num(y) + U"年" + num(m) + U"月");
        return out;
    }
    // ---- 相対的な日付・曜日・和暦・時刻 (提案の帯に出す。2026-10-09)
    // 日数の読み: ついたち・ふつか … とおか・はつか、それ以外は かなの数 + にち (じゅうごにち)
    auto day_num = [&](const ustr& w, int& d) -> bool {
        static const std::pair<const char32_t*, int> SP[] = {
            {U"ついたち", 1}, {U"ふつか", 2}, {U"みっか", 3}, {U"よっか", 4}, {U"いつか", 5}, {U"むいか", 6}, {U"なのか", 7},
            {U"ようか", 8}, {U"ここのか", 9}, {U"とおか", 10}, {U"じゅうよっか", 14}, {U"はつか", 20}, {U"にじゅうよっか", 24}};
        for (auto& x : SP) if (w == x.first) { d = x.second; return true; }
        if (ends_with(w, U"にち")) {
            int64_t v;
            if (parse_num(w.substr(0, w.size() - 2), v) && v >= 1 && v <= 31) { d = int(v); return true; }
        }
        return false;
    };
    auto put_date = [&](time_t t) {
        const std::tm tm = local(t);
        const int y = tm.tm_year + 1900, m = tm.tm_mon + 1, dd = tm.tm_mday;
        out.push_back(num(m) + U"/" + num(dd) + U"(" + ustr(1, WD[tm.tm_wday]) + U")");
        out.push_back(num(m) + U"月" + num(dd) + U"日(" + ustr(1, WD[tm.tm_wday]) + U")");
        out.push_back(num(y) + U"/" + num(m) + U"/" + num(dd));
        out.push_back(num(y) + U"年" + num(m) + U"月" + num(dd) + U"日");
    };
    // 曜日 (言い回しを広く。2026-10-09 ユーザー): 週で言う (さらいしゅう・らいしゅう・こんしゅう・せんしゅう・せんせんしゅう)、
    // 次・前で言う (つぎの・こんどの・こんどの・このつぎの / まえの・このまえの・さきの)、曜日だけ (きんようび = 今日か次の金曜)。
    // 曜日は げつようび・げつよう・げつ (前に週・次の言葉があるときだけ 1 字の げつ も)
    {
        static const std::pair<const char32_t*, int> WEEKS[] = {{U"さらいしゅう", 14}, {U"らいしゅう", 7}, {U"こんしゅう", 0},
                                                                  {U"せんせんしゅう", -14}, {U"せんしゅう", -7}};
        static const std::pair<const char32_t*, int> NEXT[] = {{U"このつぎの", 1}, {U"つぎの", 1}, {U"こんどの", 1}, {U"きたる", 1},
                                                                 {U"このまえの", -1}, {U"まえの", -1}, {U"さきの", -1}, {U"さる", -1}};
        static const char32_t* WDR[] = {U"にち", U"げつ", U"か", U"すい", U"もく", U"きん", U"ど"};
        auto weekday = [&](const ustr& rest, bool short_ok) -> int {
            for (int w = 0; w < 7; w++) {
                const ustr k = WDR[w];
                if (rest == k + U"ようび" || rest == k + U"よう" || (short_ok && rest == k && k.size() >= 2)) return w;
            }
            return -1;
        };
        const std::tm t0 = local(now);
        for (auto& wk : WEEKS) {
            if (!starts_with(r, 0, wk.first)) continue;
            ustr rest = r.substr(std::char_traits<char32_t>::length(wk.first));
            if (starts_with(rest, 0, U"の")) rest = rest.substr(1);
            const int w = weekday(rest, true);
            if (w < 0) continue;
            // 週は月曜から (こんしゅうのにちようび = この週の日曜 = 次の日曜)
            const int mon_off = (t0.tm_wday + 6) % 7;   // 今日から見た今週の月曜までの日数
            const int target = (w + 6) % 7;              // 月曜を 0 とした曜日
            put_date(now + time_t(-mon_off + target + wk.second) * 86400);
            return out;
        }
        for (auto& nx : NEXT) {
            if (!starts_with(r, 0, nx.first)) continue;
            const int w = weekday(r.substr(std::char_traits<char32_t>::length(nx.first)), true);
            if (w < 0) continue;
            int d = (w - t0.tm_wday + 7) % 7;            // 次の (今日なら 7 日後)
            if (nx.second > 0) d = d == 0 ? 7 : d;
            else d = -((t0.tm_wday - w + 7) % 7 == 0 ? 7 : (t0.tm_wday - w + 7) % 7);
            put_date(now + time_t(d) * 86400);
            return out;
        }
        {
            const int w = weekday(r, false);   // 曜日だけ: 今日か次のその曜日
            if (w >= 0 && r.size() >= 4) {
                put_date(now + time_t((w - t0.tm_wday + 7) % 7) * 86400);
                return out;
            }
        }
        // 週末 (こんしゅうまつ・らいしゅうまつ・しゅうまつ): 土曜と日曜
        static const std::pair<const char32_t*, int> WE[] = {{U"らいしゅうまつ", 7}, {U"こんしゅうまつ", 0}, {U"しゅうまつ", 0}, {U"せんしゅうまつ", -7}};
        for (auto& we : WE) {
            if (r != we.first) continue;
            const int mon_off = (t0.tm_wday + 6) % 7;
            const time_t sat = now + time_t(-mon_off + 5 + we.second) * 86400;
            put_date(sat);
            const std::tm ts = local(sat), tsu = local(sat + 86400);
            out.insert(out.begin() + 1, num(ts.tm_mon + 1) + U"/" + num(ts.tm_mday) + U"〜" + num(tsu.tm_mon + 1) + U"/" + num(tsu.tm_mday));
            return out;
        }
        // 月末・年末 (げつまつ・こんげつまつ・らいげつまつ・せんげつまつ・ねんまつ)
        static const std::pair<const char32_t*, int> ME[] = {{U"らいげつまつ", 1}, {U"こんげつまつ", 0}, {U"げつまつ", 0}, {U"せんげつまつ", -1}};
        for (auto& me : ME) {
            if (r != me.first) continue;
            std::tm t = t0;
            t.tm_mon += me.second + 1;
            t.tm_mday = 0;   // 前の月の末日
            t.tm_hour = 12;
            put_date(std::mktime(&t));
            return out;
        }
        if (r == U"ねんまつ" || r == U"らいねんまつ") {
            std::tm t = t0;
            t.tm_year += r == U"ねんまつ" ? 0 : 1;
            t.tm_mon = 11;
            t.tm_mday = 31;
            t.tm_hour = 12;
            put_date(std::mktime(&t));
            return out;
        }
    }
    // N 日後・N 日前 (みっかご・とおかまえ・じゅうごにちご)、N 週間後
    for (const auto& suf : {std::pair<const char32_t*, int>{U"ご", 1}, std::pair<const char32_t*, int>{U"あと", 1}, std::pair<const char32_t*, int>{U"まえ", -1}}) {
        if (!ends_with(r, suf.first)) continue;
        const ustr body = r.substr(0, r.size() - std::char_traits<char32_t>::length(suf.first));
        int d;
        if (day_num(body, d) && body.size() >= 3) { put_date(now + time_t(d * suf.second) * 86400); return out; }
        if (ends_with(body, U"しゅうかん")) {
            int64_t v;
            bool sk;
            const ustr wn = body.substr(0, body.size() - 5);
            if ((parse_num(wn, v) || parse(wn, v, sk)) && v >= 1 && v <= 52) { put_date(now + time_t(v * 7 * suf.second) * 86400); return out; }
        }
    }
    // N か月後・N 年後 (さんかげつご・いちねんご)、N 時間後・N 分後 (さんじかんご・じゅっぷんご -> 時刻)
    for (const auto& suf : {std::pair<const char32_t*, int>{U"ご", 1}, std::pair<const char32_t*, int>{U"あと", 1}, std::pair<const char32_t*, int>{U"まえ", -1}}) {
        if (!ends_with(r, suf.first)) continue;
        const ustr body = r.substr(0, r.size() - std::char_traits<char32_t>::length(suf.first));
        auto numof = [&](const ustr& x, int64_t& v) { bool sk; return parse_num(x, v) || parse(x, v, sk) || (x == U"いっ" && (v = 1, true)); };
        int64_t v;
        if ((ends_with(body, U"かげつ") || ends_with(body, U"かげつかん")) && numof(body.substr(0, body.find(U"かげつ")), v) && v >= 1 && v <= 36) {
            // らいげつ と同じく月で (さんかげつご -> 1月・2027年1月)。日まで要るとき用に日付も後ろに
            std::tm t = local(now);
            t.tm_mon += int(v) * suf.second;
            t.tm_hour = 12;
            const time_t tt = std::mktime(&t);
            const std::tm u = local(tt);
            out.push_back(num(u.tm_mon + 1) + U"月");
            out.push_back(num(u.tm_year + 1900) + U"年" + num(u.tm_mon + 1) + U"月");
            put_date(tt);
            return out;
        }
        if (ends_with(body, U"ねん") && numof(body.substr(0, body.size() - 2), v) && v >= 1 && v <= 30) {
            // らいねん と同じく年で (いちねんご -> 2027年・令和9年)。日付も後ろに
            std::tm t = local(now);
            t.tm_year += int(v) * suf.second;
            t.tm_hour = 12;
            const time_t tt = std::mktime(&t);
            const int Y = local(tt).tm_year + 1900;
            out.push_back(num(Y) + U"年");
            if (Y >= 2019) out.push_back(U"令和" + (Y == 2019 ? ustr(U"元") : num(Y - 2018)) + U"年");
            put_date(tt);
            return out;
        }
        int64_t mins = -1;
        if (ends_with(body, U"じかん") && numof(body.substr(0, body.size() - 3), v) && v >= 1 && v <= 48) mins = v * 60;
        else if ((ends_with(body, U"ふん") || ends_with(body, U"ぷん")) && numof(body.substr(0, body.size() - 2), v) && v >= 1 && v <= 600) mins = v;
        if (mins > 0) {
            const std::tm t = local(now + time_t(mins * 60 * suf.second));
            const ustr mm = ustr(1, char32_t(U'0' + t.tm_min / 10)) + char32_t(U'0' + t.tm_min % 10);
            out.push_back(num(t.tm_hour) + U":" + mm);
            out.push_back(num(t.tm_hour) + U"時" + num(t.tm_min) + U"分");
            return out;
        }
    }
    // らいげつ・こんげつ・せんげつ + 日 (らいげつついたち -> 11/1(日))
    {
        static const std::pair<const char32_t*, int> MS[] = {{U"さらいげつ", 2}, {U"らいげつ", 1}, {U"こんげつ", 0}, {U"せんげつ", -1}};
        for (auto& mo : MS) {
            if (!starts_with(r, 0, mo.first)) continue;
            ustr rest = r.substr(std::char_traits<char32_t>::length(mo.first));
            if (starts_with(rest, 0, U"の")) rest = rest.substr(1);
            int d;
            if (rest.empty() || !day_num(rest, d)) continue;
            std::tm t = local(now);
            t.tm_mon += mo.second;
            t.tm_mday = d;
            t.tm_hour = 12;
            put_date(std::mktime(&t));
            return out;
        }
    }
    // 和暦 -> 西暦 (れいわはちねん -> 2026年)、西暦 -> 和暦 (にせんにじゅうろくねん -> 令和8年)
    {
        static const std::pair<const char32_t*, int> ERAS[] = {{U"れいわ", 2018}, {U"へいせい", 1988}, {U"しょうわ", 1925}, {U"たいしょう", 1911}, {U"めいじ", 1867}};
        static const char32_t* ERAK[] = {U"令和", U"平成", U"昭和", U"大正", U"明治"};
        if (ends_with(r, U"ねん")) {
            for (int i = 0; i < 5; i++) {
                if (!starts_with(r, 0, ERAS[i].first)) continue;
                const ustr body = r.substr(std::char_traits<char32_t>::length(ERAS[i].first), r.size() - 2 - std::char_traits<char32_t>::length(ERAS[i].first));
                int64_t v = 0;
                if (body == U"がん") v = 1;
                else if (!parse_num(body, v)) continue;
                if (v < 1 || v > 64) continue;
                out.push_back(num(int(ERAS[i].second + v)) + U"年");
                out.push_back(ustr(ERAK[i]) + (v == 1 ? ustr(U"元") : num(int(v))) + U"年(" + num(int(ERAS[i].second + v)) + U"年)");
                return out;
            }
            int64_t y;
            if (parse_num(r.substr(0, r.size() - 2), y) && y >= 1868 && y <= 2100) {
                const int Y = int(y);
                ustr era;
                if (Y >= 2019) era = U"令和" + (Y == 2019 ? ustr(U"元") : num(Y - 2018));
                else if (Y >= 1989) era = U"平成" + (Y == 1989 ? ustr(U"元") : num(Y - 1988));
                else if (Y >= 1926) era = U"昭和" + (Y == 1926 ? ustr(U"元") : num(Y - 1925));
                else if (Y >= 1912) era = U"大正" + (Y == 1912 ? ustr(U"元") : num(Y - 1911));
                else era = U"明治" + (Y == 1868 ? ustr(U"元") : num(Y - 1867));
                out.push_back(era + U"年");
                out.push_back(num(Y) + U"年(" + era + U"年)");
                return out;
            }
        }
    }
    // 時刻: (ごぜん|ごご)? N じ (M ふん|ぷん|はん)? -> 15:00・15時・午後3時
    {
        ustr t = r;
        int ampm = 0;
        if (starts_with(t, 0, U"ごぜん")) { ampm = 1; t = t.substr(3); }
        else if (starts_with(t, 0, U"ごご")) { ampm = 2; t = t.substr(2); }
        // 時の じ (じゅう の じ と区別: 前が数として読める じ を後ろから探す)
        // 時の数: よじ・くじ (よ・く の 1 字)、じゅうよじ (よ = よん)
        auto hour = [&](const ustr& x, int64_t& v) -> bool {
            if (x == U"よ") { v = 4; return true; }
            if (x == U"く") { v = 9; return true; }
            if (parse_num(x, v)) return true;
            return ends_with(x, U"よ") && parse_num(x + U"ん", v);
        };
        size_t j = ustr::npos;
        for (size_t q = t.size(); q-- > 1;) {
            int64_t tmp;
            if (t[q] == U'じ' && hour(t.substr(0, q), tmp)) { j = q; break; }
        }
        if (j != ustr::npos && j > 0) {
            int64_t h, mi = 0;
            const ustr hs = t.substr(0, j), rest = t.substr(j + 1);
            bool ok = hour(hs, h);
            if (ok && !rest.empty()) {
                bool sk;
                if (rest == U"はん") mi = 30;
                else if (ends_with(rest, U"ふん") || ends_with(rest, U"ぷん")) {
                    const ustr ms = rest.substr(0, rest.size() - 2);
                    ok = (parse_num(ms, mi) || parse(ms, mi, sk) || (ms == U"いっ" && (mi = 1, true))) && mi < 60;
                } else ok = false;
            }
            if (ok && h >= 0 && h <= 24 && (ampm || !rest.empty() || hs.size() >= 2)) {
                int H = int(h);
                if (ampm == 2 && H < 12) H += 12;
                const ustr mm = ustr(1, char32_t(U'0' + mi / 10)) + char32_t(U'0' + mi % 10);
                out.push_back(num(H) + U":" + mm);
                out.push_back(num(H) + U"時" + (mi ? num(int(mi)) + U"分" : ustr()));
                if (ampm) out.push_back(ustr(ampm == 1 ? U"午前" : U"午後") + num(int(h)) + U"時" + (mi ? num(int(mi)) + U"分" : ustr()));
                return out;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- M5: 個人化 (学習の網 kkcnative2 の add_variant_edges・style_marks と同じ決まり)
static const int32_t VAR_PENALTY = 300;   // 足した書き方の候補のコストの上乗せ (辞書だけの下書きは元の書き方のまま)

static int count_bucket(int n) { return n <= 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n <= 4 ? 3 : n <= 9 ? 4 : n <= 19 ? 5 : 6; }
static int rate_bucket(double s) { return s < .2 ? 1 : s < .4 ? 2 : s < .6 ? 3 : s < .8 ? 4 : 5; }

static void personalize(kkc_engine* e, std::vector<Edge>& E) {
    if (!e->var.empty()) {   // 網に書き方の違う候補を足す (同じ区間に同じ表記があれば足さない)
        std::unordered_set<ustr> have;
        auto key = [](const Edge& x) { ustr k = x.surf; k.push_back(char32_t(0x110000 + x.s)); k.push_back(char32_t(0x110000 + x.e)); return k; };
        for (auto& x : E) have.insert(key(x));
        const size_t n0 = E.size();
        for (size_t i = 0; i < n0; i++) {
            if (E[i].kind == K_TYPO || E[i].kind == K_LATIN) continue;
            auto v = e->var.find(E[i].surf);
            if (v == e->var.end()) continue;
            Edge ne = E[i];
            ne.surf = v->second.other;
            ne.kind = K_DICT;
            ne.cost = E[i].cost + VAR_PENALTY;
            if (have.insert(key(ne)).second) E.push_back(std::move(ne));
        }
    }
    for (auto& x : E) {
        int um = 0, f = 0, c = 0;
        auto u = e->umarks.find(x.surf);
        if (u != e->umarks.end()) um = u->second;
        auto v = e->var.find(x.surf);
        if (v != e->var.end()) {
            const auto& vf = v->second;
            auto fi = e->fam.find(int64_t(vf.lemma) * 2 + (vf.kanji ? 1 : 0));
            if (fi != e->fam.end()) f = count_bucket(fi->second);
            if (vf.cls > 0 && vf.cls < 8) {
                const int a = e->cls_n[vf.cls][vf.kanji ? 1 : 0], b = e->cls_n[vf.cls][vf.kanji ? 0 : 1];
                if (a + b >= 5) c = rate_bucket(double(a) / double(a + b));   // 5 回以上書いた種類だけ
            }
        }
        x.mark = um + 32 * (f * 6 + c);
    }
}

KKC_API int kkc_typo_spans(kkc_engine* e, int32_t* out, int cap) {
    if (!e) return 0;
    int n = 0;
    for (int v : e->typo_spans) { if (n >= cap) break; out[n++] = v; }
    return n / 2;
}

KKC_API int kkc_personal(kkc_engine* e) { return e && e->m.n_style > 0 ? 1 : 0; }

// 確定した文 text を読み reading の網の上でたどり (学習の網の正解の経路と同じ決め方)、通った語のうち書き方の対の表にある表記を
// 0 区切りで out に。書き方の記録 (StyleLog) に数える語を決める。経路が無ければ 0
KKC_API int kkc_path_variants(kkc_engine* e, const uint16_t* reading, int nr, const uint16_t* text, int nt, uint16_t* out, int cap) {
    if (!e || e->var.empty() || nr <= 0 || nt <= 0) return 0;
    const ustr r = from16(reading, size_t(nr)), t = from16(text, size_t(nt));
    std::vector<Edge> E = edges(e->lex, r, &e->user);
    personalize(e, E);
    const int n = int(r.size()), G = int(t.size());
    std::vector<std::vector<int>> by_start(size_t(n) + 1);
    for (int i = 0; i < int(E.size()); i++) by_start[size_t(E[i].s)].push_back(i);
    // (読みの位置, 文の位置) ごとに来た辺 (-1 = まだ)。左から
    const size_t W = size_t(G) + 1;
    std::vector<int> from(size_t(n + 1) * W, -2);
    from[0] = -1;
    for (int p = 0; p < n; p++)
        for (int k = 0; k < G; k++) {
            if (from[size_t(p) * W + size_t(k)] == -2) continue;
            for (int i : by_start[size_t(p)]) {
                if (!starts_with(t, size_t(k), E[size_t(i)].surf)) continue;
                const size_t to = size_t(E[size_t(i)].e) * W + size_t(k) + E[size_t(i)].surf.size();
                if (from[to] == -2) from[to] = i;
            }
        }
    if (from[size_t(n) * W + size_t(G)] == -2) return 0;
    std::vector<ustr> words;
    for (int p = n, k = G; p > 0 || k > 0;) {
        const int i = from[size_t(p) * W + size_t(k)];
        if (i < 0) break;
        if (e->var.count(E[size_t(i)].surf)) words.push_back(E[size_t(i)].surf);
        p = E[size_t(i)].s;
        k -= int(E[size_t(i)].surf.size());
    }
    int w = 0;
    std::vector<uint16_t> s;
    for (auto it = words.rbegin(); it != words.rend(); ++it) {
        s.clear();
        to16(*it, s);
        if (w + int(s.size()) + 1 > cap) break;
        for (uint16_t c : s) out[w++] = c;
        out[w++] = 0;
    }
    return w;
}

KKC_API int kkc_set_variants(kkc_engine* e, const uint16_t* tsv, int n) {
    if (!e) return -1;
    e->var.clear();
    std::unordered_map<ustr, int> lem;
    ustr line;
    auto flush = [&]() {
        std::vector<ustr> col(1);
        for (char32_t ch : line) { if (ch == U'\t') col.emplace_back(); else col.back().push_back(ch); }
        if (col.size() >= 4 && !col[0].empty() && !col[1].empty()) {
            const int id = lem.emplace(col[2], int(lem.size())).first->second;
            int cl = 0;
            for (char32_t ch : col[3]) if (ch >= U'0' && ch <= U'9') cl = cl * 10 + int(ch - U'0');
            e->var.emplace(col[0], kkc_engine::VarForm{col[1], id, cl, false});
            e->var.emplace(col[1], kkc_engine::VarForm{col[0], id, cl, true});
        }
        line.clear();
    };
    const ustr all = from16(tsv, size_t(n));
    for (char32_t ch : all) { if (ch == U'\n') flush(); else if (ch != U'\r') line.push_back(ch); }
    if (!line.empty()) flush();
    e->kcache.clear(); e->K.clear();
    return int(e->var.size());
}

KKC_API int kkc_variant_info(kkc_engine* e, const uint16_t* s, int n, int32_t* out3) {
    if (!e || !out3) return 0;
    auto v = e->var.find(from16(s, size_t(n)));
    if (v == e->var.end()) return 0;
    out3[0] = v->second.lemma; out3[1] = v->second.cls; out3[2] = v->second.kanji ? 1 : 0;
    return 1;
}

KKC_API void kkc_set_marks(kkc_engine* e, const uint16_t* surfs, int n, const int32_t* marks, int nm) {
    if (!e) return;
    e->umarks.clear();
    ustr cur;
    int k = 0;
    const ustr all = from16(surfs, size_t(n));
    for (char32_t ch : all) {
        if (ch == 0) { if (!cur.empty() && k < nm && marks[k] > 0) e->umarks[cur] = std::min(marks[k], 31); k++; cur.clear(); }
        else cur.push_back(ch);
    }
}

KKC_API void kkc_set_style(kkc_engine* e, const int32_t* fam, int nf, const int32_t* cls) {
    if (!e) return;
    e->fam.clear();
    for (int i = 0; i < nf; i++) e->fam[int64_t(fam[3 * i]) * 2 + (fam[3 * i + 1] ? 1 : 0)] = fam[3 * i + 2];   // (語, 漢字か, 回数) × nf
    for (int c = 0; c < 8; c++) for (int k = 0; k < 2; k++) e->cls_n[c][k] = cls ? cls[c * 2 + k] : 0;
}

KKC_API int kkc_convert(kkc_engine* e, const uint16_t* ctx, int nctx, const uint16_t* kana, int nk,
                        int maxout, int use_model, uint16_t* out, int cap) {
    if (!e) return -2;
    const Model& M = e->m;
    auto t0 = std::chrono::steady_clock::now();
    ustr r = from16(kana, size_t(nk));
    const int n = int(r.size());
    // 打った鍵の列 (kkc_set_keys) は直後の 1 回の変換だけに効く。ここで取り出して消す (読みの長さが合わなければ使わない)
    std::vector<uint16_t> keys;
    std::vector<int32_t> span_end;
    if (e->lex.latin && int(e->span_end.size()) == n) {
        keys.swap(e->keys);
        span_end.swap(e->span_end);
    }
    e->keys.clear();
    e->span_end.clear();
    std::vector<uint16_t> rctx;   // 後ろの文脈も直後の 1 回だけ
    rctx.swap(e->rctx);
    std::vector<Edge> E = edges(e->lex, r, &e->user);
    kata_continuation_edges(e->lex, from16(ctx, size_t(nctx)), r, E);    // 確定済みのカタカナ語の続き (フリッ + く)
    kanji_continuation_edges(e->lex, from16(ctx, size_t(nctx)), r, E);   // 確定済みの漢字の語の続き (直滑 + こう)
    if (!keys.empty()) {
        std::vector<Edge> lat;
        auto latin_ch = [](uint16_t c) { return c == u' ' || (c < 0x80 && std::isalpha(int(c))); };
        const bool left_latin = nctx > 0 && latin_ch(ctx[nctx - 1]), right_latin = !rctx.empty() && latin_ch(rctx[0]);
        if (e->latin) latin_edges(e->lex, keys, span_end, n, r, e->user_latin, lat, left_latin, right_latin);
        // 読みに英字が残った所 (ローマ字の打ち間違い: arigstou -> ありgsとう) は、鍵の列から 1 打戻した読みで打ち間違いの辺を作る。
        // かなだけの区間の打ち間違いは edges() が読みから作っている
        if (e->lex.typo) {
            typo::Ctx tctx;
            typo::build_ctx(e->lex, E, n, tctx);
            std::vector<Edge> ty;
            const int W = 6;   // 英字の前後 6 字まで
            for (int i = 0; i < n;) {
                if (is_kana(r[size_t(i)])) { i++; continue; }
                int j = i;
                while (j < n && !is_kana(r[size_t(j)])) j++;
                const int w = std::max(0, i - W), we = std::min(n, j + W);
                const int ks = w == 0 ? 0 : span_end[size_t(w - 1)], ke = we == n ? int(keys.size()) : span_end[size_t(we - 1)];
                std::string rom;
                bool ok = ke - ks >= 2;
                for (int k = ks; ok && k < ke; k++) {
                    uint16_t c = keys[size_t(k)];
                    if (c >= u'A' && c <= u'Z') c = uint16_t(c + 32);
                    if ((c >= u'a' && c <= u'z') || c == u'-') rom.push_back(char(c));
                    else ok = false;   // 記号・数字の混じる所は見ない
                }
                if (ok) typo::add_edges(e->lex, r.substr(size_t(w), size_t(we - w)), w, E, tctx, ty, &rom);
                i = j;
            }
            for (auto& ed : ty) lat.push_back(std::move(ed));
        }
        for (auto& ed : lat) E.push_back(std::move(ed));
    }
    if (M.n_style > 0) personalize(e, E);   // M5: 書き方の違う候補と、辺ごとの印
    e->times[0] = ms_since(t0);
    std::vector<ustr> res;
    e->suggest.clear();
    e->typo_spans.clear();
    e->last_u.clear();
    e->last_segs.clear();
    tl_paths.clear();
    bool done = false;
    if (use_model == 2 && n >= CASCADE_MAX_LEN) use_model = 1;
    if (!use_model || n == 0 || use_model == 2) {
        // 辞書だけ。use_model = 2 (段階式) なら、1 位と 2 位のコストの差が読み 1 字あたり CASCADE_PER_CHAR 以上
        // (辞書が迷っていない) ときだけこれで決め、そうでなければモデルで変換し直す
        for (int i = 1; i < 5; i++) e->times[i] = 0;
        t0 = std::chrono::steady_clock::now();
        // 段階式の判定は上位 3 で足りる (上位 10 は重い。迷っていればどのみちモデルで変換し直すので無駄になる)
        const int k0 = use_model == 2 && n > 0 ? CASCADE_K : 10;
        // 辞書だけの変換では打ち間違いの辺を使わない (文脈を読まずに直すと、正しい語を辞書のコストだけで別の語にしてしまう)
        std::vector<Edge> Ed;
        for (const Edge& ed : E) if (ed.kind != K_TYPO) Ed.push_back(ed);
        const bool strip = Ed.size() != E.size();
        nbest(e->lex, strip ? Ed : E, n, nullptr, 1.0, 1.0, k0, k0 == 10 ? maxout : CASCADE_K, res, &e->last_segs, e->last_cost);
        done = use_model != 2 || n == 0 || (e->last_cost[1] - e->last_cost[0]) >= CASCADE_PER_CHAR * n;
        // 辞書の上位に英字の語が混ざるとき (ずーむ: ズーム / Zoom) は、どちらかを文脈で決めるので辞書だけで決めない。
        // 辞書のコストはカタカナが勝ちやすく、段階式で辞書に任せると英字交じりベンチの L 語が 60% -> 40% に落ちていた。
        // 略語 (英大文字・数字だけで 5 字まで: GDP・IPO) は辞書のほうが当たるので、辞書に任せる
        if (done && use_model == 2)
            for (const ustr& c : res)
                if (has_latin_word(c)) done = false;
        if (done && k0 != 10) nbest(e->lex, strip ? Ed : E, n, nullptr, 1.0, 1.0, 10, maxout, res, &e->last_segs, e->last_cost);
        e->times[4] = ms_since(t0);
    }
    if (!done) {
        // 入力: [文脈] SEP [読み] (SEP [下書き])
        t0 = std::chrono::steady_clock::now();
        ustr c = from16(ctx, size_t(nctx));
        if (int(c.size()) > M.ctx) c = c.substr(c.size() - M.ctx);
        std::vector<int> tok, seg;
        for (char32_t ch : c) { tok.push_back(M.vid(ch)); seg.push_back(0); }
        tok.push_back(SEP); seg.push_back(0);
        const int rstart = int(tok.size());
        for (char32_t ch : r) { tok.push_back(M.vid(ch)); seg.push_back(1); }
        if (M.draft) {
            ustr dr = best_path(e->lex, E, n);
            int room = M.max_len - int(tok.size()) - 1;
            int lim = std::min(M.draft_max, std::max(room, 0));
            if (int(dr.size()) > lim) dr.resize(size_t(lim));
            if (room > 0) {
                tok.push_back(SEP); seg.push_back(2);
                for (char32_t ch : dr) { tok.push_back(M.vid(ch)); seg.push_back(2); }
            }
        }
        // 後ろの文脈 (S8 のモデル: 区分 3)。入力の最後に置く。位置の表に入りきらない分は削る (学習と同じ)
        if (M.n_seg > 3 && !rctx.empty()) {
            const int room = M.max_len - int(tok.size()) - 1;
            if (room > 0) {
                const ustr rc = from16(rctx.data(), std::min(rctx.size(), size_t(room)));
                tok.push_back(SEP); seg.push_back(3);
                for (char32_t ch : rc) { tok.push_back(M.vid(ch)); seg.push_back(3); }
            }
        }
        if (int(tok.size()) > M.max_len) return -3;   // 読みが長すぎる
        e->times[1] = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        encode(e, tok, seg);
        e->times[2] = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        scores(e, rstart, n, E, e->last_u);
        if (e->typo_bias != 0)
            for (size_t i = 0; i < E.size(); i++)
                if (E[i].kind == K_TYPO) e->last_u[i] -= e->typo_bias;
        if (e->latin_promote > 0) {   // 点数の単位はコスト 1000 = 1
            const float pen = float(e->latin_promote * std::clamp(n, 1, 12) / 1000.0);
            for (size_t i = 0; i < E.size(); i++)
                if (E[i].kind == K_LATIN) e->last_u[i] -= pen;
        }
        e->times[3] = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        bool has_typo = false;
        for (const Edge& ed : E) has_typo = has_typo || ed.kind == K_TYPO;
        if (e->suggest_mode && has_typo) {
            // 提案: 打ち間違いの辺を使ったいちばんよい変換を別に出し、候補は辺を使わない (打ったとおりの) 変換だけにする
            std::vector<ustr> ty;
            double tyc[2] = {0, 0};
            std::vector<SegInfo> tsegs;
            nbest(e->lex, E, n, e->last_u.data(), M.beta, M.gamma, 1, 1, ty, &tsegs, tyc);
            // 直した辺の読みの区間 (打った綴りの帯で、直した鍵だけ色を変えるため)
            if (!ty.empty()) {
                size_t ci = 0;
                int s0 = 0;
                for (auto& sg : tsegs) {
                    ustr sf;
                    for (int u16n = 0; ci < ty[0].size() && u16n < sg.l16; ci++) { sf.push_back(ty[0][ci]); u16n += ty[0][ci] > 0xFFFF ? 2 : 1; }
                    for (const Edge& ed : E)
                        if (ed.kind == K_TYPO && ed.s == s0 && ed.e == sg.e && ed.surf == sf) { e->typo_spans.push_back(s0); e->typo_spans.push_back(sg.e); break; }
                    s0 = sg.e;
                }
            }
            std::vector<float> lit = e->last_u;
            for (size_t i = 0; i < E.size(); i++)
                if (E[i].kind == K_TYPO) lit[i] = -1e4f;
            nbest(e->lex, E, n, lit.data(), M.beta, M.gamma, 10, maxout, res, &e->last_segs, e->last_cost);
            // 打ったままの道より直した道がはっきり良い (1 字あたり typo_promote 以上) なら、明らかな打ち間違い: 直した形を候補の 1 位に
            // (打ったままの形は 2 位に残る)。そうでなければ提案に (ユーザー 2026-10-14「明らかに間違ってる誤字は 1 位に」)
            // 1 字あたりの差。長い読みで割ると、前の方の明らかな打ち間違いが、後ろを打ち足すほど薄まって直らなくなった
            // (ユーザー 2026-10-23「長く打ちすぎると前の方の誤字訂正が戻ってしまう」)。割るのは 12 字まで
            e->typo_margin = (e->last_cost[0] - tyc[0]) / std::clamp(n, 1, 12);
            if (!ty.empty() && !res.empty() && ty[0] != res[0]) {
                if (e->typo_margin >= e->typo_promote) {
                    res.erase(std::remove(res.begin(), res.end(), ty[0]), res.end());
                    res.insert(res.begin(), ty[0]);
                    // 1 位にした直しは種類 8 で知らせる (呼ぶ側が “ ” で囲んで、直した形だと分かるように。提案の帯には出さない)
                    e->suggest.push_back({KKC_SUGGEST_TYPO_TOP, ty[0]});
                } else {
                    e->suggest.push_back({KKC_SUGGEST_TYPO, ty[0]});
                }
            }
        } else {
            nbest(e->lex, E, n, e->last_u.data(), M.beta, M.gamma, 10, maxout, res, &e->last_segs, e->last_cost);
        }
        if (!res.empty()) {
            // アラビア数字を 1 位、元の漢数字を 2 位、カンマつき (5,000兆円) を 3 位に
            const ustr ar = arabicize(r, res[0], e->last_segs);
            const ustr arc = with_commas_in(ar.empty() ? res[0] : ar);
            auto place = [&](const ustr& c, size_t rank) {
                auto it = std::find(res.begin(), res.end(), c);
                if (it != res.end()) res.erase(it);
                res.insert(res.begin() + std::min(rank, res.size()), c);
            };
            if (!ar.empty()) place(ar, 0);
            if (!arc.empty() && std::find(res.begin(), res.begin() + std::min<size_t>(3, res.size()), arc) == res.begin() + std::min<size_t>(3, res.size()))
                place(arc, 2);
            if (int(res.size()) > maxout) res.resize(size_t(maxout));
        }
        if (n <= SHORT_READING && !res.empty()) {
            // 短い読み (単漢字・2 字の語) は、文脈の手がかりが少なく、採点器の 2 位以下の並びが当てにならない
            // (かん: 上位 20 に「間」が無い)。1 位〜SHORT_KEEP_MODEL 位は文脈を読む採点器のまま (神/紙/髪の使い分け)、
            // その後ろは辞書のコストの順 (Mozc と同じ並び)、最後に採点器の残り
            // 辞書の順は、読み全体で 1 語の語 (Mozc の単語の候補) を、文頭・文末とのつなぎを足したコストの順に先に。
            // 語のつなぎ (は死・は歯) はその後ろ
            std::vector<std::pair<int, ustr>> whole;
            for (const Edge& ed : E)
                if (ed.s == 0 && ed.e == n)
                    whole.push_back({e->lex.conn(0, ed.lid) + ed.cost + e->lex.conn(ed.rid, 0), ed.surf});
            std::stable_sort(whole.begin(), whole.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<ustr> dict, merged;
            nbest(e->lex, E, n, nullptr, 1.0, 1.0, 10, maxout, dict);
            std::unordered_set<ustr> seen;
            auto add = [&](const ustr& s) { if (int(merged.size()) < maxout && seen.insert(s).second) merged.push_back(s); };
            for (size_t i = 0; i < res.size() && int(i) < SHORT_KEEP_MODEL; i++) add(res[i]);
            for (auto& w : whole) add(w.second);
            for (auto& s : res) add(s);
            for (auto& s : dict) add(s);
            res.swap(merged);
        }
        e->times[4] = ms_since(t0);
    }
    // 読み全体が式なら (えっくすのにじょうぷらすいち)、その式 (x²+1) を 2 位までに。読み 1 つの記号 (かける -> ×) は 5 位までに
    {
        auto put = [&](const ustr& c, size_t rank) {
            auto it = std::find(res.begin(), res.end(), c);
            if (it != res.end() && size_t(it - res.begin()) <= rank) return;
            if (it != res.end()) res.erase(it);
            res.insert(res.begin() + std::min(rank, res.size()), c);
            if (int(res.size()) > maxout) res.resize(size_t(maxout));
        };
        const ustr mx = mathx::whole(r);
        if (!mx.empty()) put(mx, 1);
        // 読み全体が 100 以上の数 + 短い語 (にせんちょうえん・さんぜんごひゃくえん) で、辞書の 1 位がアラビア数字 (2000兆円) なら、
        // それを 1 位に。モデルは かなの数 -> アラビア数字 を教材で見ておらず、「2千長円」「さん前後100円」を上にしていた。
        // 辞書の 1 位が数でないとき (ひゃくしょう -> 百姓) は何もしない
        if (use_model && n >= 3) {
            size_t ne = 0;
            int64_t v = 0;
            for (size_t b : boundaries(r, 0)) {
                int64_t x;
                bool sk;
                if (b >= 2 && parse(r.substr(0, b), x, sk) && !sk) { ne = b; v = x; }
            }
            if (ne && v >= 100 && size_t(n) - ne <= 3) {
                std::vector<ustr> dict;
                nbest(e->lex, E, n, nullptr, 1.0, 1.0, 10, 1, dict);
                if (!dict.empty() && !dict[0].empty() && dict[0][0] >= U'1' && dict[0][0] <= U'9') {
                    put(dict[0], 0);
                    const ustr dc = with_commas_in(dict[0]);   // カンマつき (4,000兆円) も 3 位までに
                    if (!dc.empty()) put(dc, 2);
                }
            }
        }
        // 英カタカナ辞書: 読み全体が一般の外来語なら、その英単語を 4 位に (いんじぇくしょん → injection)
        {
            uint32_t a, b;
            if (e->lex.find(reinterpret_cast<const uint16_t*>(kana), nk, a, b))
                for (uint32_t j = a, k = 0; j < b && k < 2; j++) {
                    const Ent en = e->lex.ent(j);
                    if (en.cost < ENG_COST) continue;
                    // 提案モードでは英単語は提案の帯へ (候補の帯をすっきりさせる)
                    if (e->suggest_mode) e->suggest.push_back({KKC_SUGGEST_ENGLISH, from16(e->lex.s_blob + en.soff, en.slen)});
                    else put(from16(e->lex.s_blob + en.soff, en.slen), 3);
                    k++;
                }
        }
        for (const auto& sy : mathx::SYMBOLS)
            if (r == sy.first) put(sy.second, 4);
        // 絵文字: 読み全体に当たる絵文字のうちコストの安い 2 つ (代表: ねこ -> 🐈、続いて 🐱) を 4・5 位に。
        // 辞書の絵文字はコストが高く、そのままでは「根子」「ね娘」のような候補より下 (11 位以下) に沈んでいた
        {
            uint32_t a, b;
            std::vector<std::pair<int32_t, ustr>> em;
            if (e->lex.find(reinterpret_cast<const uint16_t*>(kana), nk, a, b))
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = e->lex.ent(j);
                    // 絵文字の帯 (コスト 8000 台: 代表 8300・ほか 8500) だけ。7000 の記号 (☂・☠) は普通の並びのまま
                    if (en.cost < 8000 || en.cost >= 9000) continue;
                    ustr sf = from16(e->lex.s_blob + en.soff, en.slen);
                    if (is_emoji(sf)) em.push_back({en.cost, std::move(sf)});
                }
            std::stable_sort(em.begin(), em.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            // 異体字セレクタ (U+FE0F) の有る無しだけ違う同じ絵文字 (☔ / ☔️) は 1 つに。絵文字で表示される FE0F つきを使う
            auto bare = [](ustr t) { t.erase(std::remove(t.begin(), t.end(), char32_t(0xFE0F)), t.end()); return t; };
            std::vector<ustr> pick;
            for (auto& x : em) {
                const ustr k = bare(x.second);
                bool dup = false;
                for (auto& p2 : pick) dup = dup || bare(p2) == k;
                if (dup) continue;
                ustr best = x.second;
                for (auto& y : em) if (bare(y.second) == k && y.second.size() > best.size()) best = y.second;
                pick.push_back(best);
                if (pick.size() == 2) break;
            }
            for (size_t i = pick.size(); i-- > 0;) put(pick[i], 3);
        }
        // 顔文字 (Mozc の emoticon.tsv、コスト 9300〜9399 の帯。かな・漢字を含まない): 読み全体に当たる一番よく使う 1 つを 6 位に。
        // 読み「かおもじ」なら表の先頭から 6 つを 2 位から
        {
            uint32_t a, b;
            std::vector<std::pair<int32_t, ustr>> km;
            if (e->lex.find(reinterpret_cast<const uint16_t*>(kana), nk, a, b))
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = e->lex.ent(j);
                    if (en.cost < 9300 || en.cost > 9399) continue;
                    ustr sf = from16(e->lex.s_blob + en.soff, en.slen);
                    bool ok = true;
                    for (char32_t c : sf) ok = ok && !((c >= 0x3041 && c <= 0x3096) || (c >= 0x4E00 && c <= 0x9FFF));
                    if (ok) km.push_back({en.cost, std::move(sf)});
                }
            std::stable_sort(km.begin(), km.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            static const ustr KAO = U"かおもじ";
            if (r == KAO) {
                const size_t m = std::min<size_t>(km.size(), 6);
                for (size_t i = m; i-- > 0;) put(km[i].second, 1);
            } else if (!km.empty()) {
                put(km[0].second, 5);
            }
        }
    }
    // 読み全体がアルファベットの読み 3 字以上 (あいえむいー) なら、その略語 (IME) を 2 位までに必ず置く
    // (「アイエムイー」と打つ人はいないので。1 位は辞書・モデルの答えのまま)
    if (n >= 2 * ABBR_MIN && e->lex.abbr_lid >= 0) {
        const ustr ab = whole_abbr(r);
        if (!ab.empty()) {
            auto it = std::find(res.begin(), res.end(), ab);
            if (it == res.end() || it - res.begin() > 1) {
                if (it != res.end()) res.erase(it);
                res.insert(res.begin() + std::min<size_t>(1, res.size()), ab);
                if (int(res.size()) > maxout) res.resize(size_t(maxout));
            }
        }
    }
    // 郵便番号 (1000001・100-0001) なら、住所 (東京都千代田区千代田) を 2 位から、続けて 100-0001・〒100-0001。
    // 住所は辞書に 読み = 数字 7 桁 で入っている (kkc/names/make_zip_list.py。数字の読みは網に入らない)。
    // 無い番号 (ただの 7 桁の数: 1234567) には何も足さない。打ったまま (100-0001) が 1 位ならそのまま
    {
        ustr z;
        for (char32_t c : r) {
            if (c >= U'0' && c <= U'9') z.push_back(c);
            else if (!(z.size() == 3 && (c == U'-' || c == U'ー' || c == U'−'))) { z.clear(); break; }
        }
        std::vector<std::pair<int32_t, ustr>> addr;
        if (z.size() == 7 && (r.size() == 7 || r.size() == 8)) {
            std::vector<uint16_t> k16(z.begin(), z.end());
            uint32_t a, b;
            if (e->lex.find(k16.data(), int(k16.size()), a, b))
                for (uint32_t j = a; j < b; j++) {
                    const Ent en = e->lex.ent(j);
                    addr.push_back({en.cost, from16(e->lex.s_blob + en.soff, en.slen)});
                }
            std::stable_sort(addr.begin(), addr.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        }
        if (!addr.empty() && e->suggest_mode) {   // 提案: 住所を別の出口に (候補には入れない)
            for (size_t i = 0; i < addr.size() && i < 3; i++) e->suggest.push_back({KKC_SUGGEST_ADDRESS, addr[i].second});
            addr.clear();
        }
        if (!addr.empty()) {
            const ustr hy = z.substr(0, 3) + U"-" + z.substr(3);
            std::vector<ustr> add;
            for (auto& x : addr) add.push_back(x.second);
            add.push_back(hy);
            add.push_back(U"〒" + hy);
            size_t at = std::min<size_t>(1, res.size());
            for (const ustr& c : add) {
                auto it = std::find(res.begin(), res.end(), c);
                if (it != res.end() && size_t(it - res.begin()) < at) continue;
                if (it != res.end()) res.erase(it);
                res.insert(res.begin() + std::min(at, res.size()), c);
                at++;
            }
            if (int(res.size()) > maxout) res.resize(size_t(maxout));
        }
    }
    // 小文字のまま打った英字の語 (iphone・github) は、辞書の書き方 (iPhone・GitHub) にした 1 位を先頭に (打ったままは 2 位に)。
    // モデルは英字の続きを 1 字ずつの辺で見て打ったままを選びがちなので (make_case_list.py の語。頭だけ大文字の一般語は入っていない)
    if (!res.empty()) {
        ustr top = res[0];
        for (int i0 = 0; i0 < n;) {
            auto low = [](char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9'); };
            if (!low(r[i0])) { i0++; continue; }
            int i1 = i0;
            bool letter = false;
            while (i1 < n && low(r[i1])) { letter = letter || r[i1] >= U'a'; i1++; }
            if (letter && i1 - i0 >= 2 && (i1 == n || !((r[i1] >= U'A' && r[i1] <= U'Z')))) {
                std::vector<uint16_t> k16(r.begin() + i0, r.begin() + i1);
                uint32_t a, b;
                int32_t bc = 5001;
                ustr best;
                if (e->lex.find(k16.data(), int(k16.size()), a, b))
                    for (uint32_t j = a; j < b; j++) {
                        const Ent en = e->lex.ent(j);
                        if (en.cost < bc) { bc = en.cost; best = from16(e->lex.s_blob + en.soff, en.slen); }
                    }
                const ustr typed = r.substr(size_t(i0), size_t(i1 - i0));
                const size_t at = top.find(typed);
                if (!best.empty() && at != ustr::npos) top = top.substr(0, at) + best + top.substr(at + typed.size());
            }
            i0 = i1;
        }
        if (top != res[0]) {
            auto it = std::find(res.begin(), res.end(), top);
            if (it != res.end()) res.erase(it);
            res.insert(res.begin(), top);
            if (int(res.size()) > maxout) res.resize(size_t(maxout));
        }
    }
    // 読み全体が英字 (英数字キーボードで data・hello) なら、打ったまま・頭だけ大文字・全部大文字・全角も候補に (後ろに足す)
    if (n >= 1 && std::all_of(r.begin(), r.end(), [](char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9'); }) &&
        std::any_of(r.begin(), r.end(), [](char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); })) {
        ustr lo = r, cap, up = r, fw;
        for (auto& c : lo) if (c >= U'A' && c <= U'Z') c += 32;
        for (auto& c : up) if (c >= U'a' && c <= U'z') c -= 32;
        cap = lo;
        if (cap[0] >= U'a' && cap[0] <= U'z') cap[0] -= 32;
        for (char32_t c : r) fw.push_back(c + 0xFEE0);
        for (const ustr& c : {r, lo, cap, up, fw})
            if (std::find(res.begin(), res.end(), c) == res.end()) res.push_back(c);
        if (int(res.size()) > maxout) res.resize(size_t(maxout));
    }
    // 読み全体が数字 (数字パッドで 2000 と打った) なら、打ったままを 1 位に、カンマ・単位まじり・漢数字・全角を 2 位から
    if (n > 0 && n <= 16 && std::all_of(r.begin(), r.end(), [](char32_t c) { return c >= U'0' && c <= U'9'; }) && (r[0] != U'0' || n == 1)) {
        int64_t v = 0;
        for (char32_t c : r) v = v * 10 + int64_t(c - U'0');
        std::vector<ustr> add = {r};
        if (v >= 1000) add.push_back(with_commas(r));
        if (v >= 10000) {
            add.push_back(unit_mixed(v, false));
            add.push_back(unit_mixed(v, true));
        }
        add.push_back(kanji(v));
        add.push_back(digits(v, true));
        size_t at = 0;
        for (const ustr& c : add) {
            auto it = std::find(res.begin(), res.end(), c);
            if (it != res.end() && size_t(it - res.begin()) < at) continue;
            if (it != res.end()) res.erase(it);
            res.insert(res.begin() + std::min(at, res.size()), c);
            at++;
        }
        if (int(res.size()) > maxout) res.resize(size_t(maxout));
    }
    // 日付・時刻 (きょう -> 2026/10/07 …、いま -> 14:40 …) を 3 位から並べる。1・2 位 (今日・本日) はそのまま
    {
        std::vector<ustr> dc = date_candidates(r);
        if (e->suggest_mode) {   // 提案: 日付・時刻を別の出口に
            for (const ustr& c : dc) e->suggest.push_back({KKC_SUGGEST_DATE, c});
            dc.clear();
        }
        size_t at = std::min<size_t>(2, res.size());
        for (const ustr& c : dc) {
            auto it = std::find(res.begin(), res.end(), c);
            if (it != res.end()) res.erase(it);
            res.insert(res.begin() + std::min(at, res.size()), c);
            at++;
        }
        if (int(res.size()) > maxout) res.resize(size_t(maxout));
    }
    // 端末のフォントに無く豆腐 (□) になる字を含む候補は出さない (2026-10-09 HarmonyOS で 𛀂 が豆腐に):
    // 変体仮名・かなの拡張 (U+1B000-1B16F)、ヒエログリフ (U+13000-1345F)、麻雀牌・ドミノ・トランプ (U+1F000-1F0FF、絵文字の 🀄 🃏 は残す)
    {
        const int cf = e->char_filter;
        auto tofu = [cf](const ustr& c) {
            for (char32_t ch : c)
                if (((cf & 1) && ch >= 0x1B000 && ch <= 0x1B16F) || ((cf & 2) && ch >= 0x13000 && ch <= 0x1345F) ||
                    ((cf & 4) && ch >= 0x1F000 && ch <= 0x1F0FF && ch != 0x1F004 && ch != 0x1F0CF))
                    return true;
            return false;
        };
        res.erase(std::remove_if(res.begin(), res.end(), tofu), res.end());
        e->suggest.erase(std::remove_if(e->suggest.begin(), e->suggest.end(), [&](const std::pair<int, ustr>& x) { return tofu(x.second); }),
                         e->suggest.end());
    }
    // 提案モード: 絵文字・顔文字・かっこ・記号だけの候補 (かな・漢字・英数字を含まない) は候補の帯から提案の帯へ。
    // 読みにかなが無い (記号を打った) ときはそのまま。候補が 1 つも残らないときもそのまま
    if (e->suggest_mode && std::any_of(r.begin(), r.end(), [](char32_t c) { return c >= 0x3041 && c <= 0x30FA; })) {
        auto symbolic = [](const ustr& c) {
            // 単位の記号 (℃・%・° …) は文の中で使うものなので、提案に回さず普通の候補に残す (ユーザー 2026-10-14)
            static const ustr UNIT_MARKS = U"℃°%％‰℉Ω㎏㎞㎝㎜㎡㎥¥￥$＄€£";
            if (!c.empty() && std::all_of(c.begin(), c.end(), [](char32_t ch) { return UNIT_MARKS.find(ch) != ustr::npos; })) return false;
            size_t letters = 0;
            for (char32_t ch : c) {
                if ((ch >= 0x3041 && ch <= 0x3096) || (ch >= 0x30A1 && ch <= 0x30FA) || (ch >= 0x4E00 && ch <= 0x9FFF) ||
                    (ch >= U'0' && ch <= U'9') || (ch >= 0xFF10 && ch <= 0xFF19))
                    return false;
                if ((ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z')) letters++;
            }
            // 英字は半分以下なら顔文字 (:D・m(_ _)m・^m^)。それより多いもの (orz・OK) は語として候補に残す
            return !c.empty() && letters * 2 <= c.size();
        };
        std::vector<ustr> keep, moved;
        for (auto& c : res) (symbolic(c) ? moved : keep).push_back(c);
        if (!moved.empty() && !keep.empty()) {
            res.swap(keep);
            for (auto& c : moved) e->suggest.push_back({KKC_SUGGEST_SYMBOL, c});
        }
    }
    // 数字の後の「度」は ℃ の形も候補に (ななじゅうどで -> 70度で の直後に 70℃で。ユーザー 2026-10-14)
    for (size_t i = 0; i < res.size() && i < 3; i++) {
        const ustr& c = res[i];
        size_t p = c.find(U'度');
        if (p == ustr::npos || p == 0 || !((c[p - 1] >= U'0' && c[p - 1] <= U'9') || (c[p - 1] >= 0xFF10 && c[p - 1] <= 0xFF19))) continue;
        ustr v = c;
        v[p] = U'℃';
        if (std::find(res.begin(), res.end(), v) == res.end()) res.insert(res.begin() + long(i) + 1, v);
        break;
    }
    // 確定の後に出す絵文字・顔文字 (KKC_SUGGEST_AFTER、提案の帯には出さない): 1 位の文の語のうち、漢字・カタカナを含む語の読みに
    // 当たる絵文字 (コスト 8000 台) と顔文字 (9300〜9399、読み 3 字以上)。絵文字 8・顔文字 6 まで (もっと出していい、2026-10-14)。文の終わりの語ほど先に。呼ぶ側が確定の後の予測の帯に出す
    // (ユーザー 2026-10-14「文に絵文字の出る語があれば、予測にその絵文字を」。打っている途中の提案にしたのは 10/13 にやめた)
    if (e->suggest_mode && !e->last_segs.empty() && !res.empty()) {
        const ustr top0 = res[0];
        std::vector<std::pair<int, int>> spans;   // (読みの始め, 終わり) と表記の始め
        std::vector<std::pair<size_t, size_t>> surfs;
        int rs = 0;
        size_t ss = 0;
        for (const SegInfo& sg : e->last_segs) {
            size_t se = ss;
            for (int l = 0; se < top0.size() && l < sg.l16;) l += top0[se++] >= 0x10000 ? 2 : 1;
            spans.push_back({rs, sg.e});
            surfs.push_back({ss, se});
            rs = sg.e;
            ss = se;
        }
        std::vector<ustr> em, km;
        for (int k = int(spans.size()) - 1; k >= 0 && (em.size() < 8 || km.size() < 6); k--) {
            const int a0 = spans[size_t(k)].first, b0 = spans[size_t(k)].second;
            const ustr sf = top0.substr(surfs[size_t(k)].first, surfs[size_t(k)].second - surfs[size_t(k)].first);
            bool content = false;
            for (char32_t c : sf) content = content || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x30A1 && c <= 0x30FA);
            if (!content || b0 - a0 < 2) continue;
            std::vector<uint16_t> k16(r.begin() + a0, r.begin() + b0);
            uint32_t la, lb;
            if (!e->lex.find(k16.data(), int(k16.size()), la, lb)) continue;
            std::vector<std::pair<int32_t, ustr>> ce, ck;
            for (uint32_t j = la; j < lb; j++) {
                const Ent en = e->lex.ent(j);
                ustr x = from16(e->lex.s_blob + en.soff, en.slen);
                if (en.cost >= 8000 && en.cost < 9000 && is_emoji(x)) ce.push_back({en.cost, x});
                else if (en.cost >= 9300 && en.cost <= 9399 && b0 - a0 >= 3) {
                    bool ok = true;
                    for (char32_t c : x) ok = ok && !((c >= 0x3041 && c <= 0x3096) || (c >= 0x4E00 && c <= 0x9FFF));
                    if (ok) ck.push_back({en.cost, x});
                }
            }
            std::stable_sort(ce.begin(), ce.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            std::stable_sort(ck.begin(), ck.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            for (auto& x : ce) if (em.size() < 8 && std::find(em.begin(), em.end(), x.second) == em.end()) em.push_back(x.second);
            for (auto& x : ck) if (km.size() < 6 && std::find(km.begin(), km.end(), x.second) == km.end()) km.push_back(x.second);
        }
        for (auto& x : em) e->suggest.push_back({KKC_SUGGEST_AFTER, x});
        for (auto& x : km) e->suggest.push_back({KKC_SUGGEST_AFTER, x});
    }
    // 提案の並び: 打ち間違いの直し・日付・住所・気分の絵文字・記号・英単語の順 (英単語 KYO が日付より前に出ていた)
    {
        auto pri = [](int k) { static const int P[] = {9, 0, 1, 2, 5, 9, 3, 4, 0, 9}; return k >= 0 && k < 10 ? P[k] : 9; };
        std::stable_sort(e->suggest.begin(), e->suggest.end(), [&](const auto& x, const auto& y) { return pri(x.first) < pri(y.first); });
    }
    std::vector<uint16_t> buf;
    for (auto& s : res) { to16(s, buf); buf.push_back(0); }
    if (int(buf.size()) > cap) return -1;
    std::copy(buf.begin(), buf.end(), out);
    return int(res.size());
}

KKC_API double kkc_last_margin(kkc_engine* e) { return e ? e->last_cost[1] - e->last_cost[0] : 0; }

// 直前の kkc_convert の提案 (kkc_set_suggest がオンのとき)。表記を 0 区切りで out に、種類を kinds に。返り値は数 (out が足りなければ -1)
KKC_API void kkc_set_typo_promote(kkc_engine* e, double per_char) { if (e) e->typo_promote = per_char; }
KKC_API void kkc_set_latin(kkc_engine* e, int on) { if (e) e->latin = on != 0; }

// 綴り (英字・数字、小文字) の辞書の英字の語の表記を out に。無い・英字の打ち分けがオフなら 0。返り値は表記の長さ (UTF-16)
KKC_API int kkc_latin_word(kkc_engine* e, const uint16_t* key, int nk, uint16_t* out, int cap) {
    if (!e || !e->latin || nk <= 0) return 0;
    std::string k;
    for (int i = 0; i < nk; i++) {
        if (key[i] >= 0x80) return 0;
        k.push_back(char(key[i] >= u'A' && key[i] <= u'Z' ? key[i] + 32 : key[i]));
    }
    auto it = e->lex.latin_words.find(k);
    if (it == e->lex.latin_words.end() || int(it->second.surf.size()) > cap) return 0;
    for (size_t i = 0; i < it->second.surf.size(); i++) out[i] = uint16_t(it->second.surf[i]);
    return int(it->second.surf.size());
}
KKC_API double kkc_last_typo_margin(kkc_engine* e) { return e ? e->typo_margin : 0; }

KKC_API int kkc_last_suggest(kkc_engine* e, uint16_t* out, int cap, int32_t* kinds, int kcap) {
    if (!e) return 0;
    std::vector<uint16_t> buf;
    int n = 0;
    for (auto& x : e->suggest) {
        if (n >= kcap) break;
        std::vector<uint16_t> b;
        to16(x.second, b);
        buf.insert(buf.end(), b.begin(), b.end());
        buf.push_back(0);
        kinds[n++] = x.first;
    }
    if (int(buf.size()) > cap) return -1;
    std::copy(buf.begin(), buf.end(), out);
    return n;
}

KKC_API void kkc_last_times(kkc_engine* e, double* t5) { for (int i = 0; i < 5; i++) t5[i] = e ? e->times[i] : 0; }

KKC_API int kkc_last_scores(kkc_engine* e, float* u, int cap) {
    if (!e) return -2;
    int n = int(e->last_u.size());
    if (n > cap) return -1;
    std::copy(e->last_u.begin(), e->last_u.end(), u);
    return n;
}

KKC_API int kkc_last_paths(kkc_engine* e, int pmax, double* costs, int32_t* counts, int32_t* ends, int32_t* lens, int wcap,
                           uint16_t* text, int tcap) {
    if (!e) return 0;
    e->last_paths = tl_paths;
    int np = 0, nw = 0, nt = 0;
    for (const PathInfo& p : e->last_paths) {
        if (np >= pmax) break;
        if (nw + int(p.w.size()) > wcap) break;
        int need = 0;
        for (auto& w : p.w) for (char32_t c : w.second) need += c >= 0x10000 ? 2 : 1;
        if (nt + need > tcap) break;
        costs[np] = p.c;
        counts[np] = int(p.w.size());
        for (auto& w : p.w) {
            int l = 0;
            for (char32_t c : w.second) {
                if (c >= 0x10000) { char32_t v = c - 0x10000; text[nt++] = uint16_t(0xD800 + (v >> 10)); text[nt++] = uint16_t(0xDC00 + (v & 0x3FF)); l += 2; }
                else { text[nt++] = uint16_t(c); l++; }
            }
            ends[nw] = w.first;
            lens[nw] = l;
            nw++;
        }
        np++;
    }
    return np;
}

KKC_API int kkc_last_segments(kkc_engine* e, int32_t* ends, int32_t* lens, int cap) {
    if (!e) return -2;
    int n = int(e->last_segs.size());
    if (n > cap) return -1;
    for (int i = 0; i < n; i++) { ends[i] = e->last_segs[i].e; lens[i] = e->last_segs[i].l16; }
    return n;
}

KKC_API int kkc_last_segment_pos(kkc_engine* e, int32_t* lids, int32_t* rids, int cap) {
    if (!e) return -2;
    int n = int(e->last_segs.size());
    if (n > cap) return -1;
    for (int i = 0; i < n; i++) { lids[i] = e->last_segs[i].lid; rids[i] = e->last_segs[i].rid; }
    return n;
}

KKC_API void kkc_user_clear(kkc_engine* e) {
    if (e) { e->user.clear(); e->user_latin.clear(); }
}

KKC_API int kkc_user_add_like(kkc_engine* e, const uint16_t* r, int nr, const uint16_t* s, int ns, const uint16_t* tr, int ntr,
                              const uint16_t* ts, int nts, int bonus) {
    if (!e || nr <= 0 || ns <= 0) return 0;
    uint32_t a, b;
    if (!e->lex.find(tr, ntr, a, b)) return 0;
    const ustr want = from16(ts, size_t(nts));
    Ent best{};
    bool found = false;
    for (uint32_t j = a; j < b; j++) {
        const Ent en = e->lex.ent(j);
        if (from16(e->lex.s_blob + en.soff, en.slen) == want && (!found || en.cost < best.cost)) { best = en; found = true; }
    }
    if (!found) return 0;
    e->user.push_back({from16(r, size_t(nr)), from16(s, size_t(ns)), best.lid, best.rid, best.cost - bonus});
    {   // 表記が英字 (と数字・間の空白) だけの語は、ローマ字で英字のまま打ったときの英字の辺にも (ShuntiLettuce、2026-10-23)
        std::string lower;
        bool latin = true;
        for (int i = 0; i < ns && latin; i++) {
            const uint16_t c = s[i];
            if (c >= u'a' && c <= u'z') lower.push_back(char(c));
            else if (c >= u'A' && c <= u'Z') lower.push_back(char(c + 32));
            else if (c >= u'0' && c <= u'9') lower.push_back(char(c));
            else if (c != u' ') latin = false;
        }
        if (latin && lower.size() >= 2)
            e->user_latin[lower] = LatinWord{from16(s, size_t(ns)), best.lid, best.rid, best.cost - bonus};
    }
    return 1;
}

KKC_API int kkc_user_add_like2(kkc_engine* e, const uint16_t* r, int nr, const uint16_t* s, int ns, const uint16_t* tr, int ntr,
                               const uint16_t* ts, int nts, int bonus, int flags) {
    const int ok = kkc_user_add_like(e, r, nr, s, ns, tr, ntr, ts, nts, bonus);
    if (ok && (flags & 1)) e->user.back().whole = true;
    return ok;
}

#ifndef KKC_FRAG
#define KKC_FRAG 4500   // 「ね」へのつながりのコストがこれより高い語は、言い切れない形として予測に出さない
#endif

// 固有名詞 (Mozc の ID: 固有名詞一般 1920・人名 1921・名 1922・姓 1923・組織 1929)。読みの続く語 (予測) には出さない:
// 「いく」で 生田絵梨花・育、「おつ」で 尾辻舞・乙骨耐軒 が並んでいた (ユーザー 2026-10-17)。名前は読みを打ち切れば変換の候補に出る
static bool proper_noun_lid(int lid) { return (lid >= 1920 && lid <= 1923) || lid == 1929; }

KKC_API int kkc_complete(kkc_engine* e, const uint16_t* prefix, int np, int max_extra, int maxout, uint16_t* out, int cap) {
    if (!e || np <= 0) return 0;
    const Lex& L = e->lex;
    // 読みは辞書の中で並んでいるので、prefix で始まる読みの範囲の先頭を二分探索で探す
    const int lo = L.lower_bound(prefix, np);
    // 終助詞「ね」の左 ID (読み ね・表記 ね の語のうちコストのいちばん低いもの)
    int ne_lid = 0;
    {
        const uint16_t ne = 0x306D;
        uint32_t a, b;
        int32_t best = INT32_MAX;
        if (L.find(&ne, 1, a, b))
            for (uint32_t j = a; j < b; j++) {
                const Ent en = L.ent(j);
                if (en.slen == 1 && L.s_blob[en.soff] == ne && en.cost < best) best = en.cost, ne_lid = en.lid;
            }
    }
    struct C { int32_t cost; uint32_t ent; };
    std::vector<C> found;
    uint16_t rbuf[Lex::MAXR];
    for (int i = lo, scanned = 0; i < L.nread && scanned < 50000; i++, scanned++) {
        // 版 2 は前の読みから順に戻す (1 つ目だけ区切りの先頭から)
        int rn;
        const uint16_t* r = (L.ver == 1 || i == lo) ? L.reading(i, rn, rbuf) : (rn = L.next_reading(i, rbuf), rbuf);
        if (rn < np || memcmp(r, prefix, size_t(np) * 2)) break;          // prefix で始まる読みが尽きた
        if (rn == np || rn > np + max_extra) continue;                      // 打った読みそのもの・長すぎる読みは除く
        // 順位は語のコスト (よく使う語ほど低い)。ただし後ろに終助詞「ね」が付きにくい語 (よろしけれ・いただい など
        // 活用の途中の形) は、言い切れないので除く
        for (uint32_t j = L.e_first[i]; j < L.e_first[i + 1]; j++)
            if (L.ent(j).cost < ENG_COST && L.conn(L.ent(j).rid, ne_lid) <= KKC_FRAG && !proper_noun_lid(L.ent(j).lid))
                found.push_back({L.ent(j).cost, j});
    }
    std::sort(found.begin(), found.end(), [](const C& a, const C& b) { return a.cost < b.cost; });
    std::unordered_set<ustr> all;   // 候補の表記すべて (言いかけの形を見分けるため)
    for (const C& c : found) {
        const Ent en = L.ent(c.ent);
        all.insert(from16(L.s_blob + en.soff, en.slen));
    }
    std::vector<uint16_t> buf;
    std::unordered_set<ustr> seen;
    int n = 0;
    for (const C& c : found) {
        if (n >= maxout) break;
        const Ent en = L.ent(c.ent);
        ustr sf = from16(L.s_blob + en.soff, en.slen);
        // 言い切れない形を表記の終わりで除く (ありゃ・すみゃ・ありがたかっ・よろしけれ・よろしかろ)
        const char32_t last = sf.back();
        if (last == U'ゃ' || last == U'ゅ' || last == U'ょ' || last == U'っ') continue;
        if (sf.size() >= 2 && (sf.compare(sf.size() - 2, 2, U"けれ") == 0 || sf.compare(sf.size() - 2, 2, U"かろ") == 0)) continue;
        if (sf.size() >= 3 && sf.compare(sf.size() - 3, 3, U"しから") == 0) continue;   // よろしから・宜しから (未然形)
        // 歩こ・楽しも・住み着こ (後ろに う の付いた 歩こう が候補にあれば、言いかけの形は出さない)。
        // 感動詞・接続詞 (Mozc の ID 2591・2593: おはよ / おはよう) は別の語なので除かない
        if (en.lid != 2591 && en.lid != 2593 && all.count(sf + U"う")) continue;
        if (!seen.insert(sf).second) continue;
        to16(sf, buf);
        buf.push_back(0);
        n++;
    }
    if (int(buf.size()) > cap) return -1;
    std::copy(buf.begin(), buf.end(), out);
    return n;
}

#include "style.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "converter.h"

namespace shunti {

namespace {

uint32_t crc32(const std::string& s) {   // zlib.crc32 と同じ (判定器の学習は Python)
    static uint32_t tab[256] = {};
    if (!tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = tab[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put_utf8(std::string& o, char32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
}

std::u32string to32(const u16& s) {
    std::u32string o;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        o.push_back(c);
    }
    return o;
}

constexpr float A = 3.0f, T = 0.2f, LAM = 0.2f;

}  // namespace

void StyleModel::set_variants(const std::string& tsv_utf8) {
    var_.clear();
    std::unordered_map<u16, int> lem;
    size_t st = 0;
    while (st < tsv_utf8.size()) {
        size_t en = tsv_utf8.find('\n', st);
        if (en == std::string::npos) en = tsv_utf8.size();
        std::string line = tsv_utf8.substr(st, en - st);
        st = en + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> col(1);
        for (char ch : line) { if (ch == '\t') col.emplace_back(); else col.back() += ch; }
        if (col.size() < 4 || col[0].empty() || col[1].empty()) continue;
        const u16 kana = from_utf8(col[0]), kanji = from_utf8(col[1]);
        const int id = lem.emplace(from_utf8(col[2]), int(lem.size())).first->second;
        int cl = 0;
        for (char ch : col[3]) if (ch >= '0' && ch <= '9') cl = cl * 10 + (ch - '0');
        if (cl < 0 || cl > 7) cl = 0;
        var_.emplace(kana, Form{kanji, id, cl, false});
        var_.emplace(kanji, Form{kana, id, cl, true});
    }
}

// 形: "SREG" 版 (1) H (u32) 尺度 (f32) と H × 2 の int8 (行ごとに 丁寧・崩れ)
bool StyleModel::set_register(const std::string& bin) {
    w_.clear();
    H_ = 0;
    if (bin.size() < 16 || bin.compare(0, 4, "SREG") != 0) return false;
    uint32_t ver, h;
    float sc;
    std::memcpy(&ver, bin.data() + 4, 4);
    std::memcpy(&h, bin.data() + 8, 4);
    std::memcpy(&sc, bin.data() + 12, 4);
    if (ver != 1 || h == 0 || bin.size() != 16 + size_t(h) * 2) return false;
    w_.assign(bin.begin() + 16, bin.end());
    H_ = h;
    scale_ = sc;
    return true;
}

std::array<float, 2> StyleModel::register_of(const u16& ctx) const {
    if (!H_ || ctx.empty()) return {.5f, .5f};   // 文脈が無いときは分からない (判定器は教材の割合で崩れ寄りになる)
    std::u32string s = to32(ctx);
    if (s.size() > 80) s = s.substr(s.size() - 80);
    std::vector<uint32_t> g;
    g.push_back(crc32("L" + std::to_string(std::min<size_t>(s.size() / 10, 8))) % H_);
    for (size_t n = 1; n <= 3; n++)
        for (size_t i = 0; i + n <= s.size(); i++) {
            std::string u;
            for (size_t k = 0; k < n; k++) put_utf8(u, s[i + k]);
            g.push_back(crc32(u) % H_);
        }
    float z[2] = {0, 0};
    for (uint32_t x : g) { z[0] += w_[size_t(x) * 2]; z[1] += w_[size_t(x) * 2 + 1]; }
    const float d = (z[1] - z[0]) * scale_ / float(g.size());
    const float p1 = 1.0f / (1.0f + std::exp(-d));
    return {1 - p1, p1};
}

std::array<float, 2> StyleModel::weight_of(const u16& ctx) const {
    auto r = register_of(ctx);
    float a = r[0] * r[0], b = r[1] * r[1];
    const float n = std::sqrt(a * a + b * b);
    return {a / n, b / n};
}

void StyleModel::set_counts(const std::unordered_map<u16, Count>& counts) {
    lem_.clear();
    cls_ = {};
    for (auto& [s, c] : counts) {
        auto v = var_.find(s);
        if (v == var_.end()) continue;
        const int k = v->second.kanji ? 0 : 1;
        for (auto* dst : {&lem_[v->second.lemma][k], &cls_[size_t(v->second.cls)][k]}) {
            dst->w[0] += c.w[0];
            dst->w[1] += c.w[1];
            dst->n += c.n;
        }
    }
}

int StyleModel::pref(const u16& surface, const std::array<float, 2>& r) const {
    auto v = var_.find(surface);
    if (v == var_.end()) return 0;
    auto eff = [&](const Count& c) { return c.w[0] * r[0] + c.w[1] * r[1] + LAM * float(c.n); };
    float a1 = 0, b1 = 0;
    auto l = lem_.find(v->second.lemma);
    if (l != lem_.end()) { a1 = eff(l->second[0]); b1 = eff(l->second[1]); }
    const auto& ck = cls_[size_t(v->second.cls)];
    const float ak = eff(ck[0]), bk = eff(ck[1]);
    const float q = (ak + 1) / (ak + bk + 2);
    const float pk = (a1 + A * q) / (a1 + b1 + A);
    return pk >= .5f + T ? 1 : pk <= .5f - T ? -1 : 0;
}

bool StyleModel::restyle(const u16& ctx, Conversion& out) const {
    if (var_.empty() || lem_.empty() || out.cands.empty() || out.words.empty()) return false;
    const auto r = weight_of(ctx);
    u16 text;
    std::vector<Word> words;
    bool changed = false;
    // 網の語の区切りは書き方の対の表と合わないことがある (でき|ない と できない): 隣の語を 3 つまでつないで長い方から引く
    for (size_t i = 0; i < out.words.size();) {
        size_t len = 1;
        u16 s = out.words[i].surface;
        auto v = var_.end();
        for (size_t L = std::min<size_t>(3, out.words.size() - i); L >= 1; L--) {
            u16 t;
            for (size_t k = 0; k < L; k++) t += out.words[i + k].surface;
            if ((v = var_.find(t)) != var_.end()) { len = L; s = t; break; }
        }
        Word w = out.words[i];
        for (size_t k = 1; k < len; k++) { w.reading += out.words[i + k].reading; w.rid = out.words[i + k].rid; }
        w.surface = s;
        if (v != var_.end()) {
            const int p = pref(s, r);
            if (p != 0 && (p > 0) != v->second.kanji) {
                w.surface = v->second.other;
                changed = true;
            }
        }
        text += w.surface;
        words.push_back(std::move(w));
        i += len;
    }
    if (!changed) return false;
    auto& c = out.cands;
    auto dup = std::find(c.begin(), c.end(), text);
    if (dup != c.end()) c.erase(dup);
    c.insert(c.begin(), text);
    out.words = std::move(words);
    return true;
}

}  // namespace shunti

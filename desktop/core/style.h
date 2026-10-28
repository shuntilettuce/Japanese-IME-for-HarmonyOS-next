// 書き方の癖 (2026-10-28)。モデルには読ませず、確定した書き方を数えて 1 位の書き方 (できない ↔ 出来ない) だけを入れ替える。
// - 書き方の対の表 (kkc_variants.tsv): かなの形・漢字の形・見出し・品詞の種類
// - 文の種類の判定器 (kkc_register.bin): 左の文脈から 丁寧 / 崩れ の確率 (字の 1〜3 字組の hash を引いて平均)
// - 覚え方: 見出しごとの回数 + 3 × 品詞の種類の漢字率 (経験ベイズ)。回数は文の種類で重みづけ
//   (覚えるとき A += r̂、使うとき a = A·r̂0 + 0.2 × 全体の回数。r̂ = r² を長さ 1 に)。見込みが 0.7 以上なら漢字・0.3 以下ならかな
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "text.h"

namespace shunti {

struct Word;
struct Conversion;

class StyleModel {
public:
    struct Count { float w[2] = {0, 0}; int n = 0; };   // 書き方ごとの記録: 文の種類で重みづけした回数・回数

    void set_variants(const std::string& tsv_utf8);
    bool set_register(const std::string& bin);   // 判定器の重み。壊れていれば false (文の種類を見ずに数える)
    bool ready() const { return !var_.empty(); }
    bool is_variant(const u16& s) const { return var_.count(s) > 0; }
    // 丁寧・崩れの確率 (判定器が無ければ半々)
    std::array<float, 2> register_of(const u16& ctx) const;
    // 覚えるときの重み r̂
    std::array<float, 2> weight_of(const u16& ctx) const;
    void set_counts(const std::unordered_map<u16, Count>& counts);
    // 1 位 (out.cands[0]、語の区切り out.words) の書き方を、覚えた書き方に入れ替えた候補を先頭に足す。足したら true
    bool restyle(const u16& ctx, Conversion& out) const;
    // その語を今の文脈で書くなら: 1 漢字・-1 かな・0 分からない
    int pref(const u16& surface, const std::array<float, 2>& r) const;

private:
    struct Form { u16 other; int lemma = 0, cls = 0; bool kanji = false; };
    std::unordered_map<u16, Form> var_;
    std::vector<int8_t> w_;   // 判定器 (H 行 × 2)
    float scale_ = 0;
    uint32_t H_ = 0;
    // 見出し・品詞の種類ごとの [漢字/かな] の重みつき回数と回数
    std::unordered_map<int, std::array<Count, 2>> lem_;
    std::array<std::array<Count, 2>, 8> cls_{};
};

}  // namespace shunti

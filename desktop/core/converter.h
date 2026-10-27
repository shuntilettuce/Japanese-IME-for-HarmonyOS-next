// 変換エンジン (engine.cpp) の窓口。差別語の候補を弾き、長すぎる読みは分けて変換し、1 位の候補を文節に分ける。
#pragma once
#include <cstddef>
#include <vector>

#include "store.h"
#include "text.h"

struct kkc_engine;

namespace shunti {

// 1 位の候補の語 (読み・表記・品詞の左右 ID)
struct Word { u16 reading, surface; int lid = 0, rid = 0; bool pass = false; };
// 文節 (自立語 + 付属語)。head_r・head_s = 自立語 (と接頭) の読みと表記の長さ。残りが付属語
struct Phrase { u16 reading, surface; size_t head_r = 0, head_s = 0; };

// 候補の木 (候補の窓の上の帯に出す)。上位の経路の点数を確率にして、1 位の語ごとの強さ・隣との結びつき・
// 同じ読みの区間の別の表記・区切り方で迷う所 (別の区切りの道) を出す
struct TreeWord {
    u16 surface;
    int rs = 0, re = 0;   // 読みの区間
    float s = 1;          // この語の強さ (この語を通る経路の確率の和)
    float link = 1;       // 前の語との結びつき (前の語を通る経路のうち、この語も通るものの割合)
    std::vector<std::pair<u16, float>> alts;   // 同じ読みの区間の別の表記 (強い順)
};
struct TreeNode {
    std::vector<TreeWord> main;   // 1 位の道
    std::vector<TreeWord> fork;   // 区切り方で迷う所: 別の区切りの道 (空なら迷っていない)
    float p = 1;                  // 1 位の区切りの確率 (fork があるとき)
};
// paths = 上位の経路 (コスト, 語)。cuts = 1 位の文節の終わりの位置 (読み)。語はこの位置で文節にまとめる
std::vector<TreeNode> build_tree(const std::vector<std::pair<double, std::vector<TreeWord>>>& paths, const std::vector<int>& cuts);

struct Conversion {
    std::vector<TreeNode> tree;  // 候補の木 (分からなければ空)
    std::vector<u16> cands;      // 候補 (よい順)
    std::vector<Word> words;     // cands[0] の語の区切り (分からなければ空)
    // 提案 (set_suggest がオンのとき): 打ち間違いの直し・日付・住所・英単語・絵文字・顔文字・かっこ など、候補に混ぜないもの
    std::vector<u16> sugg;
    std::vector<int> sugg_kinds;   // engine.h の KKC_SUGGEST_*
};

class Converter {
public:
    Converter() = default;
    ~Converter();
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    // 辞書とモデル (呼び出し側がメモリに写したもの。閉じるまで持っておく)
    bool open(const void* lex, size_t lex_size, const void* model, size_t model_size, int threads);
    bool ok() const { return e_ != nullptr; }

    // ctx = 左の文脈 (確定済みの文)、kana = 読み
    Conversion convert(const u16& ctx, const u16& kana, int max);
    // 後ろの文脈 (カーソルの後ろ・後で打った文)。直後の 1 回の convert にだけ効く。has_right_ctx = モデルが使えるか
    void set_right_ctx(const u16& text);
    // 打った鍵の列と、読みの i 字目が鍵の何打目までで決まったか (英字の語・ローマ字の打ち間違いの直しに使う)。直後の 1 回の convert にだけ効く
    void set_keys(const u16& keys, const std::vector<int32_t>& span_end, size_t reading_len);
    bool has_right_ctx() const;
    // 提案を候補に混ぜず Conversion::sugg に分けて出すか (設定「提案を分けて表示する」)
    void set_suggest(bool on);
    void set_typo(bool on);    // ローマ字の打ち間違いの直し (既定オン)
    void set_latin(bool on);   // 英字の打ち分け (既定オン)
    // M5 の個人化 (版 6 のモデルのときだけ効く)。書き方の対の表 (TSV)・表記ごとの使う人の印・書き方の記録 (表記, 回数)
    bool personal() const;
    void set_variants(const std::string& tsv_utf8);
    void set_marks(const std::vector<std::pair<u16, int>>& marks);
    void set_style(const std::vector<std::pair<u16, int>>& counts);
    bool is_variant(const u16& surface) const;
    // 確定した文 (読み, 表記) の中の、書き方の対の表にある語 (書き方の記録に数える)
    std::vector<u16> path_variants(const u16& reading, const u16& text) const;
    u16 latin_word(const u16& key) const;   // 綴り (小文字) の辞書の英字の語の表記 (無ければ空)
    // 予測: 読みが prefix で始まる辞書の語
    std::vector<u16> complete(const u16& prefix, int max, int max_extra = 6);
    // ユーザー辞書の語を網に入れ直す (前の分は消す)
    int set_user_words(const std::vector<UserDict::Form>& forms, int bonus = 800);

private:
    // 返り値 false = 読みが長すぎる
    bool convert_once(const u16& ctx, const u16& kana, int max, Conversion& out);

    kkc_engine* e_ = nullptr;
    std::vector<UserDict::Form> user_;   // 最後に入れたユーザー辞書の語 (モデルを替えて開き直したときに入れ直す)
    int user_bonus_ = 800;
    bool suggest_ = true;
    bool latin_ = true;
    bool typo_ = true;
    std::string variants_;   // モデルを替えて開き直したときに入れ直す
};

// 語をつないで差別語になった候補か (HarmonyOS 版 AiConverter.SLURS と同じ)
bool is_slur(const u16& s);

// 1 位の候補の語を文節にまとめる (付属語は前の語に、接頭の後ろの語はその接頭に、続く英数字はひとまとまりに)
std::vector<Phrase> group_phrases(const std::vector<Word>& words);

}  // namespace shunti

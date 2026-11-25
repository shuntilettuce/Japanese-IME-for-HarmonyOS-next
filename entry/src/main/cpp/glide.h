// グライド入力 (QWERTY のローマ字をなぞる) のデコーダ。なぞった軌跡から読み (ひらがな) の候補をコスト付きで返す。
// 読みの辞書の木を、ローマ字の綴り (同じキーが続くのは 1 つ = っ・nn は見えない) でたどり、
// 軌跡の点とキーの割り当てを DP で決める。語は 3 つまでつなげる (語 + 助詞)
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace glide {

enum class Mode { Kana, Letters };   // Kana: 読みはひらがな、綴りの表でキーに直す。Letters: 読みが英字そのまま (英語・ピンイン)

struct Key { char c; float x, y; };   // キーの文字と中心 (キーの幅を 1 にした座標)

struct Result { std::u16string reading; float cost; float path; };

struct Params {
    float spacing = 0.25f;    // 軌跡を取り直す点の間隔 (キーの幅)
    int max_pts = 128;
    float sigma_key = 0.45f;  // キーの中心からのずれの尺度 (キーの幅)
    float sigma_seg = 0.45f;  // キーとキーの間の線からのずれの尺度
    float lex_w = 1.0f / 1000; // 辞書のコスト (Mozc の単位) をなぞりのコストに直す係数
    float word_pen = 8.0f;    // 語を 1 つ増やすごとの罰
    float beam = 20.0f;       // 今の 1 位からこれ以上悪い枝は切る
    float abs_bound = 60.0f;  // これ以上悪い枝は 1 位が無くても切る
    int max_words = 3;
    float same_key_pen = 0.5f;  // 同じキーが続く字 (点を使わない字) ごとの罰 (1.5 だと ふつう の う が重く ふつの・ふゆの に負けた。擬似ベンチ 0.5 が最良)
    float dir_w = 4.0f;         // 線の上の点の向きの違いの重み
    int beam_size = 3000;     // 点ごとに残すトークンの数
    int nbest = 10;
    uint64_t max_expand = 2000000;   // 展開の上限 (時間の保険)
};

class Decoder {
public:
    // words: 読み (ひらがな・ー) と辞書のコスト。同じ読みが複数あれば小さい方を使う
    void build(const std::vector<std::pair<std::u16string, int32_t>>& words, Mode mode = Mode::Kana);
    void set_keys(const std::vector<Key>& keys);
    // pts: 軌跡 (x, y の並び、キーの座標と同じ単位)
    std::vector<Result> decode(const std::vector<float>& pts, const Params& p) const;
    // ネットワーク (glide_nn) の log p [frames × (1 + キーの数)] を、辞書の木に沿った CTC のビームサーチで語にする (Letters の形だけ)。
    // キーの番号は set_keys で渡した順。score = CTC の log p + lm_w × log p(語) + len_w × 字数、cost = −score
    std::vector<Result> decode_ctc(const std::vector<float>& lp, int frames, int nkeys, float lm_w, float len_w, int beam, int nbest) const;
    size_t nodes() const { return node_.size(); }
    uint64_t last_expanded = 0;

private:
    // 16 バイト (スマホで 137 万個 = 22MB)。コストは辞書のコスト (0〜32766)、NOC = その読みの語が無い
    static constexpr int16_t NOC = INT16_MAX;
    struct Node { char16_t ch; int16_t cost = NOC, minsub = NOC; int32_t first = -1, next = -1; };
    std::vector<Node> node_;
    std::vector<Key> keys_;
    int kidx_[128];
    Mode mode_ = Mode::Kana;
    static constexpr int NSLOT = 320;
    std::vector<std::vector<std::vector<int8_t>>> sp1_;
    std::vector<uint8_t> has2_;
    std::vector<uint8_t> vowel_;   // キー -> 母音・n・ー
    std::unordered_map<uint32_t, std::vector<std::vector<int8_t>>> sp2_;
    std::unordered_map<int32_t, std::vector<int32_t>> two_;   // 節 -> 2 字の綴りの後ろの字の子
    friend struct Search;
    friend struct Beam;
};

}  // namespace glide

namespace glide {
// 試験用: 読みをローマ字の綴り (まとめる前) にする。variant: 0 = 最初の綴り、それ以外は rng で選ぶ
std::string romanize(const std::u16string& rd, unsigned (*rng)());
}

// グライドのキーの確率を出す小さいネットワーク (FUTO Swipe の作りに倣って自前で学習、shuntllim/glide/nn)。
// 軌跡 64 点とキーの中心 (キーボードの字の範囲を 0〜1) から、32 フレームそれぞれの log p (blank と各キー) を出す。
// キーの位置は実行時に渡すので、配列が変わっても学習し直さなくてよい
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace glide {

class Net {
public:
    // glide_nn.bin の中身 ("GNN1" + d + float32 の重み)。読めなければ false
    bool load(const void* data, size_t n);
    bool ok() const { return d_ > 0; }
    // pts: 軌跡 (x, y の並び、0〜1)。64 点に取り直してから流す。keys: キーの中心 (x, y の並び、0〜1)
    // 返り値: [32 × (1 + K)] の log p (0 番が blank)
    std::vector<float> run(const std::vector<float>& pts, const std::vector<float>& keys) const;
    static constexpr int T = 64;
    static constexpr int TF = 32;

private:
    int d_ = 0;
    std::vector<float> w_;
    struct Blk { const float *dw, *dwb, *lnw, *lnb, *p1, *p1b, *p2, *p2b, *s1, *s1b, *s2, *s2b; int dil; };
    const float *stem_ = nullptr, *stemb_ = nullptr, *down_ = nullptr, *downb_ = nullptr, *lnw_ = nullptr, *lnb_ = nullptr, *head_ = nullptr, *headb_ = nullptr;
    Blk b_[5];
};

}  // namespace glide

// ローマ字 → かな (HarmonyOS 版 ime/JapaneseConverter.ets・Android 版 JapaneseConverter.kt の移植。決まりは向こうと同じ)。
// 表は HarmonyOS 版と同じもの (tables.h) に、PC のキーボードでよく打つ綴り (ca・qa・kwa・tsa・wha・n' など) と
// z で始まる記号 (zh → ← など) を足したもの。
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "text.h"

namespace shunti {

class Romaji {
public:
    Romaji();

    struct Result {
        u16 committed;   // かなに決まった分
        u16 pending;     // まだ決まらないローマ字
    };

    // 1 キー (小文字の a-z・-・' など) を足す
    Result process(char16_t key);
    // 溜めているローマ字 (まだかなに決まっていない分)
    const u16& pending() const { return buffer_; }
    // 溜めている分を出して空にする (n は ん、それ以外は打ったまま)
    u16 flush();
    void backspace() { if (!buffer_.empty()) buffer_.pop_back(); }
    void reset() { buffer_.clear(); }
    // key が溜めている分に続けてローマ字として意味を持つか (z の後の記号など)
    bool accepts(char16_t key) const;
    // s (小文字のローマ字) が表のどれかの綴りの頭になっているか
    bool is_prefix(const u16& s) const { return prefixes_.count(s) > 0; }
    // 溜めている分の前に字を戻す (打ち間違いを消して子音だけが残ったとき、それを続きのローマ字として使う)
    void unread(const u16& s) { buffer_ = s + buffer_; }

    // かな → ローマ字 (F9・F10 で英字に戻すとき、打ったキーが分からない場合に使う)
    u16 to_romaji(const u16& kana) const;
    // 表に綴りを足す・上書きする (詳細設定のローマ字の表)
    void add_pair(const u16& key, const u16& kana);
    // 拡張ローマ字 (AZIK など、子音 + 子音の綴りを足した表)。オンのとき、子音を 2 つ重ねた「っ」(kk) と、n の後の子音で
    // 「ん」にする決まり (nk → んk) より、表の綴り (kk = きん・nk = にん) とその続きを先に見る
    void set_extended(bool on) { extended_ = on; }
    bool extended() const { return extended_; }
    // 元の表を空にする (gACT10 のように、ふつうのローマ字と全く違う綴りの表だけで打つとき)。
    // 子音を重ねた「っ」・n の決まりも使わない。かな → ローマ字 (F9・F10) は元の表のまま
    void clear_table() { table_.clear(); prefixes_.clear(); longer_.clear(); max_kana_ = 1; bare_ = true; }

private:
    Result process_buffered();

    std::unordered_map<u16, u16> table_;
    std::unordered_set<u16> prefixes_;
    std::unordered_set<u16> longer_;   // 足した綴りの、自分より長い綴りの頭になっているもの (拡張ローマ字で続きを待つ)
    std::unordered_map<u16, u16> reverse_;   // かな → いちばん短い綴り
    size_t max_kana_ = 1;
    u16 buffer_;
    bool extended_ = false;
    bool bare_ = false;   // clear_table の後
};

}  // namespace shunti

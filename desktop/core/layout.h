// キー配列 (PC の物理キーの並びを、別の配列として読む)。大西配列・Dvorak (英字の入れ替え)、JIS かな (かなを直接)、
// NICOLA・薙刀式 (同時押し) を、同じ形の表 (layouts/*.tsv) で書く。同じ系統の配列は表を足すだけで増やせる (ユーザー 2026-10-16)。
//
// 表の形 (1 行 1 つ、# で始まる行は説明。キーは US 配列の物理位置の名前: q w e … a s d … ; ' , . / - = [ ] \ ` と、
// JIS の余分なキー ro (\ _ のキー) yen (¥ | のキー)、space、lthumb (無変換)、rthumb (変換)):
//   #! name 配列の名前 / #! kind letters|kana|chord / #! author 作者 / #! url 公式ページ
//   a<TAB>出る字              単打
//   S-a<TAB>出る字            Shift と
//   a+j<TAB>出る字            2 キーの同時押し (順番は問わない)
//   a+j+k<TAB>出る字          3 キーの同時押し
//   space+a / lthumb+a / rthumb+a<TAB>出る字   シフト (押している間ずっと効く)
//   d<TAB>{p1}  と  p1>e<TAB>出る字           前置シフト (月配列など: d を打ってから e で「ね」)
// 出る字: letters は英字・記号 (そのままローマ字に流す)、kana と chord はかな・記号 (そのまま入れる)。
// #! kind romaji は ローマ字の表に綴りを足す・上書きする (綴り<TAB>かな。AZIK など。2026-11-15)。
//   キーはふつう読み替えないが、「#! keys」の行の後は letters と同じ形のキーの読み替え (Dvorak の上の ACT・DvorakJP、SKY)。
//   読み替えた字はそのままローマ字に流すので、1 キーで複数の字 (SKY の ai) も書ける。
//   「#! base empty」は元のローマ字の表を使わず、この表の綴りだけで打つ (gACT10)
// {bs} {enter} {left} {right} は打鍵として返す (薙刀式の編集のキーなど)
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace shunti {

class Layout {
public:
    enum class Kind { Letters, Kana, Chord, Romaji };
    // 判定の結果: 出す字 (text) か、打鍵 (key: "bs" など)
    struct Out {
        std::u16string text;
        std::string key;
        std::string unit;   // text を出した打鍵の単位 (表の書き方: a・S-a・a+j・lthumb+a・p1>e)。打ち間違いの直しに使う
    };

    bool load(const std::string& tsv, std::string* err = nullptr);
    bool loaded() const { return !single_.empty() || !chords_.empty() || !romaji_.empty(); }
    // kind romaji: ローマ字の表に足す綴り (綴り, かな)。キーは読み替えない (handles は いつも false)
    const std::vector<std::pair<std::u16string, std::u16string>>& romaji_pairs() const { return romaji_; }
    // 「#! base empty」: 元のローマ字の表を空にして、この表の綴りだけで打つ (gACT10)
    bool romaji_base_empty() const { return romaji_base_empty_; }
    Kind kind() const { return kind_; }
    const std::string& name() const { return name_; }
    const std::string& author() const { return author_; }
    const std::string& url() const { return url_; }
    // この配列が受け持つキーか (受け持たないキーは今までどおり扱う)
    bool handles(const std::string& key) const;

    // 押した・離した (t はミリ秒)。決まった分を返す。同時押しの配列は、組になるか分かるまで待つ (空を返す)
    std::vector<Out> down(const std::string& key, bool shift, uint64_t t);
    std::vector<Out> up(const std::string& key, uint64_t t);
    // 待っているキーがあれば、時間切れ (now - 押した時刻 >= window) で単打として出す
    std::vector<Out> tick(uint64_t now);
    bool waiting() const { return !pending_.empty() || !evs_.empty(); }
    void reset();

    uint64_t window_ms = 100;  // 1 つだけ押していて、ほかと重ならないまま この時間がたてば単打に決める
    // 同時押し (chord の配列) は、押していた時間の重なりで決める: 2 つのキーの重なりが、短い方 (まだ押している相手なら
    // 先に離した方) の押していた時間のこの割合以上なら組。速く打って前後のキーが少し重なっただけ (ロールオーバー) は単打。
    // 親指・空白を押したまま字を丸ごと打てば、続けてシフト (連続シフト) になる
    double overlap_ratio = 0.5;
    // 流儀の分かれる所 (配列ソフトの設定と同じ。ユーザー 2026-10-16)
    int thumbs = 0;            // 親指キー: 0 = 無変換・変換、1 = 無変換・空白 (空白が右親指)、2 = 空白・変換 (空白が左親指)
    bool continuous = true;    // 連続シフト: 親指・空白を押したまま続けて打てば、続けてシフト (切ると 1 字ずつ押し直す)
    bool thumb_tap = true;     // 親指キーを単独で押したら、本来のキー (空白・変換・無変換) として働く
    bool repeat = true;        // 押しっぱなしで繰り返す
    // 物理キーの名前を、親指キーの割り当てに合わせて読み替える (空なら この配列では受け持たない)
    std::string map_key(const std::string& k) const;

    // ---- 配列の打ち間違いの直し (2026-11-15)。打鍵の単位 (Out::unit) の並びを 1 か所だけ直した並びを作る:
    // 消す・隣の物理キー (同じ面)・同じキーの別の面 (Shift・親指・前置シフトの付け外し)・入れ替え・同時押しの組み直し
    // (組みそこね・組みすぎ)・抜けた 1 打 (英字の配列だけ: 母音と抜けやすい子音を出すキー、前の打鍵の重ね)。
    // 返すのは単位ごとの出す字の並び (呼ぶ側が同じ配列・同じローマ字の表で読みに直す)
    std::u16string unit_text(const std::string& unit) const;
    bool has_key_map() const { return !single_.empty() || !shifted_.empty(); }
    std::vector<std::vector<std::u16string>> typo_variants(const std::vector<std::string>& units) const;
    std::vector<std::string> unit_alts(const std::string& unit) const;   // 1 つの単位の言い換え (隣のキー・別の面)
    std::vector<std::string> all_units() const;   // 表にある打鍵の単位の全部 (ベンチで読みを打鍵に直すのに使う)

private:
    struct Pending { std::string key; bool shift; uint64_t t; };
    // 同時押しの配列で、まだ決まっていない打鍵 (押した順)
    struct Ev {
        std::string key;
        bool shift = false;       // Shift と一緒
        bool is_shift = false;    // 押している間効くシフト (space・lthumb・rthumb)
        uint64_t down = 0, up = 0;
        bool released = false, decided = false, used = false;
        int shift_id = -1;        // シフトなら、shifts_ の記録の番号
        std::vector<Out> out;
    };
    std::deque<Ev> evs_;
    // シフト (space・親指) を押した記録。組んだあとも押している間は残す (連続シフト: 押したまま次の字も)
    struct ShiftRec { int id; std::string key; uint64_t down = 0, up = 0; bool released = false, used = false; };
    std::vector<ShiftRec> shifts_;
    int next_shift_id_ = 0;
    std::vector<Out> chord_down(const std::string& key, bool shift, uint64_t t);
    std::vector<Out> chord_up(const std::string& key, uint64_t t);
    std::vector<Out> resolve(uint64_t now);
    Out lookup1(const std::string& key, bool shift) const;
    std::vector<Out> flush_pending();
    std::vector<Out> emit(const std::u16string& s, const std::string& unit = std::string());   // {p1} は出さずに前置シフトにする
    std::string shift_held() const;   // 押している間効くシフト (space・lthumb・rthumb) のうち押されているもの

    Kind kind_ = Kind::Letters;
    std::string name_, author_, url_;
    std::map<std::string, std::u16string> single_, shifted_;
    std::map<std::string, std::u16string> chords_;   // キーは キーを名前順に + でつないだもの
    std::set<std::string> chord_keys_;               // 同時押しに出てくるキー (それ以外は待たずに出す)
    std::set<std::string> shift_keys_;               // 押している間効くシフト (表に出てくるもの)
    std::vector<Pending> pending_;
    std::set<std::string> held_;                      // いま押されているキー
    bool shift_used_ = false;                         // シフトを押している間に、ほかのキーと組んだ (離したときに単打で出さない)
    std::vector<std::pair<std::u16string, std::u16string>> romaji_;   // kind romaji の綴り
    bool romaji_base_empty_ = false;
    std::map<std::string, std::u16string> prefixed_;   // 前置シフトの面: キーは "p1>e"
    std::string prefix_;                              // 打った前置 ("p1")。空 = なし
};

// US 配列の物理位置の名前 (Windows のスキャンコードから)。分からないキーは空
std::string key_name_from_scancode(unsigned sc, bool extended);

}  // namespace shunti

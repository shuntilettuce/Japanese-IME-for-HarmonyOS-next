// 学習とユーザー辞書 (Android 版 Learning.kt・UserDict.kt と同じ決まり・同じ形の JSON ファイル)。
// PC では IME がアプリごとのプロセスに読み込まれるので、どちらも使う前にファイルが変わっていないかを見て、
// 変わっていれば読み直す (別のアプリで学習・登録した分を取り込む)。保存は一時ファイルに書いてから置き換える。
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "style.h"
#include "text.h"

namespace shunti {

// 最近の学習の控え (recent_learn.tsv)。時間を指定して学習を消すために、学習のたびに 1 行足す。
// 時刻 (秒)・種類 (L 学習 / H 候補を消した / S 書き方 / F 文の流れ / C 確定した文)・元に戻すのに要る値。256MB を超えたら古い半分を捨てる
void recent_add(const std::filesystem::path& dir, const std::vector<std::string>& fields);
// 種類 kind の行だけ控えから消す
void recent_drop(const std::filesystem::path& dir, const std::string& kind);
constexpr const wchar_t* RECENT_FILE = L"recent_learn.tsv";

class Learning {
public:
    explicit Learning(std::filesystem::path file) : file_(std::move(file)) {}

    // record の前の値 (確定の取り消し・時間を指定した消去で元に戻す)
    struct Undo { u16 reading, surface; int count = 0, day = 0; bool hidden = false; };   // hidden: 消した操作を戻す
    // 手で選び直した表記を覚えて、すぐ保存する。2 回分に数える (実機の入力ログの時刻順の評価は MELTYPE_NOTES。ユーザー 2026-10-30)
    Undo record(const u16& reading, const u16& surface, int add = 2);
    // 選び直さずに確定した表記。すでに学習のある読みでだけ 1 回分足す (選び直した表記が 1 位を奪い続けないように)
    Undo confirm(const u16& reading, const u16& surface);
    // 確定した文 (読み・表記・左の文脈) を控えに足す。「学習を始める」で同意したときだけ呼ぶ
    void note_commit(const u16& reading, const u16& surface, const u16& ctx);
    // 控えの順に元の値へ戻す。消した (回数 -1 の) 表記は、hidden の控えでだけ戻す
    void restore(const std::vector<Undo>& us);
    // その読みでその表記を選んだ記録を消す
    void forget(const u16& reading, const u16& surface);
    // その読みではその表記を候補に出さない (Ctrl+Delete。MS-IME と同じ。回数 -1 として覚える。選び直すと戻る)
    void hide(const u16& reading, const u16& surface, bool keep_recent = true);
    bool hidden(const u16& reading, const u16& surface);
    // 選んだことのある表記を回数の多い順に前へ (他の並びはそのまま)
    std::vector<u16> apply_order(const u16& reading, const std::vector<u16>& cands);
    // 予測: 読みが prefix で始まる、より長い読みで選んだ表記 (選んだ回数の多い順)
    std::vector<u16> completions(const u16& prefix, size_t max);
    // その読みで選んだ (表記, 回数)。回数 -1 は候補に出さない表記
    std::vector<std::pair<u16, int>> counts(const u16& reading);
    // その読みでいちばん多く選んだ表記 (無ければ空)
    u16 top(const u16& reading);
    // min 回以上選んだ (読み, 表記, 回数)。変換の網に候補として入れる (AI が文脈で選ぶ。ブログの報告 2026-10-21)
    struct Strong { u16 reading, surface; int count; };
    std::vector<Strong> strong(int min);
    uint64_t version() { refresh(); return version_; }   // 覚えた中身が変わるたびに増える
    void clear();

    // M5 の「使う人の印」: 表記ごとの (回数の段階 1〜6 × 時期 0〜3) を 1 + (c-1)*4 + r にしたもの (学習の網 kkcnative2 と同じ)。
    // 表記は文節の末尾の同じかな (を・が) を外した自立語 (なにを → 何を は 何)。回数が 0 以下 (候補に出さない) の表記は入れない
    std::vector<std::pair<u16, int>> marks();
    // その読みを最後に選んだ日 (1970-01-01 からの日数。分からなければ 0)。M5 の「使う人の印」の時期に使う
    int last_day(const u16& reading);
    static int today();

    // 読みは 2 万まで (以前は 500 で、普段使いだとすぐ押し出されて学習が消えていた。2026-10-24 ユーザーの指摘)。
    // あふれたら、回数が少なく長く使っていない読みから捨てる
    static constexpr size_t MAX_READINGS = 20000;
    static constexpr size_t MAX_SURFACES = 5;
    static constexpr int TOP_MIN_COUNT = 2;      // この回数から先頭へ (手で 1 度選べば届く)

private:
    struct Entry { u16 reading; std::vector<std::pair<u16, int>> counts; int day = 0; };
    void refresh();
    void save();
    void reindex();
    Entry* find(const u16& reading);
    std::unordered_map<u16, size_t> idx_;   // 読み -> entries_ の位置

    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    bool loaded_ = false;
    std::vector<Entry> entries_;
    uint64_t version_ = 0;
};

// 書き方の記録 (style.json)。かなでも漢字でも書ける語 (書き方の対の表にある表記) を、確定したときの文の種類 (丁寧・崩れの重み) と一緒に数える。
// 書き方の癖 (style.h) が使う。2026-10-28 からアプリごとには分けない (古い形はアプリの合計を読み込む)
class StyleLog {
public:
    explicit StyleLog(std::filesystem::path file) : file_(std::move(file)) {}
    void record(const u16& surface, const std::array<float, 2>& w);
    void unrecord(const std::vector<std::pair<u16, std::array<float, 2>>>& items);   // record を取り消す
    const std::unordered_map<u16, StyleModel::Count>& counts() { refresh(); return m_; }
    uint64_t version() { refresh(); return version_; }
    void clear();
    static constexpr size_t MAX_SURFACES = 20000;

private:
    void refresh();
    void save();
    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    bool loaded_ = false;
    std::unordered_map<u16, StyleModel::Count> m_;
    uint64_t version_ = 0;
};

// 英字の語の綴り (英字・数字を小文字に、間の空白は除く)。英字・数字・空白以外を含めば空
u16 latin_key(const u16& w);

class UserDict {
public:
    struct Entry { u16 reading, word; std::string pos = "noun", group; };
    // 変換の網に入れる形: 読み・表記と、品詞を写す代表語の同じ形
    // whole = 読み全体が一致したときだけ (短縮よみ)。bonus = 代表語より下げるコスト (-1 は Converter の既定)
    struct Form { u16 reading, surface, tmpl_reading, tmpl_surface; bool whole = false; int bonus = -1; };

    explicit UserDict(std::filesystem::path file) : file_(std::move(file)) {}

    // ファイルが変わっていれば読み直す。変わったら true
    bool refresh();
    const std::vector<Entry>& entries() const { return entries_; }
    std::vector<u16> lookup(const u16& reading) const;
    // 登録。うまく登録できなければ理由 (UTF-8)。できたら空
    std::string add(const u16& reading, const u16& word, const std::string& pos, const std::string& group);
    void remove(const u16& reading, const u16& word);
    // まとめて登録 (MS-IME の一覧の出力の形 / CSV)。(登録できた数, できなかった行の説明)
    std::pair<int, std::vector<std::string>> import_text(const u16& text);
    // MS-IME の品詞名から内部の品詞
    static std::pair<std::string, std::string> pos_from_label(const u16& label);
    // 書き出し (MS-IME の「一覧の出力」と同じ形: 読み<TAB>語句<TAB>品詞)
    u16 export_text() const;
    std::vector<Form> engine_forms() const;
    // 英字の語 (表記が英字・数字・間の空白だけ): (小文字・空白なしの綴り, 表記)
    std::vector<std::pair<u16, u16>> latin_words() const;
    // 変わるたびに増える
    uint32_t version() const { return version_; }

private:
    void save();

    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    bool loaded_ = false;
    std::vector<Entry> entries_;
    uint32_t version_ = 0;
};

// ファイルを読む・書く (書くときは一時ファイルから置き換える)
bool read_file(const std::filesystem::path& p, std::string& out);
bool write_file_atomic(const std::filesystem::path& p, const std::string& data);

}  // namespace shunti

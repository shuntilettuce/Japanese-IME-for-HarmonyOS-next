// 文の流れの予測 (スマホ版 SentenceFlow と同じ。ユーザー 2026-10-13: 「お疲れ様です」を出したら「。」、続けて「本日の進捗報告です。」)。
// 入力欄に出た文字を句読点・改行・送信で区切った節の並びとして覚え、節 -> 句読点・節 -> 次の節 の流れのうち 2 回以上あったものを、
// 確定の後に出す (Tab で入れる)。端末の中だけに保存する (TSV: キー\t続き\t回数)。
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "text.h"

namespace shunti {

class SentenceFlow {
public:
    void load(const std::filesystem::path& path);
    void save();
    void clear();
    void reset();                 // 別の欄・カーソルを動かした: 途中の節はつながらない
    void boundary();              // 送信・改行: 文が終わった
    void backspace();             // 確定した文字を 1 字消した
    void feed(const u16& text);   // 入力欄に文字が出た
    std::vector<u16> predict() const;
    // 提案を選んだ回数 (よく選ぶ提案を前に。スマホ版の提案の学習と同じ。同じ表に u"S" + 表記 で入れる)
    int suggest_use(const u16& t) const;
    void record_suggest(const u16& t);

private:
    void close(const u16& punct);
    void bump(const u16& key, const u16& v);
    std::vector<u16> top(const u16& key, size_t n) const;

    std::map<u16, std::map<u16, int>> table_;   // u"P" + 節 -> 句読点、u"N" + 節 -> 次の節
    u16 buf_, prev_;
    std::filesystem::path path_;
    bool dirty_ = false;
};

}  // namespace shunti

// 日付・時刻の予測変換 (きょう → 今日の日付、いま → 今の時刻 など)。HarmonyOS 版
// DateTimePredictor.ets と同じ決まり (両方同じ中身にする)。純粋関数 (reading, now):
// 現在時刻 (UNIX 時刻) を外から渡すので決定的に試験できる。結果は一時的な値で、
// 呼び出し側は変換の学習に入れないこと (Composer の sugg_set_ を参照)。
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "text.h"

namespace shunti {

// 読み reading に対する日付・時刻の候補 (now = UNIX 時刻、ローカル時刻で扱う)。当たらなければ空。
std::vector<u16> datetime_predict(const u16& reading, std::time_t now);
// 数字だけの読みの書き換え (和暦・桁区切り・万の混ぜ書き・漢数字・丸数字)。数字だけでなければ空
std::vector<u16> number_forms(const u16& reading);

}  // namespace shunti

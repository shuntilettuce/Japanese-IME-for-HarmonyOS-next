// 物理キーの打ち間違いの直し (2026-11-15): 打鍵の単位の打ち直し (Layout::typo_variants) を読みに直し、
// 変換に渡す「読みの打ち直し」(kkc_set_alt_readings) を作る。組み立て (Composer)・ベンチ・誤爆の物差しで同じものを使う
#pragma once
#include <string>
#include <vector>

#include "layout.h"
#include "romaji.h"
#include "text.h"

namespace shunti {

// かな入力の 拗 (U+E000: 小さく)・修 (U+E001: 濁音 → 半濁音 → 元、小さく → 元)・゛・゜ を前の字 prev に付ける。
// 拗・修は前の字が変わらなくても true (使った)、゛・゜ は付けられたときだけ true (付けられなければ呼ぶ側がそのまま入れる)
bool kana_modify(char16_t& prev, char16_t c);

// 打鍵の単位ごとの出した字を読みに直す。letters = 字をローマ字 (rom の表) に流す、false = かな入力の決まり
u16 decode_unit_texts(const std::vector<u16>& outs, bool letters, Romaji& rom);

// 読みの打ち直し。orig = 打ったとおりの単位ごとの字、alts = 1 か所を直した並び、all = 今の読み全体、
// off = 変換する所の始まり (all の後ろの部分)。記録を読みに直した形が all と字数で合い、直す所の前後 1 字まで同じ
// ものだけ (句読点・数字など、組み立てで形の変わる字は直さない所にあってよい)。合わなければ false
bool alt_readings(const std::vector<u16>& orig, const std::vector<std::vector<u16>>& alts, bool letters, Romaji& rom,
                  const u16& all, size_t off, std::vector<u16>& out);

// 配列を使わない QWERTY のローマ字の単位の表 (打ったキーがそのまま単位。隣は QWERTY の物理キー)
const Layout& qwerty_layout();

// 打ったキーの並び (ローマ字) を単位にする。大文字・かな・記号の混じるものは false
bool units_from_keys(const u16& keys, std::vector<std::string>& units);

}  // namespace shunti

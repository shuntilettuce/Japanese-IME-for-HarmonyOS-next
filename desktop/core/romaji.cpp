#include "romaji.h"

#include "tables.h"

namespace shunti {

namespace {
const char16_t* CONSONANTS = u"bcdfghjklmnpqrstvwxyz";

bool has(const char16_t* s, char16_t c) {
    for (; *s; s++) if (*s == c) return true;
    return false;
}

// PC のキーボードでよく打つ綴り (HarmonyOS 版の表に無いもの)
const RomajiPair PC_EXTRA[] = {
    {u"ca", u"か"}, {u"ci", u"し"}, {u"cu", u"く"}, {u"ce", u"せ"}, {u"co", u"こ"},
    {u"cya", u"ちゃ"}, {u"cyi", u"ちぃ"}, {u"cyu", u"ちゅ"}, {u"cye", u"ちぇ"}, {u"cyo", u"ちょ"},
    {u"qa", u"くぁ"}, {u"qi", u"くぃ"}, {u"qu", u"く"}, {u"qe", u"くぇ"}, {u"qo", u"くぉ"},
    {u"kwa", u"くぁ"}, {u"gwa", u"ぐぁ"},
    {u"tsa", u"つぁ"}, {u"tsi", u"つぃ"}, {u"tse", u"つぇ"}, {u"tso", u"つぉ"},
    {u"wha", u"うぁ"}, {u"whi", u"うぃ"}, {u"whu", u"う"}, {u"whe", u"うぇ"}, {u"who", u"うぉ"},
    {u"wu", u"う"}, {u"wyi", u"ゐ"}, {u"wye", u"ゑ"},
    {u"fya", u"ふゃ"}, {u"fyu", u"ふゅ"}, {u"fyo", u"ふょ"}, {u"vyu", u"ゔゅ"},
    {u"xka", u"ヵ"}, {u"xke", u"ヶ"}, {u"lka", u"ヵ"}, {u"lke", u"ヶ"},
    {u"zh", u"←"}, {u"zj", u"↓"}, {u"zk", u"↑"}, {u"zl", u"→"},
    {u"z-", u"〜"}, {u"z.", u"…"}, {u"z,", u"‥"}, {u"z/", u"・"}, {u"z[", u"『"}, {u"z]", u"』"},
};

// かな → ローマ字で、表の中のいちばん短い綴りより、よく打つ綴りを使うもの
const RomajiPair REVERSE_PREFER[] = {
    {u"し", u"shi"}, {u"ち", u"chi"}, {u"つ", u"tsu"}, {u"ふ", u"fu"}, {u"じ", u"ji"},
    {u"しゃ", u"sha"}, {u"しゅ", u"shu"}, {u"しょ", u"sho"}, {u"ちゃ", u"cha"}, {u"ちゅ", u"chu"}, {u"ちょ", u"cho"},
    {u"じゃ", u"ja"}, {u"じゅ", u"ju"}, {u"じょ", u"jo"}, {u"ん", u"nn"},
};
}  // namespace

void Romaji::add_pair(const u16& key0, const u16& kana) {
    u16 key = key0;
    for (auto& c : key) if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    if (key.empty() || kana.empty()) return;
    table_[key] = kana;
    for (size_t i = 1; i <= key.size(); i++) prefixes_.insert(key.substr(0, i));
    for (size_t i = 1; i < key.size(); i++) longer_.insert(key.substr(0, i));
    if (kana.size() > max_kana_) max_kana_ = kana.size();
}

Romaji::Romaji() {
    auto add = [&](const RomajiPair& p) {
        u16 k = p.key, v = p.kana;
        table_[k] = v;
        for (size_t i = 1; i <= k.size(); i++) prefixes_.insert(k.substr(0, i));
        if (k.find_first_not_of(u"abcdefghijklmnopqrstuvwxyz") != u16::npos) return;   // 記号の綴りは逆引きに使わない
        auto it = reverse_.find(v);
        if (it == reverse_.end() || k.size() < it->second.size()) reverse_[v] = k;
        if (v.size() > max_kana_) max_kana_ = v.size();
    };
    for (const auto& p : ROMAJI_TABLE) add(p);
    for (const auto& p : PC_EXTRA) add(p);
    for (const auto& p : REVERSE_PREFER) reverse_[p.key] = p.kana;
    prefixes_.insert(u"n");
    prefixes_.insert(u"n'");
}

bool Romaji::accepts(char16_t key) const {
    if (key >= u'A' && key <= u'Z') key = char16_t(key + 32);
    return prefixes_.count(buffer_ + key) > 0;
}

Romaji::Result Romaji::process(char16_t key) {
    if (key >= u'A' && key <= u'Z') key = char16_t(key + 32);
    buffer_.push_back(key);
    if (buffer_.size() >= 2) {
        char16_t c0 = buffer_[0];
        if (!bare_ && c0 == buffer_[1] && c0 != u'n' && has(CONSONANTS, c0) && !(extended_ && prefixes_.count(buffer_))) {
            buffer_ = buffer_.substr(1);
            Result sub = process_buffered();
            return {u"っ" + sub.committed, sub.pending};
        }
    }
    return process_buffered();
}

Romaji::Result Romaji::process_buffered() {
    if (buffer_.empty()) return {u"", u""};
    if (buffer_ == u"n" && !bare_) return {u"", u"n"};
    // n' → ん (ACT・DvorakJP のように表に n' があるときは表のとおり: n' = ない)
    if (!bare_ && buffer_.size() >= 2 && buffer_[0] == u'n' && buffer_[1] == u'\'' && !(extended_ && table_.count(u"n'"))) {
        buffer_ = buffer_.substr(2);
        Result sub = process_buffered();
        return {u"ん" + sub.committed, sub.pending};
    }
    if (!bare_ && buffer_.size() >= 2 && buffer_[0] == u'n' && !has(u"aiueoyn", buffer_[1]) && !(extended_ && prefixes_.count(buffer_))) {
        buffer_ = buffer_.substr(1);
        Result sub = process_buffered();
        return {u"ん" + sub.committed, sub.pending};
    }
    auto it = table_.find(buffer_);
    // 拡張ローマ字で、それだけで決まる綴りが長い綴りの頭も兼ねるとき (gACT10 の r = っ と rg = ぃ) は続きを待つ。
    // 続かなかったら下の「頭のいちばん長い綴り」で出る
    if (it != table_.end() && !(extended_ && longer_.count(buffer_))) {
        buffer_.clear();
        return {it->second, u""};
    }
    if (prefixes_.count(buffer_)) return {u"", buffer_};
    for (size_t i = buffer_.size() - 1; i >= 1; i--) {
        auto hit = table_.find(buffer_.substr(0, i));
        if (hit == table_.end()) continue;
        u16 kana = hit->second;
        buffer_ = buffer_.substr(i);
        Result sub = process_buffered();
        return {kana + sub.committed, sub.pending};
    }
    u16 emitted = buffer_.substr(0, 1);
    buffer_ = buffer_.substr(1);
    if (!buffer_.empty()) {
        Result sub = process_buffered();
        return {emitted + sub.committed, sub.pending};
    }
    return {emitted, u""};
}

u16 Romaji::flush() {
    u16 p = buffer_;
    buffer_.clear();
    if (p == u"n" && !bare_) return u"ん";
    if (extended_ && !p.empty()) {   // 続きを待っていた綴り (r = っ など) と、その後に残った分
        auto it = table_.find(p);
        if (it != table_.end()) return it->second;
        for (size_t i = p.size() - 1; i >= 1; i--) {
            auto hit = table_.find(p.substr(0, i));
            if (hit == table_.end()) continue;
            buffer_ = p.substr(i);
            return hit->second + flush();
        }
    }
    return p;
}

u16 Romaji::to_romaji(const u16& kana0) const {
    u16 kana = to_hiragana(kana0);
    u16 out;
    bool sokuon = false;
    for (size_t i = 0; i < kana.size();) {
        char16_t c = kana[i];
        if (c == u'っ' && i + 1 < kana.size() && is_hira(kana[i + 1])) { sokuon = true; i++; continue; }
        if (c == u'ー') { out += u"-"; i++; continue; }
        u16 piece;
        size_t len = 0;
        for (size_t n = std::min(max_kana_, kana.size() - i); n >= 1; n--) {
            auto it = reverse_.find(kana.substr(i, n));
            if (it != reverse_.end()) { piece = it->second; len = n; break; }
        }
        if (!len) { piece = kana.substr(i, 1); len = 1; }
        if (c == u'ん' && (i + 1 >= kana.size() || !has(u"あいうえおやゆよなにぬねの", kana[i + 1]))) piece = u"n";
        if (sokuon && !piece.empty() && has(CONSONANTS, piece[0])) out.push_back(piece[0]);
        else if (sokuon) out += u"xtu";
        sokuon = false;
        out += piece;
        i += len;
    }
    return out;
}

}  // namespace shunti

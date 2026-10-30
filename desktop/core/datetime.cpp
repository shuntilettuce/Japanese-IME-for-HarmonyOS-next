#include "datetime.h"

#include <algorithm>
#include <cstdlib>

namespace shunti {
namespace {

const char16_t* DOW = u"日月火水木金土";                       // 日〜土 (tm_wday と同じ並び)
const char16_t* DOW_FULL[] = {u"日曜日", u"月曜日", u"火曜日", u"水曜日", u"木曜日", u"金曜日", u"土曜日"};

// ---------------------------------------------------------------- 暦の計算 (1970-01-01 からの日数)
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const int mp = (int)((5u * doy + 2u) / 153u);
    const int dd = (int)(doy - (153u * (unsigned)mp + 2u) / 5u + 1u);
    const int mm = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (mm <= 2));
    m = mm;
    d = dd;
}
int wday_of_days(int64_t days) { return (int)(((days + 4) % 7 + 7) % 7); }   // 0=日 .. 6=土
int days_in_month(int y, int m) {
    static const int DM[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return DM[m - 1];
}

// ローカル時刻 (Windows は localtime_s、他は localtime_r)
void local_breakdown(std::time_t now, int& y, int& mo, int& d, int& hh, int& mi) {
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    y = t.tm_year + 1900;
    mo = t.tm_mon + 1;
    d = t.tm_mday;
    hh = t.tm_hour;
    mi = t.tm_min;
}

// ---------------------------------------------------------------- 数の読み (算用数字とかな)
struct Piece { const char16_t* k; char kind; int64_t v; };
struct Sok { const char16_t* k; int64_t v, unit; };

// 長い方から (貪欲に当てる)
const Piece PIECES[] = {
    {u"きゅう", 'd', 9}, {u"じゅう", 's', 10}, {u"ひゃく", 's', 100}, {u"びゃく", 's', 100}, {u"ぴゃく", 's', 100}, {u"ちょう", 'b', 1000000000000LL},
    {u"いち", 'd', 1}, {u"さん", 'd', 3}, {u"よん", 'd', 4}, {u"ろく", 'd', 6}, {u"なな", 'd', 7}, {u"しち", 'd', 7}, {u"はち", 'd', 8},
    {u"れい", 'd', 0}, {u"ぜろ", 'd', 0}, {u"いっ", 'd', 1}, {u"ろっ", 'd', 6}, {u"はっ", 'd', 8},
    {u"せん", 's', 1000}, {u"ぜん", 's', 1000}, {u"まん", 'b', 10000}, {u"おく", 'b', 100000000LL},
    {u"に", 'd', 2}, {u"し", 'd', 4}, {u"ご", 'd', 5}, {u"く", 'd', 9},
};
const Sok SOKUON[] = {
    {u"じゅっ", 10, 10}, {u"じっ", 10, 10}, {u"ひゃっ", 100, 100}, {u"せっ", 1000, 1000},
    {u"いっ", 1, 1}, {u"ろっ", 6, 1}, {u"はっ", 8, 1},
};

size_t clen(const char16_t* s) { return std::char_traits<char16_t>::length(s); }
bool starts_with(const u16& s, size_t i, const char16_t* k) { const size_t L = clen(k); return i + L <= s.size() && s.compare(i, L, k, L) == 0; }
bool ends_with(const u16& s, const char16_t* k) { const size_t L = clen(k); return s.size() >= L && s.compare(s.size() - L, L, k, L) == 0; }

u16 num(int64_t v) {
    if (v == 0) return u"0";
    std::string a = std::to_string(v);
    u16 o;
    for (char c : a) o += char16_t(c);
    return o;
}
u16 num2(int v) { return v < 10 ? u16(u"0") + char16_t(char16_t('0' + v)) : num(v); }

// かなの数 (2 字以上)。engine.cpp の parse と同じ決まり
bool parse_kana(const u16& r, int64_t& value) {
    if (r.size() < 2) return false;
    const Sok* sok = nullptr;
    for (auto& s : SOKUON)
        if (ends_with(r, s.k)) { sok = &s; break; }
    u16 body = sok ? r.substr(0, r.size() - clen(sok->k)) : r;
    std::vector<std::pair<char, int64_t>> toks;
    size_t i = 0;
    u16 prev;
    while (i < body.size()) {
        bool hit = false;
        for (auto& p : PIECES) {
            if (!starts_with(body, i, p.k)) continue;
            if ((u16(p.k) == u"ぜん" || u16(p.k) == u"びゃく") && prev != u"さん" && prev != u"なん") return false;
            if (u16(p.k) == u"ぴゃく" && prev != u"ろっ" && prev != u"はっ") return false;
            toks.push_back({p.kind, p.v});
            prev = p.k;
            i += clen(p.k);
            hit = true;
            break;
        }
        if (!hit) return false;
    }
    if (sok) {
        if (sok->unit > 1 && sok->v == sok->unit) toks.push_back({'s', sok->unit});
        else toks.push_back({'d', sok->v});
    }
    if (toks.empty()) return false;
    int64_t total = 0, group = 0, last_small = 100000;
    bool has_digit = false;
    int64_t digit = 0;
    for (auto& t : toks) {
        if (t.first == 'd') {
            if (has_digit) return false;
            has_digit = true;
            digit = t.second;
        } else if (t.first == 's') {
            if (t.second >= last_small) return false;
            group += (has_digit ? digit : 1) * t.second;
            has_digit = false;
            last_small = t.second;
        } else {
            group += has_digit ? digit : 0;
            if (group == 0) return false;
            total += group * t.second;
            group = 0;
            has_digit = false;
            last_small = 100000;
        }
    }
    group += has_digit ? digit : 0;
    value = total + group;
    if (value == 0 && !(toks.size() == 1 && toks[0].first == 'd' && toks[0].second == 0)) return false;
    return true;
}

int single_kana(char16_t c) {
    switch (c) {
        case u'に': return 2;
        case u'し': return 4;
        case u'ご': return 5;
        case u'く': return 9;
        default: return -1;
    }
}

// 数の読み 1 語 (算用数字 or かな)。読めたら value に
bool parse_number_seg(const u16& seg, int64_t& v) {
    if (seg.empty()) return false;
    bool arabic = true;
    for (char16_t c : seg) {
        if (!((c >= u'0' && c <= u'9') || (c >= 0xFF10 && c <= 0xFF19))) { arabic = false; break; }
    }
    if (arabic) {
        int64_t x = 0;
        for (char16_t c : seg) x = x * 10 + (c >= 0xFF10 ? c - 0xFF10 : c - u'0');
        v = x;
        return true;
    }
    if (seg.size() == 1) {
        const int s = single_kana(seg[0]);
        if (s >= 0) { v = s; return true; }
        return false;
    }
    return parse_kana(seg, v);
}

// ---------------------------------------------------------------- 和暦
// 日付の形に添える和暦 (HarmonyOS 版 wareki と同じ。令和だけ)
u16 wareki(int y, int m, int d) {
    if (y > 2019 || (y == 2019 && (m > 5 || (m == 5 && d >= 1)))) {
        const int r = y - 2018;
        return r == 1 ? u16(u"令和元年") : u16(u"令和") + num(r) + u"年";
    }
    return u16();
}
// 年の和暦 (令和・平成・昭和)
u16 era_year(int y) {
    if (y >= 2019) return y == 2019 ? u16(u"令和元年") : u16(u"令和") + num(y - 2018) + u"年";
    if (y >= 1989) return y == 1989 ? u16(u"平成元年") : u16(u"平成") + num(y - 1988) + u"年";
    if (y >= 1926) return y == 1926 ? u16(u"昭和元年") : u16(u"昭和") + num(y - 1925) + u"年";
    return u16();
}

// ---------------------------------------------------------------- 形を作る
// 日の形 (11 通り。HarmonyOS 版 dateForms と同じ並び)
std::vector<u16> date_forms(int64_t days) {
    int y, m, d;
    civil_from_days(days, y, m, d);
    const int w = wday_of_days(days);
    const u16 Y = num(y), M = num(m), D = num(d);
    const u16 dow1(1, DOW[w]);
    const u16 dowf = DOW_FULL[w];
    std::vector<u16> out = {
        Y + u"年" + M + u"月" + D + u"日",
        Y + u"年" + M + u"月" + D + u"日(" + dow1 + u")",
        M + u"月" + D + u"日",
        M + u"月" + D + u"日(" + dow1 + u")",
        M + u"月" + D + u"日(" + dowf + u")",
        M + u"/" + D,
        M + u"/" + D + u"(" + dow1 + u")",
        Y + u"/" + M + u"/" + D,
        Y + u"-" + M + u"-" + D,
    };
    const u16 w_ = wareki(y, m, d);
    if (!w_.empty()) {
        out.push_back(w_ + M + u"月" + D + u"日");
        out.push_back(w_ + M + u"月" + D + u"日(" + dow1 + u")");
    }
    out.push_back(Y + num2(m) + num2(d));   // 数字 8 桁 (20261021。8 桁から日付にする向きの逆。ユーザー 2026-10-20)
    return out;
}

// <数>がつ<数>にち の日付に曜日をつけた形 (4 通り)
std::vector<u16> date_dow_forms(int y, int m, int d) {
    const int64_t days = days_from_civil(y, m, d);
    const int w = wday_of_days(days);
    const u16 Y = num(y), M = num(m), D = num(d);
    const u16 dow1(1, DOW[w]);
    const u16 dowf = DOW_FULL[w];
    return {
        M + u"月" + D + u"日(" + dow1 + u")",
        M + u"月" + D + u"日(" + dowf + u")",
        M + u"/" + D + u"(" + dow1 + u")",
        Y + u"年" + M + u"月" + D + u"日(" + dow1 + u")",
    };
}

std::vector<u16> time_forms(int y, int m, int d, int hh, int mi) {
    const int h12 = hh % 12 == 0 ? 12 : hh % 12;
    const u16 mm = num2(mi);
    const u16 ampm = hh < 12 ? u"午前" : u"午後";
    return {
        num(hh) + u":" + mm,
        num(hh) + u"時" + num(mi) + u"分",
        ampm + num(h12) + u"時" + num(mi) + u"分",
        num(m) + u"月" + num(d) + u"日 " + num(hh) + u":" + mm,
    };
}

std::vector<u16> year_offset_forms(int y) {
    std::vector<u16> out = {num(y) + u"年"};
    const u16 w = wareki(y, 6, 15);
    if (!w.empty()) out.push_back(w);
    return out;
}

std::vector<u16> month_forms(int y, int m) { return {num(y) + u"年" + num(m) + u"月", num(m) + u"月"}; }

std::vector<u16> week_range_forms(int64_t monday) {
    int y1, m1, d1, y2, m2, d2;
    civil_from_days(monday, y1, m1, d1);
    civil_from_days(monday + 6, y2, m2, d2);
    return {
        num(m1) + u"月" + num(d1) + u"日〜" + num(m2) + u"月" + num(d2) + u"日",
        num(m1) + u"/" + num(d1) + u"〜" + num(m2) + u"/" + num(d2),
    };
}

// 曜日の読み (にちようび…どようび / にちよう…どよう) -> 0=日..6=土
int weekday_of(const u16& s) {
    static const char16_t* WDR[] = {u"にち", u"げつ", u"か", u"すい", u"もく", u"きん", u"ど"};
    for (int w = 0; w < 7; w++) {
        if (s == u16(WDR[w]) + u"ようび" || s == u16(WDR[w]) + u"よう") return w;
    }
    return -1;
}

struct OffsetEntry { const char16_t* k; int off; };
const OffsetEntry DAY_OFFSETS[] = {
    {u"きょう", 0}, {u"ほんじつ", 0},
    {u"あした", 1}, {u"あす", 1}, {u"みょうにち", 1},
    {u"きのう", -1}, {u"さくじつ", -1},
    {u"あさって", 2}, {u"みょうごにち", 2}, {u"しあさって", 3},
    {u"おととい", -2}, {u"いっさくじつ", -2}, {u"さきおととい", -3},
};
const OffsetEntry YEAR_OFFSETS[] = {
    {u"ことし", 0}, {u"こんねん", 0},
    {u"らいねん", 1}, {u"みょうねん", 1},
    {u"きょねん", -1}, {u"さくねん", -1}, {u"さらいねん", 2},
};
const OffsetEntry MONTH_OFFSETS[] = {{u"こんげつ", 0}, {u"らいげつ", 1}, {u"せんげつ", -1}};
const OffsetEntry WEEKDAY_WEEKS[] = {{u"さらいしゅう", 2}, {u"らいしゅう", 1}, {u"こんしゅう", 0}, {u"せんしゅう", -1}};
const OffsetEntry WEEK_RANGES[] = {{u"らいしゅう", 1}, {u"こんしゅう", 0}, {u"せんしゅう", -1}};
const char16_t* TIME_READINGS[] = {u"いま", u"げんざい", u"ただいま"};

bool find_offset(const OffsetEntry* t, size_t n, const u16& s, int& off) {
    for (size_t i = 0; i < n; i++)
        if (s == t[i].k) { off = t[i].off; return true; }
    return false;
}

// <数>がつ<数>にち [<数>ねん 付きも]。曜日のついた日付の読み
bool parse_month_day(const u16& r, int& y, int& m, int& d, bool& has_year) {
    const size_t g = r.find(u"がつ");
    if (g == u16::npos) return false;
    const u16 before = r.substr(0, g);
    const u16 after = r.substr(g + 2);
    int year = 0;
    bool hy = false;
    const size_t n = before.find(u"ねん");
    if (n != u16::npos) {
        const u16 yr = before.substr(0, n);
        const u16 mo = before.substr(n + 2);
        int64_t yv = 0;
        static const struct { const char16_t* k; int64_t base; } ERAS[] = {
            {u"れいわ", 2018}, {u"へいせい", 1988}, {u"しょうわ", 1925},
        };
        for (auto& e : ERAS) {
            if (!starts_with(yr, 0, e.k)) continue;
            const u16 nseg = yr.substr(clen(e.k));
            int64_t nv;
            if (nseg == u"がん") nv = 1;
            else if (!parse_number_seg(nseg, nv)) return false;
            yv = e.base + nv;
            hy = true;
            break;
        }
        if (!hy) {
            if (!parse_number_seg(yr, yv) || yv < 1) return false;
            hy = true;
        }
        year = (int)yv;
        int64_t mv;
        if (!parse_number_seg(mo, mv) || mv < 1 || mv > 12) return false;
        m = (int)mv;
    } else {
        int64_t mv;
        if (!parse_number_seg(before, mv) || mv < 1 || mv > 12) return false;
        m = (int)mv;
    }
    static const struct { const char16_t* k; int d; } SP[] = {
        {u"ついたち", 1}, {u"ふつか", 2}, {u"みっか", 3}, {u"よっか", 4}, {u"いつか", 5}, {u"むいか", 6},
        {u"なのか", 7}, {u"ようか", 8}, {u"ここのか", 9}, {u"とおか", 10}, {u"じゅうよっか", 14},
        {u"はつか", 20}, {u"にじゅうよっか", 24}, {u"みそか", 30},
    };
    for (auto& s : SP)
        if (after == s.k) { d = s.d; y = year; has_year = hy; return true; }
    if (ends_with(after, u"にち")) {
        int64_t dv;
        const u16 body = after.substr(0, after.size() - 2);
        if (parse_number_seg(body, dv) && dv >= 1 && dv <= 31) { d = (int)dv; y = year; has_year = hy; return true; }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- 数字の書き換え (スマホ版 NumberFormatter と同じ形に、和暦・丸数字を足す)
namespace {
const char16_t* const DIGIT_KANJI[] = {u"〇", u"一", u"二", u"三", u"四", u"五", u"六", u"七", u"八", u"九"};
const char16_t* const SMALL_UNITS[] = {u"", u"十", u"百", u"千"};
const char16_t* const BIG_UNITS[] = {u"", u"万", u"億", u"兆", u"京"};
constexpr size_t MAX_DIGITS = 20;

u16 group_kanji(const u16& g) {   // 1〜4 桁 (千・百・十の前の一は書かない)
    u16 out;
    for (size_t i = 0; i < g.size(); i++) {
        const int d = g[i] - u'0';
        const size_t place = g.size() - 1 - i;
        if (d == 0) continue;
        if (d != 1 || place == 0) out += DIGIT_KANJI[d];
        out += SMALL_UNITS[place];
    }
    return out;
}

std::vector<u16> groups_of(const u16& s) {   // 右から 4 桁ずつ。[0] がいちばん下の組
    std::vector<u16> g;
    for (size_t end = s.size(); end > 0; end = end > 4 ? end - 4 : 0) g.push_back(s.substr(end > 4 ? end - 4 : 0, end > 4 ? 4 : end));
    return g;
}

u16 to_kanji(const u16& s) {
    const auto gs = groups_of(s);
    u16 out;
    for (size_t i = gs.size(); i-- > 0;) {
        const u16 k = group_kanji(gs[i]);
        if (!k.empty()) out += k + BIG_UNITS[i];
    }
    return out;
}

u16 to_man_mixed(const u16& s) {   // 12345678 → 1234万5678・1000000 → 100万
    const auto gs = groups_of(s);
    u16 out;
    for (size_t i = gs.size(); i-- > 0;) {
        if (gs[i].find_first_not_of(u'0') == u16::npos) continue;
        out += (out.empty() ? gs[i] : u16(4 - gs[i].size(), u'0') + gs[i]) + BIG_UNITS[i];
    }
    return out;
}

u16 add_commas(const u16& s) {
    u16 out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i > 0 && (s.size() - i) % 3 == 0) out += u',';
        out += s[i];
    }
    return out;
}

char16_t circled(int v) {   // 白丸 0〜50
    if (v == 0) return char16_t(0x24EA);
    if (v <= 20) return char16_t(0x2460 + v - 1);
    if (v <= 35) return char16_t(0x3251 + v - 21);
    return char16_t(0x32B1 + v - 36);
}
char16_t black_circled(int v) { return char16_t(v <= 10 ? 0x2776 + v - 1 : 0x24EB + v - 11); }   // 黒丸に白抜き 1〜20
}  // namespace

std::vector<u16> number_forms(const u16& reading) {
    if (reading.empty() || reading.size() > MAX_DIGITS) return {};
    u16 d;
    for (char16_t c : reading) {
        if (c >= u'0' && c <= u'9') d += c;
        else if (c >= 0xFF10 && c <= 0xFF19) d += char16_t(u'0' + (c - 0xFF10));
        else return {};
    }
    if (d[0] == u'0' && d.size() > 1) return {};   // 0 で始まるのは番号
    std::vector<u16> out;
    auto push = [&](const u16& s) {
        if (!s.empty() && s != reading && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    int v = -1;
    if (d.size() <= 4) {
        v = 0;
        for (char16_t c : d) v = v * 10 + (c - u'0');
    }
    if (d.size() == 4 && v >= 1926 && v <= 2100) push(era_year(v));
    if (d.size() >= 4) {
        push(add_commas(d));
        push(to_man_mixed(d));
    }
    if (d == u"0") push(u"〇");
    else push(to_kanji(d));
    if (v >= 0 && v <= 50) push(u16(1, circled(v)));
    if (v >= 1 && v <= 20) push(u16(1, black_circled(v)));
    return out;
}

std::vector<u16> datetime_predict(const u16& reading, std::time_t now) {
    int y, mo, d, hh, mi;
    local_breakdown(now, y, mo, d, hh, mi);
    const int64_t today = days_from_civil(y, mo, d);
    const int wday = wday_of_days(today);

    // 数字 8 桁の日付 (20261018 → 2026年10月18日(日)・2026/10/18 など)。ありえない日付は出さない (ブログの要望 2026-10-18、Beki IME のように)
    if (reading.size() == 8) {
        int v[8], k = 0;
        for (char16_t c : reading) {
            if (c >= u'0' && c <= u'9') v[k++] = c - u'0';
            else if (c >= 0xFF10 && c <= 0xFF19) v[k++] = c - 0xFF10;
            else break;
        }
        if (k == 8) {
            const int Y = v[0] * 1000 + v[1] * 100 + v[2] * 10 + v[3], M = v[4] * 10 + v[5], D = v[6] * 10 + v[7];
            if (Y >= 1900 && Y <= 2100 && M >= 1 && M <= 12 && D >= 1 && D <= 31) {
                const int64_t days = days_from_civil(Y, M, D);
                int y2, m2, d2;
                civil_from_days(days, y2, m2, d2);
                if (y2 == Y && m2 == M && d2 == D) {
                    std::vector<u16> out = date_forms(days);
                    for (auto& f : number_forms(reading)) out.push_back(f);
                    return out;
                }
            }
        }
    }
    {
        std::vector<u16> nf = number_forms(reading);
        if (!nf.empty()) return nf;
    }
    int off;
    if (find_offset(DAY_OFFSETS, sizeof(DAY_OFFSETS) / sizeof(DAY_OFFSETS[0]), reading, off)) return date_forms(today + off);
    if (find_offset(YEAR_OFFSETS, sizeof(YEAR_OFFSETS) / sizeof(YEAR_OFFSETS[0]), reading, off)) return year_offset_forms(y + off);
    for (auto* t : TIME_READINGS)
        if (reading == t) return time_forms(y, mo, d, hh, mi);
    if (reading == u"ようび" || reading == u"なんようび")
        return {u16(DOW_FULL[wday]), u16(1, DOW[wday]), u16(u"(") + char16_t(DOW[wday]) + u")"};
    if (find_offset(WEEK_RANGES, sizeof(WEEK_RANGES) / sizeof(WEEK_RANGES[0]), reading, off))
        return week_range_forms(today - (wday + 6) % 7 + off * 7);
    if (find_offset(MONTH_OFFSETS, sizeof(MONTH_OFFSETS) / sizeof(MONTH_OFFSETS[0]), reading, off)) {
        int tm = mo + off, ty = y;
        if (tm <= 0) { tm += 12; ty--; }
        if (tm > 12) { tm -= 12; ty++; }
        return month_forms(ty, tm);
    }
    // こんしゅうのげつようび・らいしゅうのきんようび … (の の有無どちらも、さらいしゅう も)
    for (auto& wk : WEEKDAY_WEEKS) {
        const size_t L = clen(wk.k);
        if (reading.size() <= L || reading.compare(0, L, wk.k, L) != 0) continue;
        u16 rest = reading.substr(L);
        if (starts_with(rest, 0, u"の")) rest = rest.substr(1);
        const int w = weekday_of(rest);
        if (w < 0) continue;
        const int mon_off = (wday + 6) % 7;
        const int target = (w + 6) % 7;
        return date_forms(today - mon_off + target + wk.off * 7);
    }
    // 曜日だけ (げつようび …): 今日か次のその曜日
    {
        const int w = weekday_of(reading);
        if (w >= 0) return date_forms(today + ((w - wday + 7) % 7));
    }
    // <数>がつ<数>にち [<数>ねん 付きも]: 日付に曜日をつける
    {
        int yv, mv, dv;
        bool hy;
        if (parse_month_day(reading, yv, mv, dv, hy)) {
            int year = hy ? yv : y;
            if (!hy && today - days_from_civil(year, mv, dv) >= 60) year = y + 1;
            if (dv > days_in_month(year, mv)) return {};
            return date_dow_forms(year, mv, dv);
        }
    }
    // <数>ねん (西暦 -> 和暦) / れいわ<数>ねん … (和暦 -> 西暦)
    if (ends_with(reading, u"ねん")) {
        const u16 body = reading.substr(0, reading.size() - 2);
        static const struct { const char16_t* k; int64_t base; } ERAS[] = {
            {u"れいわ", 2018}, {u"へいせい", 1988}, {u"しょうわ", 1925},
        };
        for (auto& e : ERAS) {
            if (!starts_with(body, 0, e.k)) continue;
            const u16 nseg = body.substr(clen(e.k));
            int64_t nv;
            if (nseg == u"がん") nv = 1;
            else if (!parse_number_seg(nseg, nv)) return {};
            const int64_t Y = e.base + nv;
            if (Y < 1) return {};
            return {num(Y) + u"年"};
        }
        int64_t Y;
        if (parse_number_seg(body, Y) && Y >= 1926 && Y <= 2100) {
            const u16 w = era_year((int)Y);
            if (!w.empty()) return {w};
        }
    }
    return {};
}

}  // namespace shunti

// PC 版の共通部分 (desktop/core) の試験。IME なしで打鍵を流し込み、入力中の見せ方と確定した文を確かめる。
//   core_test.exe <kkc_lex.bin> <kkc_model.bin> [作業用のフォルダ]
// 期待と違えば FAIL を出し、終了コードを 1 にする。変換の時間も出す。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../core/composer.h"
#include "../core/release_info.h"
#include "../core/datetime.h"
#include "../core/layout.h"
#include "../core/pos_classes.h"
#include "../core/romaji.h"
#include "../core/settings.h"
#include "../core/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace shunti;
namespace fs = std::filesystem;

static int failures = 0;

static std::string u8(const u16& s) { return to_utf8(s); }

static void expect(const char* name, const u16& got, const u16& want) {
    if (got == want) { printf("ok   %s: %s\n", name, u8(got).c_str()); return; }
    printf("FAIL %s: got [%s] want [%s]\n", name, u8(got).c_str(), u8(want).c_str());
    failures++;
}

static std::vector<char> slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 打鍵の並び: 普通の字はそのまま、{sp} {enter} {esc} {bs} {del} {left} {right} {up} {down} {home} {end} {tab}
// {pgdn} {pgup} {f6}〜{f10} {henkan} {muhenkan}、Shift つきは {S-left} のように
static void keys(Composer& c, const std::string& seq) {
    u16 s = from_utf8(seq);
    for (size_t i = 0; i < s.size(); i++) {
        KeyEvent ev;
        if (s[i] == u'{') {
            size_t j = s.find(u'}', i);
            std::string name = to_utf8(s.substr(i + 1, j - i - 1));
            i = j;
            if (name.rfind("S-", 0) == 0) { ev.shift = true; name = name.substr(2); }
            struct { const char* n; Key k; } names[] = {
                {"sp", Key::Space}, {"enter", Key::Enter}, {"esc", Key::Escape}, {"bs", Key::Backspace}, {"del", Key::Delete},
                {"left", Key::Left}, {"right", Key::Right}, {"up", Key::Up}, {"down", Key::Down}, {"home", Key::Home},
                {"end", Key::End}, {"tab", Key::Tab}, {"pgdn", Key::PageDown}, {"pgup", Key::PageUp}, {"f6", Key::F6},
                {"f7", Key::F7}, {"f8", Key::F8}, {"f9", Key::F9}, {"f10", Key::F10}, {"henkan", Key::Henkan},
                {"muhenkan", Key::Muhenkan},
            };
            bool found = false;
            for (auto& n : names) if (name == n.n) { ev.key = n.k; found = true; }
            if (!found) { printf("unknown key %s\n", name.c_str()); failures++; }
        } else {
            ev.key = Key::Char;
            ev.ch = s[i];
            ev.shift = s[i] >= u'A' && s[i] <= u'Z';
        }
        c.press(ev);
    }
}

static u16 segs_of(const View& v) {
    u16 o;
    for (auto& sp : v.spans) {
        if (!o.empty()) o += u"|";
        if (sp.kind == SpanKind::Focused) o += u"[";
        o += v.text.substr(size_t(sp.start), size_t(sp.len));
        if (sp.kind == SpanKind::Focused) o += u"]";
    }
    return o;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) { printf("usage: core_test <lex> <model> [workdir]\n"); return 2; }
    fs::path work = argc > 3 ? fs::path(argv[3]) : fs::temp_directory_path() / "shunti_core_test";
    fs::create_directories(work);
    fs::remove(work / "learned.json");
    fs::remove(work / "userdict.json");

    // ---- 打ち間違いの直しの強さ・文脈を読む字数 (詳細設定) が保存して読み直せる 2026-11-06
    {
        Settings a;
        a.typo_level = 2;
        a.ctx_left = 10;
        a.ctx_right = 999;
        save_settings(work / "knob_settings.json", a);
        Settings b;
        load_settings(work / "knob_settings.json", b);
        if (b.typo_level == 2 && b.ctx_left == 10 && b.ctx_right == CTX_LIMIT && typo_bias_of(2) < 0 && typo_bias_of(0) > 0 && typo_bias_of(1) == 0)
            printf("ok   typo/ctx level saved\n");
        else { printf("FAIL typo/ctx level: %d %d %d\n", b.typo_level, b.ctx_left, b.ctx_right); failures++; }
    }

    // ---- 配列 (同時押し・親指・空白シフト・3 キー、同梱の表が読めること) 2026-10-16
    {
        auto texts = [](const std::vector<Layout::Out>& v) {
            u16 t;
            for (auto& o : v) t += o.key.empty() ? o.text : u"{" + from_utf8(o.key) + u"}";
            return t;
        };
        Layout L;
        std::string err;
        L.load("#! kind chord\na\tう\nk\tの\nlthumb+a\tを\nrthumb+a\tゔ\na+k\tA\nj+k+l\tX\nj\tj\nk+l\tY\nspace+a\tS\n", &err);
        // 押す・離すを順に (式の中で並べると呼ぶ順番が決まらないので、1 つずつ足していく)
        u16 got;
        auto D_ = [&](const char* k, uint64_t t) { got += texts(L.down(k, false, t)); };
        auto U_ = [&](const char* k, uint64_t t) { got += texts(L.up(k, t)); };
        auto T_ = [&](uint64_t t) { got += texts(L.tick(t)); };
        auto check = [&](const char* name, const u16& want) { expect(name, got, want); got.clear(); L.reset(); };
        D_("a", 0); D_("k", 20); U_("a", 80); U_("k", 85); check("chord: 時間内の同時押し", u"A");
        D_("a", 100); T_(200); U_("a", 210); check("chord: 時間切れは単打", u"う");
        D_("a", 300); U_("a", 310); check("chord: 離したら単打", u"う");
        D_("a", 400); D_("lthumb", 410); U_("a", 460); U_("lthumb", 470); check("thumb: 文字→親指 (順不同)", u"を");
        D_("lthumb", 450); U_("lthumb", 460); check("thumb: 単独は無変換", u"{muhenkan}");
        D_("rthumb", 500); D_("a", 520); U_("a", 530); U_("rthumb", 540); check("thumb: 親指→文字", u"ゔ");
        D_("j", 600); D_("k", 605); D_("l", 610); U_("j", 670); U_("k", 672); U_("l", 674); check("3 キー", u"X");
        D_("space", 700); D_("a", 800); U_("a", 810); U_("space", 820); check("space シフト", u"S");
        D_("space", 900); U_("space", 950); check("space 単独", u"{space}");
        D_("a", 1000); D_("k", 1200); T_(1400); U_("a", 1410); U_("k", 1410); check("時間をおいた 2 打は別々", u"うの");
        D_("a", 1500); D_("k", 1540); U_("a", 1570); U_("k", 1610); check("速打ちの重なり (ロールオーバー) は別々", u"うの");
        D_("a", 1600); D_("j", 1620); U_("j", 1640); T_(1720); check("押したままの字の中でタップ: 長押しの字は待たずに出る", u"うj");
        U_("a", 1730); got.clear(); L.reset();
        D_("a", 1650); D_("k", 1660); D_("a", 2200); D_("k", 2230); U_("a", 2300); U_("k", 2310); check("組を押したままのリピートは重ならない", u"A");
        D_("lthumb", 1700); D_("a", 1720); U_("a", 1780); D_("a", 1800); U_("a", 1860); U_("lthumb", 1900); check("連続シフト", u"をを");
        // 流儀の設定 (連続シフト・親指キーの割り当て・単独打鍵・キーリピート)
        L.continuous = false;
        D_("lthumb", 2400); D_("a", 2420); U_("a", 2480); D_("a", 2500); U_("a", 2560); U_("lthumb", 2600); check("連続シフトを切る: 2 字目は単打", u"をう");
        L.continuous = true;
        L.thumbs = 1;   // 無変換・空白: 空白が右親指
        if (L.map_key("space") != "rthumb" || !L.map_key("rthumb").empty()) { printf("FAIL map_key 無変換・空白\n"); failures++; }
        D_("rthumb", 2700); D_("a", 2710); U_("a", 2770); U_("rthumb", 2780); check("空白を右親指に: 空白+字", u"ゔ");
        D_("rthumb", 2800); U_("rthumb", 2850); check("空白を右親指に: 単独は空白", u"{space}");
        L.thumbs = 0;
        L.thumb_tap = false;
        D_("lthumb", 2900); U_("lthumb", 2950); check("親指の単独打鍵を切る", u"");
        L.thumb_tap = true;
        L.repeat = false;
        D_("j", 3000); T_(3200); D_("j", 3500); D_("j", 3530); U_("j", 3600); check("キーリピートを切る", u"j");
        L.repeat = true;
        Layout D;
        D.load("#! kind letters\nq\t'\nw\t,\na\ta\n");
        expect("letters: Shift の行が無い英字は大文字", texts(D.down("a", true, 0)), u"A");
        fs::path dir = fs::path(argv[1]).parent_path() / ".." / ".." / ".." / ".." / ".." / "desktop" / "layouts";
        {   // 前置シフト (月配列): d を打ってから e
            Layout P;
            P.load("#! kind kana\nd\t{p1}\ne\tし\np1>e\tね\n");
            u16 t;
            for (auto& o : P.down("d", false, 0)) t += o.text;
            P.up("d", 10);
            for (auto& o : P.down("e", false, 20)) t += o.text;
            P.up("e", 30);
            for (auto& o : P.down("e", false, 40)) t += o.text;
            expect("前置シフト", t, u"ねし");
        }
        int n_layouts = 0;
        for (auto& de : fs::directory_iterator(dir)) {   // 同梱の配列が全部読めること
            if (de.path().extension() != ".tsv") continue;
            n_layouts++;
            std::ifstream in(de.path(), std::ios::binary);
            std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Layout x;
            std::string e;
            if (!x.load(t, &e)) { printf("FAIL layout %s: %s\n", de.path().filename().string().c_str(), e.c_str()); failures++; }
        }
        printf("     layouts: %d 個\n", n_layouts);
    }

    // ---- ローマ字
    {
        Romaji r;
        auto run = [&](const char* in) {
            u16 out;
            for (const char* p = in; *p; p++) out += r.process(char16_t(*p)).committed;
            out += r.flush();
            return out;
        };
        expect("romaji konnnichiha", run("konnnichiha"), u"こんにちは");
        expect("romaji kanji", run("kanji"), u"かんじ");
        expect("romaji n'a", run("kan'i"), u"かんい");
        expect("romaji tte", run("kitte"), u"きって");
        expect("romaji z/", run("z/"), u"・");
        expect("romaji ca", run("cocoa"), u"ここあ");
        expect("romaji end n", run("hon"), u"ほん");
        expect("to_romaji", r.to_romaji(u"とうきょうでしゅっぱつ"), u"toukyoudeshuppatsu");
        expect("to_romaji n", r.to_romaji(u"かんい"), u"kanni");
    }
    expect("halfwidth kana", to_halfwidth_kana(u"ガッコウ　パン"), u"ｶﾞｯｺｳ ﾊﾟﾝ");

    // ---- 日付・時刻 (2026-10-17 土曜 14:05 を固定して。HarmonyOS 版 DateTimePredictor と同じ決まり)
    {
        auto make_time = [](int y, int m, int d, int hh, int mi) {
            std::tm t{};
            t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = hh; t.tm_min = mi; t.tm_isdst = -1;
            return std::mktime(&t);
        };
        const std::time_t now = make_time(2026, 10, 17, 14, 5);
        auto pred = [&](const char* r) { return datetime_predict(from_utf8(r), now); };
        auto at = [](const std::vector<u16>& v, size_t i) { return i < v.size() ? v[i] : u16(); };

        auto ky = pred("きょう");
        expect("datetime きょう[0]", at(ky, 0), u"2026年10月17日");
        expect("datetime きょう[1]", at(ky, 1), u"2026年10月17日(土)");
        expect("datetime きょう[2]", at(ky, 2), u"10月17日");
        expect("datetime きょう[3]", at(ky, 3), u"10月17日(土)");
        expect("datetime きょう[4]", at(ky, 4), u"10月17日(土曜日)");
        expect("datetime きょう[5]", at(ky, 5), u"10/17");
        expect("datetime きょう[6]", at(ky, 6), u"10/17(土)");
        expect("datetime きょう[7]", at(ky, 7), u"2026/10/17");
        expect("datetime きょう[8]", at(ky, 8), u"2026-10-17");
        expect("datetime きょう[9]", at(ky, 9), u"令和8年10月17日");
        expect("datetime きょう[10]", at(ky, 10), u"令和8年10月17日(土)");
        expect("datetime きょう 8 桁", at(ky, 11), u"20261017");
        if (ky.size() != 12) { printf("FAIL datetime きょう count %zu\n", ky.size()); failures++; }   // 11 形 + 数字 8 桁

        expect("datetime あした", at(pred("あした"), 0), u"2026年10月18日");
        expect("datetime しあさって", at(pred("しあさって"), 0), u"2026年10月20日");
        expect("datetime さきおととい", at(pred("さきおととい"), 0), u"2026年10月14日");
        expect("datetime きのう", at(pred("きのう"), 0), u"2026年10月16日");

        // 曜日だけ (今日は土曜): 次のその曜日
        expect("datetime げつようび", at(pred("げつようび"), 0), u"2026年10月19日");
        expect("datetime かようび", at(pred("かようび"), 0), u"2026年10月20日");
        expect("datetime にちようび", at(pred("にちようび"), 0), u"2026年10月18日");
        expect("datetime げつよう (短縮)", at(pred("げつよう"), 0), u"2026年10月19日");

        // 週 + 曜日 (月曜はじまり)
        expect("datetime こんしゅうのげつようび", at(pred("こんしゅうのげつようび"), 0), u"2026年10月12日");
        expect("datetime らいしゅうのきんようび", at(pred("らいしゅうのきんようび"), 0), u"2026年10月23日");
        expect("datetime せんしゅうのすいようび", at(pred("せんしゅうのすいようび"), 0), u"2026年10月7日");
        expect("datetime さらいしゅうげつようび", at(pred("さらいしゅうげつようび"), 0), u"2026年10月26日");

        // ようび・なんようび -> 今日の曜日
        auto yb = pred("ようび");
        expect("datetime ようび", yb.empty() ? u16() : (yb[0] + u" " + yb[1] + u" " + yb[2]), u"土曜日 土 (土)");

        // 週の範囲 (月曜〜日曜)
        auto wk = pred("こんしゅう");
        expect("datetime こんしゅう", at(wk, 0), u"10月12日〜10月18日");
        expect("datetime こんしゅう 2", at(wk, 1), u"10/12〜10/18");
        expect("datetime らいしゅう", at(pred("らいしゅう"), 0), u"10月19日〜10月25日");
        expect("datetime せんしゅう", at(pred("せんしゅう"), 0), u"10月5日〜10月11日");

        // 月
        auto mo = pred("こんげつ");
        expect("datetime こんげつ", at(mo, 0), u"2026年10月");
        expect("datetime こんげつ 2", at(mo, 1), u"10月");
        expect("datetime らいげつ", at(pred("らいげつ"), 0), u"2026年11月");
        expect("datetime せんげつ", at(pred("せんげつ"), 0), u"2026年9月");

        // 日付に曜日をつける (<数>がつ<数>にち)
        auto md1 = pred("じゅうがつにじゅうにち");
        expect("datetime 10月20日", at(md1, 0), u"10月20日(火)");
        expect("datetime 10月20日 2", at(md1, 1), u"10月20日(火曜日)");
        expect("datetime 10月20日 3", at(md1, 2), u"10/20(火)");
        expect("datetime 10月20日 4", at(md1, 3), u"2026年10月20日(火)");
        auto md2 = pred("10がつ20にち");
        expect("datetime 10がつ20にち", at(md2, 0), u"10月20日(火)");
        if (!pred("にがつさんじゅうにち").empty()) { printf("FAIL datetime にがつさんじゅうにち\n"); failures++; }
        else printf("ok   datetime にがつさんじゅうにち\n");
        expect("datetime にがつにじゅうにち (60 日以上前は来年)", at(pred("にがつにじゅうにち"), 0), u"2月20日(土)");

        // 年
        expect("datetime 2026ねん", at(pred("2026ねん"), 0), u"令和8年");
        expect("datetime 20261018", at(pred("20261018"), 1), u"2026年10月18日(日)");
        {   // ありえない日は日付にしない (数字の書き換えだけ)
            const auto v = pred("20260230");
            bool date = false;
            for (auto& s : v) date = date || s.find(u'年') != u16::npos || s.find(u'/') != u16::npos;
            expect("datetime 20260230 (ありえない日)", u16(date ? u"あり" : u"なし"), u"なし");
        }
        expect("datetime れいわはちねん", at(pred("れいわはちねん"), 0), u"2026年");
        expect("datetime へいせいにねん", at(pred("へいせいにねん"), 0), u"1990年");
        expect("datetime しょうわごじゅうごねん", at(pred("しょうわごじゅうごねん"), 0), u"1980年");

        // いま (日付つきの時刻も)
        auto im = pred("いま");
        expect("datetime いま", at(im, 0), u"14:05");
        expect("datetime いま 2", at(im, 1), u"14時5分");
        expect("datetime いま 3", at(im, 2), u"午後2時5分");
        expect("datetime いま 4", at(im, 3), u"10月17日 14:05");
    }


    // ---- ショートカットキーの既定と移し替え (2026-10-16)
    {
        auto has = [](const std::vector<std::pair<std::string, std::string>>& v, const std::string& key, const std::string& func) {
            for (auto& p : v) if (p.first == key && p.second == func) return true;
            return false;
        };
        auto index_of = [](const std::vector<std::pair<std::string, std::string>>& v, const std::string& key, const std::string& func) {
            for (size_t i = 0; i < v.size(); i++) if (v[i].first == key && v[i].second == func) return int(i);
            return -1;
        };
        auto expect_shortcut = [&](const char* name, const std::vector<std::pair<std::string, std::string>>& v,
                                   const std::string& key, const std::string& func, bool want) {
            if (has(v, key, func) == want) { printf("ok   %s\n", name); return; }
            printf("FAIL %s: %s=%s\n", name, key.c_str(), func.c_str());
            failures++;
        };
        const auto msime = default_shortcuts("msime");
        expect_shortcut("shortcut msime Ctrl+U", msime, "Ctrl+U", "hiragana", true);
        expect_shortcut("shortcut msime Ctrl+M", msime, "Ctrl+M", "confirm", true);
        expect_shortcut("shortcut msime Ctrl+Space", msime, "Ctrl+Space", "ime_toggle", true);
        expect_shortcut("shortcut msime Henkan on", msime, "Henkan", "ime_on", true);
        expect_shortcut("shortcut msime Henkan reconvert", msime, "Henkan", "reconvert", true);
        expect_shortcut("shortcut msime Muhenkan off", msime, "Muhenkan", "ime_off", true);
        const int i_on = index_of(msime, "Henkan", "ime_on");
        const int i_rc = index_of(msime, "Henkan", "reconvert");
        if (i_on < 0 || i_rc < 0 || i_on >= i_rc) { printf("FAIL shortcut Henkan order\n"); failures++; }
        else printf("ok   shortcut Henkan order\n");
        const auto atok = default_shortcuts("atok");
        expect_shortcut("shortcut atok Ctrl+@", atok, "Ctrl+@", "han_alpha", true);
        expect_shortcut("shortcut atok Shift+Henkan", atok, "Shift+Henkan", "reconvert", true);
        const auto kotoeri = default_shortcuts("kotoeri");
        expect_shortcut("shortcut kotoeri Ctrl+I", kotoeri, "Ctrl+I", "seg_shrink", true);
        expect_shortcut("shortcut kotoeri Ctrl+Shift+R", kotoeri, "Ctrl+Shift+R", "reconvert", true);
        // 古い設定からの移し替え: オフにしていた機能は入れない (muhenkan_off=false なら Muhenkan が無い)
        const auto off = build_default_shortcuts("msime", "UIOPT", false, false, true);
        expect_shortcut("migrate no ctrl_space", off, "Ctrl+Space", "ime_toggle", false);
        expect_shortcut("migrate no muhenkan_off", off, "Muhenkan", "ime_off", false);
        expect_shortcut("migrate reconvert stays", off, "Henkan", "reconvert", true);
        // Ctrl + 字を変えていたらその字で (- はその F キーだけ割り当て無し)
        const auto custom = build_default_shortcuts("msime", "AB-C-", true, true, true);
        expect_shortcut("migrate custom ctrl", custom, "Ctrl+A", "hiragana", true);
        expect_shortcut("migrate custom pos C", custom, "Ctrl+C", "zen_alpha", true);
        bool any_halfkana = false;
        for (auto& p : custom) if (p.second == "halfkana") any_halfkana = true;
        if (!any_halfkana) printf("ok   migrate skip - (no halfkana)\n");
        else { printf("FAIL migrate skip -\n"); failures++; }
        printf("     shortcuts: %d\n", int(msime.size()));
    }

    auto lex = slurp(argv[1]);
    auto model = slurp(argv[2]);
    Converter conv;
    const char* th = getenv("SHUNTI_THREADS");
    auto t_open = std::chrono::steady_clock::now();
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), th ? atoi(th) : 1)) { printf("engine open failed\n"); return 2; }
    printf("     engine open %.1f ms (threads %d)\n",
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_open).count(), th ? atoi(th) : 1);
    {   // AI学習 (2026-11-05): 確定した文で育て、保存し、別の Converter が読み直す。消したら初めの値に戻る
        const std::filesystem::path d = work / "ai";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        Converter ai;
        if (!ai.open(lex.data(), lex.size(), model.data(), model.size(), 1)) { printf("FAIL ai open\n"); failures++; }
        auto top = [](Converter& c, const u16& r) { auto v = c.convert(u"", r, 5).cands; return v.empty() ? u16() : v[0]; };
        const u16 base = top(ai, u"きしゃ");
        ai.ai_learn(u"", u"きしゃ", u"汽車");   // オフの間は何もしない
        if (std::filesystem::exists(d / "ai_learn.bin")) { printf("FAIL ai off writes\n"); failures++; }
        ai.set_ai_learn(d / "ai_learn.bin");
        // 1 位を覆すのは従来の学習の役目。AI学習は少しずつ寄せる (損失 = 確定した表記の経路の -log 確率) ので、下がるかを見る
        float l0 = -1, l1 = -1;
        for (int i = 0; i < 30; i++) { l1 = ai.ai_learn(u"", u"きしゃ", u"汽車"); if (i == 0) l0 = l1; }
        const auto sz = std::filesystem::file_size(ai.ai_file(), ec);
        // 保存先はモデルごと (ai_learn_<モデルの印>.bin。2026-11-06)
        if (ai.ai_file().filename().string().rfind("ai_learn_", 0) == 0 && !std::filesystem::exists(d / "ai_learn.bin")) printf("ok   ai per-model file: %s\n", ai.ai_file().filename().string().c_str());
        else { printf("FAIL ai per-model file: %s\n", ai.ai_file().string().c_str()); failures++; }
        if (ec || sz < 8) { printf("FAIL ai save\n"); failures++; }
        if (l0 > 0 && l1 >= 0 && l1 < l0 - 0.3f) printf("ok   ai learn: loss %.3f -> %.3f (%d bytes, top %s)\n", l0, l1, int(sz), to_utf8(top(ai, u"きしゃ")).c_str());
        else { printf("FAIL ai learn: loss %.3f -> %.3f\n", l0, l1); failures++; }
        Converter other;   // 別の窓 (別のプロセス) の代わり: 保存した続きから
        other.open(lex.data(), lex.size(), model.data(), model.size(), 1);
        other.set_ai_learn(d / "ai_learn.bin");
        const float lr = other.ai_learn(u"", u"きしゃ", u"汽車");
        if (lr >= 0 && lr < l0 - 0.3f) printf("ok   ai reload: loss %.3f\n", lr);
        else { printf("FAIL ai reload: loss %.3f\n", lr); failures++; }
        forget_all(d);   // 学習を全部消しても AI学習の重みは残る (消すのは AI学習を外すときだけ)
        if (other.ai_learn(u"", u"きしゃ", u"汽車") < 0 || !std::filesystem::exists(other.ai_file())) { printf("FAIL ai kept by forget_all\n"); failures++; }
        forget_ai_learn(d);   // 外すとき: 消したら初めの値に戻る
        const float lf = other.ai_learn(u"", u"きしゃ", u"汽車");
        if (std::fabs(lf - l0) < 1e-3f) printf("ok   ai forget: loss %.3f\n", lf);
        else { printf("FAIL ai forget: loss %.3f (first %.3f)\n", lf, l0); failures++; }
        expect("ai forget top", top(ai, u"きしゃ"), base);
        other.set_ai_learn({});
        ai.set_ai_learn({});
        // 確定した文の記録があるところでオンにしたら、裏でまとめて学ぶ (2026-11-05)。2 回目は記録が変わっていないので学ばない
        forget_all(d);
        forget_ai_learn(d);
        {
            std::ofstream log(d / RECENT_FILE, std::ios::binary);
            const long long t0 = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 1000;
            for (int i = 0; i < 30; i++) log << (t0 + i) << "\tC\t" << to_utf8(u"きしゃ") << "\t" << to_utf8(u"汽車") << "\t\n";
            log << (t0 + 40) << "\tL\t" << to_utf8(u"かわ") << "\t" << to_utf8(u"川") << "\t0\t0\n";   // 確定の記録でない行は飛ばす
        }
        Converter bulk;
        bulk.open(lex.data(), lex.size(), model.data(), model.size(), 1);
        bulk.set_ai_learn(d / "ai_learn.bin");
        const bool was_busy = bulk.ai_busy();
        bulk.ai_wait();
        if (was_busy && bulk.ai_bulk_learned() == 30 && std::filesystem::exists(bulk.ai_file()) && !std::filesystem::exists(d / "ai_learn.lock"))
            printf("ok   ai bulk: 30\n");
        else { printf("FAIL ai bulk: busy %d learned %d\n", int(was_busy), bulk.ai_bulk_learned()); failures++; }
        const float lb = bulk.ai_learn(u"", u"きしゃ", u"汽車");
        if (lb >= 0 && lb < l0 - 0.3f) printf("ok   ai bulk loss: %.3f\n", lb);
        else { printf("FAIL ai bulk loss: %.3f\n", lb); failures++; }
        Converter again;
        again.open(lex.data(), lex.size(), model.data(), model.size(), 1);
        again.set_ai_learn(d / "ai_learn.bin");
        if (again.ai_busy()) { again.ai_wait(); printf("FAIL ai bulk again\n"); failures++; }
        else printf("ok   ai bulk again: skipped\n");
        again.set_ai_learn({});
        // 前の 1 つだけの保存 (ai_learn.bin) は、今のモデルの名前に移して引き継ぐ。リセットはどのモデルの分も消す
        const std::filesystem::path per = bulk.ai_file();
        bulk.set_ai_learn({});
        std::filesystem::rename(per, d / "ai_learn.bin", ec);
        std::ofstream(d / "ai_learn_00000000.bin", std::ios::binary) << "x";   // ほかのモデルの分のつもり
        Converter old;
        old.open(lex.data(), lex.size(), model.data(), model.size(), 1);
        old.set_ai_learn(d / "ai_learn.bin");
        old.ai_wait();
        const float lo = old.ai_learn(u"", u"きしゃ", u"汽車");
        if (std::filesystem::exists(per) && !std::filesystem::exists(d / "ai_learn.bin") && lo >= 0 && lo < l0 - 0.3f) printf("ok   ai adopt old file: loss %.3f\n", lo);
        else { printf("FAIL ai adopt old file: loss %.3f\n", lo); failures++; }
        old.set_ai_learn({});
        forget_ai_learn(d);
        if (!std::filesystem::exists(per) && !std::filesystem::exists(d / "ai_learn_00000000.bin")) printf("ok   ai forget all models\n");
        else { printf("FAIL ai forget all models\n"); failures++; }
    }
    {   // 数字だけの読みの書き換え (2026-10-30)
        auto at = [](const std::vector<u16>& v, size_t i) { return i < v.size() ? v[i] : u16(); };
        auto has = [](const std::vector<u16>& v, const u16& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
        expect("number 2026 era", at(number_forms(u"2026"), 0), u"令和8年");
        if (!has(number_forms(u"2026"), u"二千二十六") || !has(number_forms(u"2026"), u"2,026")) { printf("FAIL number 2026\n"); failures++; }
        if (!has(number_forms(u"20"), u"二十") || !has(number_forms(u"20"), u"⑳") || !has(number_forms(u"20"), u"⓴")) { printf("FAIL number 20\n"); failures++; }
        if (!has(number_forms(u"50"), u"㊿") || !has(number_forms(u"21"), u"㉑") || !has(number_forms(u"36"), u"㊱") || !has(number_forms(u"0"), u"⓪")) { printf("FAIL number circled\n"); failures++; }
        if (!has(number_forms(u"12345678"), u"1234万5678") || !has(number_forms(u"1000000"), u"100万") || !has(number_forms(u"1000000"), u"百万")) { printf("FAIL number big\n"); failures++; }
        if (!number_forms(u"01").empty() || !number_forms(u"3a").empty() || !number_forms(u"123456789012345678901").empty()) { printf("FAIL number reject\n"); failures++; }
    }
    {   // 時間を指定して学習を戻す: 控えの時刻より新しい分だけ、元の値へ (2026-10-30)
        const std::filesystem::path d = work / "forget";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        {
            Learning l(d / "learned.json");
            StyleLog st(d / "style.json");
            l.record(u"かわ", u"川");
            l.record(u"かわ", u"川");
            std::filesystem::rename(d / RECENT_FILE, d / "keep.tsv", ec);
            std::ofstream old(d / RECENT_FILE, std::ios::binary);   // 2 回目までは 1 時間より前のこと
            old << "1000\tL\t" << to_utf8(u"かわ") << "\t" << to_utf8(u"川") << "\t0\t0\n";
            old.close();
            l.record(u"かわ", u"川");
            l.record(u"やま", u"山");
            st.record(u"山", {1.f, 0.f});
            const int n = forget_recent(d, 2000);
            if (n != 3) { printf("FAIL forget_recent count %d\n", n); failures++; }
            int kawa = 0;
            for (auto& c : l.counts(u"かわ")) if (c.first == u"川") kawa = c.second;
            if (kawa != 4 || !l.counts(u"やま").empty() || !st.counts().empty()) { printf("FAIL forget_recent restore %d\n", kawa); failures++; }
            l.hide(u"かわ", u"皮");
            if (!l.hidden(u"かわ", u"皮")) { printf("FAIL hide\n"); failures++; }
            forget_recent(d, 2000);
            if (l.hidden(u"かわ", u"皮")) { printf("FAIL forget_recent hide\n"); failures++; }
            auto u = l.record(u"かわ", u"川");
            l.restore({u});
            kawa = 0;
            for (auto& c : l.counts(u"かわ")) if (c.first == u"川") kawa = c.second;
            if (kawa != 4) { printf("FAIL learning restore %d\n", kawa); failures++; }
            // 確定した文: 同意したときだけ書く・種類を指定して消せる・時間の指定でも消える
            l.note_commit(u"かわ", u"川", u"前の\t文\n");
            std::string rec;
            read_file(d / RECENT_FILE, rec);
            if (rec.find("\tC\t") == std::string::npos || rec.find(to_utf8(u"前の 文 ")) == std::string::npos) { printf("FAIL note_commit\n"); failures++; }
            recent_drop(d, "C");
            read_file(d / RECENT_FILE, rec);
            if (rec.find("\tC\t") != std::string::npos) { printf("FAIL recent_drop\n"); failures++; }
            l.note_commit(u"かわ", u"川", u"");
            forget_recent(d, 2000, "LHSF");   // 変換の学習を消しても確定した文は残る
            read_file(d / RECENT_FILE, rec);
            if (rec.find("\tC\t") == std::string::npos) { printf("FAIL forget_recent kinds\n"); failures++; }
            forget_recent(d, 2000, "C");
            read_file(d / RECENT_FILE, rec);
            if (rec.find("\tC\t") != std::string::npos) { printf("FAIL forget_recent commit\n"); failures++; }
            // 記録だけ消す: 学習は残り、控えの行だけ無くなる
            l.note_commit(u"かわ", u"川", u"");
            const int cut = recent_cut(d, 0);
            read_file(d / RECENT_FILE, rec);
            if (cut < 1 || rec.find("\tC\t") != std::string::npos || l.counts(u"かわ").empty()) { printf("FAIL recent_cut %d\n", cut); failures++; }
            forget_all(d);
            if (!l.counts(u"かわ").empty() || std::filesystem::exists(d / RECENT_FILE)) { printf("FAIL forget_all\n"); failures++; }
            else printf("ok   forget recent learning\n");
        }
    }
    {   // 学習は読み 500 個を超えても消えない (以前は 500 で押し出されていた)・最後に選んだ日を覚えて、読み直しても残る
        Learning big(work / "learned_big.json");
        big.clear();
        for (int k = 0; k < 2; k++)   // top は 2 回以上選んだ表記だけ
            for (int i = 0; i < 1200; i++) big.record(u"よみ" + from_utf8(std::to_string(i)), u"読" + from_utf8(std::to_string(i)));
        Learning again(work / "learned_big.json");
        const bool kept = again.top(u"よみ0") == u"読0" && again.top(u"よみ1199") == u"読1199";
        const bool day = again.last_day(u"よみ0") == Learning::today();
        if (kept && day) printf("ok   learning keeps 1200 readings and the last day\n");
        else { printf("FAIL learning: kept %d day %d\n", int(kept), int(day)); failures++; }
        big.clear();
        // 消した表記は学習の枠を食わない・6 つ目の表記はいちばん少ないものと入れ替える
        for (int i = 0; i < 4; i++) big.hide(u"かい", u"消" + from_utf8(std::to_string(i)));
        for (int i = 0; i < 6; i++) big.record(u"かい", u"表" + from_utf8(std::to_string(i)));
        big.record(u"かい", u"表5");
        if (big.apply_order(u"かい", {u"表1", u"表5"})[0] == u"表5" && big.counts(u"かい").size() == 9) printf("ok   learning: hidden words do not take slots, 6th word replaces\n");
        else { printf("FAIL learning slots: top [%s]\n", u8(big.top(u"かい")).c_str()); failures++; }
        big.clear();
        // 壊れたファイルは別名で残す (空で上書きしない)
        {
            std::ofstream(work / "learned_broken.json") << "{ broken";
            Learning br(work / "learned_broken.json");
            br.record(u"てすと", u"テスト");
            bool kept = false;
            for (auto& f : std::filesystem::directory_iterator(work))
                kept |= f.path().filename().string().rfind("learned_broken.json.broken-", 0) == 0;
            if (kept) printf("ok   learning: broken file kept aside\n");
            else { printf("FAIL learning: broken file not kept\n"); failures++; }
            for (auto& f : std::filesystem::directory_iterator(work))
                if (f.path().filename().string().rfind("learned_broken.json", 0) == 0) std::filesystem::remove(f.path());
        }
    }
    Learning learning(work / "learned.json");
    UserDict dict(work / "userdict.json");
    Composer c(&conv, &learning, &dict);
    c.options.live_commit = false;   // リアルタイム確定は下でまとめて試す

    // ---- 語の区切りと品詞 (文節の分け方を見る)
    for (const char16_t* r : {u"きょうはいいてんきですね", u"わたしはがくせいです", u"たべさせられなかった"}) {
        Conversion cv = conv.convert(u"", r, 3);
        printf("     words:");
        for (auto& w : cv.words) printf(" %s/%s(%d:%c,%d:%c)", u8(w.reading).c_str(), u8(w.surface).c_str(), w.lid,
                                        POS_CLASSES[w.lid], w.rid, POS_CLASSES[w.rid]);
        printf("\n");
    }

    // ---- 1 打鍵ごとの重さ (打っている間の変換と予測)
    {
        auto ms_of = [](auto f) {
            auto t = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; i++) f();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() / 20;
        };
        printf("     convert 8 kana %.1f ms, 16 kana %.1f ms, complete %.1f ms\n",
               ms_of([&] { conv.convert(u"今日は", u"いいてんきです", 10); }),
               ms_of([&] { conv.convert(u"今日は", u"いいてんきですねさんぽにいき", 10); }),
               ms_of([&] { conv.complete(u"いいてんき", 4); }));
    }

    // ---- 入力中の見せ方
    keys(c, "kyouha");
    expect("input text", c.view().text, u"きょうは");
    expect("typed strip kana", c.view().typed, u"きょうは");
    keys(c, "k");
    expect("pending romaji", c.view().text, u"きょうはk");
    expect("typed strip pending", c.view().typed, u"きょうはk");
    keys(c, "{bs}{esc}");
    expect("esc clears", c.view().text, u"");

    // ---- 変換と文節
    auto t0 = std::chrono::steady_clock::now();
    keys(c, "kyouhaiitenkidesune{sp}");
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("     convert took %.1f ms\n", ms);
    expect("convert", c.view().text, u"今日はいい天気ですね");
    printf("     phrases: %s\n", u8(segs_of(c.view())).c_str());
    keys(c, "{enter}");
    expect("commit", c.take_commit(), u"今日はいい天気ですね");

    keys(c, "watashihagakuseidesu{sp}");
    printf("     phrases: %s\n", u8(segs_of(c.view())).c_str());
    keys(c, "{right}{sp}");
    printf("     2nd phrase cands:");
    for (auto& x : c.view().cands) printf(" %s", u8(x).c_str());
    printf("\n");
    keys(c, "{esc}");
    expect("back to input", c.view().text, u"わたしはがくせいです");
    keys(c, "{esc}");

    // Shift+← で文節を縮める
    keys(c, "kaisyanidenwasuru{sp}");
    printf("     phrases: %s\n", u8(segs_of(c.view())).c_str());
    keys(c, "{S-left}");
    printf("     after S-left: %s\n", u8(segs_of(c.view())).c_str());
    keys(c, "{esc}{esc}");

    // ---- F6〜F10
    keys(c, "toukyou{f7}");
    expect("F7", c.view().text, u"トウキョウ");
    keys(c, "{f10}");
    expect("F10", c.view().text, u"toukyou");
    keys(c, "{f10}");
    expect("F10 again", c.view().text, u"TOUKYOU");
    keys(c, "{f10}");
    expect("F10 3rd", c.view().text, u"Toukyou");
    keys(c, "{f9}");
    expect("F9", c.view().text, u"ｔｏｕｋｙｏｕ");
    keys(c, "{f8}");
    expect("F8", c.view().text, u"ﾄｳｷｮｳ");
    keys(c, "{esc}{esc}");

    // ---- 無変換キーは押すたびに カタカナ → 半角カタカナ → ひらがな
    keys(c, "toukyou{muhenkan}");
    expect("muhenkan 1", c.view().text, u"トウキョウ");
    keys(c, "{muhenkan}");
    expect("muhenkan 2", c.view().text, u"ﾄｳｷｮｳ");
    keys(c, "{muhenkan}");
    expect("muhenkan 3", c.view().text, u"とうきょう");
    keys(c, "{esc}{esc}");

    // ---- 打っている間の候補と、↓ で選んで確定
    keys(c, "konnnichiha");
    printf("     live cands:");
    for (auto& x : c.view().cands) printf(" %s", u8(x).c_str());
    printf("\n");
    if (!c.view().cand_open) { printf("FAIL live cands not shown\n"); failures++; }
    keys(c, "{down}{enter}");
    expect("live select", c.take_commit(), u"こんにちは");

    // ---- 打っている間に気分の絵文字の提案は出さない (確定の後の予測で出す)
    for (const char* w : {"soresika", "hannkyouteii", "kyouhatukareta"}) {
        keys(c, w);
        printf("     mood %s:", w);
        bool mood = false;
        for (size_t i = 0; i < c.view().sugg.size(); i++) {
            printf(" [%s]", u8(c.view().sugg[i]).c_str());
            mood = mood || c.view().sugg_kinds[i] == 6;
        }
        printf("\n");
        if (mood) { printf("FAIL mood %s\n", w); failures++; }
        keys(c, "{esc}{esc}");
        c.take_commit();
    }

    // ---- 明らかな打ち間違いは直した形が 1 位 (よろしくおねぎあします)
    keys(c, "yorosikuonegiasimasu{sp}{enter}");
    expect("typo promoted", c.take_commit(), u"よろしくお願いします");
    {   // ライブ変換で直したときは、直した所に印 (SpanKind::Corrected)
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        s.options.live_display = true;
        keys(s, "yorosikuonegiasimasu");
        u16 marked;
        for (auto& sp : s.view().spans)
            if (sp.kind == SpanKind::Corrected) marked = s.view().text.substr(size_t(sp.start), size_t(sp.len));
        printf("     live: %s  corrected: [%s]\n", u8(s.view().text).c_str(), u8(marked).c_str());
        if (marked.empty()) { printf("FAIL live typo correction not marked\n"); failures++; }
        keys(s, "{esc}");
    }

    // ---- 提案 (日付など): 候補と交互に ✦ を付けて混ぜる (2 位の後ろから)。Shift+Enter で 1 つ目を確定
    keys(c, "kyou");
    u16 first_sugg;
    printf("     cands:");
    for (auto& x : c.view().cands) {
        printf(" [%s]", u8(x).c_str());
        (void)x;
    }
    printf("\n");
    for (size_t i = 0; i < c.view().cand_notes.size(); i++) if (first_sugg.empty() && c.view().cand_notes[i] == u"日付") first_sugg = c.view().cands[i];
    if (first_sugg.empty() || c.view().cands.size() < 3 || c.view().cands[2] != first_sugg) {
        printf("FAIL no suggestion after top for kyou\n");
        failures++;
    }
    keys(c, "{S-enter}");
    expect("suggestion commit", c.take_commit(), first_sugg);

    // ---- ↓ で候補に入って、3 つ目 (提案) を Enter で確定
    keys(c, "kyou{down}{down}{down}{enter}");
    expect("suggestion via cands", c.take_commit(), first_sugg);

    // ---- 候補の続き: 最後のページまで進むと読み足す (30 件で切れない)
    keys(c, "kou{sp}");
    for (int i = 0; i < 6; i++) keys(c, "{pgdn}");
    printf("     kou cands after paging: %d\n", int(c.view().cands.size()));
    if (c.view().cands.size() <= 31) { printf("FAIL no more candidates loaded\n"); failures++; }
    keys(c, "{esc}{esc}");

    // ---- 変換したらすぐ候補の窓が出る
    keys(c, "kawa{sp}");
    if (!c.view().cand_open) { printf("FAIL cands not open after convert\n"); failures++; }
    keys(c, "{esc}{esc}");

    // ---- 長い入力の前の方の固定 (保留): 入力欄には確定せず、変換した形で入力中の頭に見せる。空白で頭の文節になる
    c.options.live_commit = true;
    c.set_keep(8);   // 速さを測らずに、スマホ版と同じ 8 字で
    keys(c, "kyouhaiitenkidesunesanponiikitaikedoamegafurisoudesu");
    u16 held = c.take_commit();
    printf("     hold: committed [%s] composing [%s]\n", u8(held).c_str(), u8(c.view().text).c_str());
    if (!held.empty()) { printf("FAIL hold committed to the field\n"); failures++; }
    bool conv_head = !c.view().spans.empty() && c.view().spans[0].kind == SpanKind::Converted;
    if (!conv_head) { printf("FAIL hold head not shown converted\n"); failures++; }
    keys(c, "{sp}");
    printf("     hold -> segments: [%s] (%d spans)\n", u8(c.view().text).c_str(), int(c.view().spans.size()));
    if (c.view().spans.size() < 3) { printf("FAIL hold segments too few\n"); failures++; }
    keys(c, "{enter}");
    u16 all = c.take_commit();
    printf("     hold commit: %s\n", u8(all).c_str());
    if (all.size() < 15) { printf("FAIL hold commit lost the head\n"); failures++; }
    expect("hold commit keeps the whole reading", c.undoable().second, u"きょうはいいてんきですねさんぽにいきたいけどあめがふりそうです");
    // 読みを消し切って固定した文だけになっても、空白で文を失わない
    keys(c, "kyouhaiitenkidesunesanponiikitaikedoamegafurisoudesu{bs}{bs}{bs}{bs}{bs}{bs}{bs}{bs}{sp}");
    if (c.view().text.empty()) { printf("FAIL space with only the held part\n"); failures++; }
    keys(c, "{esc}{esc}{esc}");
    c.take_commit();
    // 固定した所を Backspace で消すと読みに戻る
    keys(c, "kyouhaiitenkidesunesanponiikitaikedoamegafurisoudesu{home}{end}");
    u16 v1 = c.view().text;
    keys(c, "{esc}");
    if (!c.take_commit().empty()) { printf("FAIL esc after hold committed\n"); failures++; }
    printf("     hold home/end: [%s]\n", u8(v1).c_str());

    // ---- ライブ変換: 入力中の文字を、空白を押す前から変換して見せ、Enter でそのまま確定する
    c.options.live_commit = false;
    c.options.live_display = true;
    keys(c, "kyouhaiitenki");
    printf("     live display: [%s]\n", u8(c.view().text).c_str());
    if (c.view().text == u"きょうはいいてんき") { printf("FAIL live display shows kana\n"); failures++; }
    u16 shown = c.view().text;
    keys(c, "{enter}");
    expect("live display enter commits what is shown", c.take_commit(), shown);
    {   // 句読点を打っても、その前の見えていた変換は変わらない (後ろを打ち続けても)
        int changed = 0;
        for (const char* s : {"kishaninoru", "sikounohansyou", "syuusyokunoki", "kyouhaameda", "kanojohakaetta"}) {
            keys(c, s);
            const u16 before = c.view().text;
            keys(c, ",soreha.");
            const u16 after = c.view().text;
            if (after.compare(0, before.size(), before) != 0) { printf("     punct changed: [%s] -> [%s]\n", u8(before).c_str(), u8(after).c_str()); changed++; }
            keys(c, "{esc}{esc}{esc}");
            c.take_commit();
        }
        if (changed) { printf("FAIL punct changed the live text %d\n", changed); failures++; }
    }
    keys(c, "yorosikuone{down}{enter}");   // 決まり文句は読みが打った字と違うので、打った読みでは覚えない
    expect("phrase committed", c.take_commit(), u"よろしくお願いします");
    if (!learning.counts(u"よろしくおね").empty()) { printf("FAIL phrase learned under the typed reading\n"); failures++; }
    keys(c, "kyouhaiiten{left}{enter}");   // 読みの途中にカーソルを動かしたら、かなで見せてかなで確定
    expect("live display falls back to kana mid-reading", c.take_commit(), u"きょうはいいてん");
    keys(c, "kyouhaiitenki");   // 英語入力に切り替えた (finish): 見えている変換のまま確定する
    shown = c.view().text;
    c.finish();
    expect("live display finish commits what is shown", c.take_commit(), shown);
    c.options.live_wait = true;   // 遅延: 打っている間は変換せず、手が止まったら変換する
    keys(c, "kyouhaiitenki");
    expect("live wait shows kana until flush", c.view().text, u"きょうはいいてんき");
    if (!c.live_waiting() || !c.flush_live()) { printf("FAIL live wait flush\n"); failures++; }
    shown = c.view().text;
    if (shown == u"きょうはいいてんき") { printf("FAIL live wait flush shows kana\n"); failures++; }
    keys(c, "desu");
    expect("live wait keeps the converted part", c.view().text, shown + u"です");
    keys(c, "{enter}");
    u16 waited = c.take_commit();
    if (waited == shown + u"です" || waited.find(u"きょう") != u16::npos) printf("     live wait enter: [%s]\n", u8(waited).c_str());
    if (c.live_waiting()) { printf("FAIL live wait pending after enter\n"); failures++; }
    keys(c, "kyouhaii{bs}");
    if (c.live_waiting()) { printf("FAIL live wait pending after backspace\n"); failures++; }
    keys(c, "{esc}");
    c.take_commit();
    c.options.live_wait = false;
    {   // かなのままの表記も学習する: 選んだ回数が多ければ漢字の学習に勝つ
        std::filesystem::remove(work / "learned_kana.json");   // 前回の試験の回数を持ち越さない
        Learning l(work / "learned_kana.json");
        l.record(u"した", u"下");
        expect("kana learn: kanji first", l.top(u"した"), u"下");
        auto has_mark = [&] {
            for (auto& m : l.marks()) if (m.first == u"下") return true;
            return false;
        };
        if (!has_mark()) { printf("FAIL kana learn: mark missing\n"); failures++; }
        l.record(u"した", u"した");
        if (has_mark()) { printf("FAIL kana learn: mark stays at a tie\n"); failures++; }
        l.record(u"した", u"した");
        expect("kana learn: kana wins", l.top(u"した"), u"した");
        l.record(u"ため", u"溜め");
        l.record(u"はし", u"橋");
        l.record(u"はし", u"はし");
        l.record(u"はし", u"はし");
        l.record(u"はし", u"はし");
        l.record(u"あいうえおかき", u"亜");
        int lifted = l.release_kana([](const u16& r) { return r != u"みず"; });
        if (lifted != 1) { printf("FAIL lift kana count %d\n", lifted); failures++; }
        expect("release kana: AI decides", l.top(u"ため"), u16());
        expect("release kana: long reading stays", l.top(u"あいうえおかき"), u"亜");
        l.record(u"ため", u"溜め");
        expect("release kana: one pick restores", l.top(u"ため"), u"溜め");
        std::filesystem::remove(work / "learned_live.json");
        Learning ll(work / "learned_live.json");
        ll.record(u"した", u"下");
        Composer lc(&conv, &ll, &dict);
        lc.options.live_display = true;
        keys(lc, "benkyouwoshita{enter}");
        lc.take_commit();
        int votes = 0;
        for (auto& x : ll.counts(u"した")) if (x.first == u"した") votes = x.second;
        if (votes != 1) { printf("FAIL live vote in a sentence %d\n", votes); failures++; }
        // ライブ変換の 1 位にも語の置き換えが効く。文脈が同じときだけ
        std::filesystem::remove(work / "learned_lc.json");
        Learning lw(work / "learned_lc.json");
        lw.record(u"した", u"下", 2, u"勉強を");
        Composer lw_c(&conv, &lw, &dict);
        lw_c.options.live_display = true;
        keys(lw_c, "benkyouwoshita{enter}");
        expect("live: learned word in the same context", lw_c.take_commit(), u"勉強を下");
        lw_c.set_context_provider([] { return u16(); });
        keys(lw_c, "sakanawoshita{enter}");
        const u16 other = lw_c.take_commit();
        if (other.find(u"下") != u16::npos) { printf("FAIL live: other context changed %s\n", u8(other).c_str()); failures++; }
    }
    {   // 回数の単位の印: 古い版が読みの項目として書き戻しても 2 倍にならない
        auto p = work / "learned_w.json";
        auto written = [&](const char* text) { std::ofstream(p, std::ios::binary | std::ios::trunc) << text; };
        auto count = [&]() {
            Learning a(p);
            auto v = a.counts(u"あ");
            return v.empty() ? 0 : v[0].second;
        };
        written(u8R"({"あ":{"亜":4},"@w":{"@w":2}})");
        if (count() != 4) { printf("FAIL weight marker as entry\n"); failures++; }
        written(u8R"({"@w":2,"あ":{"亜":4}})");
        if (count() != 4) { printf("FAIL weight marker as number\n"); failures++; }
        written(u8R"({"あ":{"亜":4}})");
        if (count() != 8) { printf("FAIL old counts not doubled\n"); failures++; }
        {
            Learning a(p);
            a.record(u"あ", u"亜");
        }
        if (count() != 10) { printf("FAIL weight marker after save\n"); failures++; }
    }
    {   // 文の流れ: お疲れ様です -> 。 -> 本日の進捗報告です。 (2 回あった流れ)
        SentenceFlow f;
        for (int i = 0; i < 2; i++) { f.feed(u"お疲れ様です。本日の進捗報告です。"); f.boundary(); }
        f.feed(u"お疲れ様です");
        auto p = f.predict();
        expect("flow: punct after clause", p.empty() ? u16() : p[0], u"。");
        c.flow = &f;
        keys(c, ".{enter}");
        c.take_commit();
        const auto& post = c.view().post;
        expect("flow: next clause after punct", post.empty() ? u16() : post[0], u"本日の進捗報告です。");
        keys(c, "{tab}");
        expect("flow: tab inserts it", c.take_commit(), u"本日の進捗報告です。");
        c.flow = nullptr;
    }
    {   // 。。。 は三点リーダーの形も候補に
        keys(c, "soredesu...{sp}");
        const auto& cs = c.view().cands;
        bool ell = false;
        for (const auto& x : cs) ell = ell || x.find(u"…") != u16::npos;
        printf("     ellipsis cands:");
        for (size_t i = 0; i < cs.size() && i < 4; i++) printf(" [%s]", u8(cs[i]).c_str());
        printf("\n");
        if (!ell) { printf("FAIL no ellipsis candidate\n"); failures++; }
        keys(c, "{esc}{esc}");
        c.take_commit();
    }
    for (auto [k, want] : {std::pair{",", u"，"}, std::pair{".", u"．"}, std::pair{"/", u"／"}, std::pair{"\\", u"\\"}, std::pair{"!", u"!"},
                                   std::pair{"~", u"〜"}, std::pair{"*", u"×"},std::pair{"(", u"「"}}) {   // 、。・ は仲間の記号も候補に
        keys(c, std::string(k) + "{sp}");
        const auto& cs = c.view().cands;
        printf("     mark cands %s:", k);
        for (size_t i = 0; i < cs.size() && i < 4; i++) printf(" [%s]", u8(cs[i]).c_str());
        printf("\n");
        if (std::find(cs.begin(), cs.end(), u16(want)) == cs.end()) { printf("FAIL no mark candidate %s\n", k); failures++; }
        keys(c, "{esc}{esc}");
        c.take_commit();
    }
    c.options.live_display = false;

    // ---- 打ち間違いを消して子音だけが残ったとき、続けて打った字とつながる
    keys(c, "ps{bs}a{enter}");
    expect("typo p+a", c.take_commit(), u"ぱ");
    keys(c, "kx{bs}ya{enter}");
    expect("typo k+ya", c.take_commit(), u"きゃ");
    keys(c, "kitx{bs}te{enter}");
    expect("typo t+te", c.take_commit(), u"きって");
    keys(c, "tq{bs}su{enter}");
    expect("typo t+su", c.take_commit(), u"つ");
    // ---- 打ちかけが無ければ Backspace はかなを 1 字消す (か を消して k が残らない)
    keys(c, "kaki{bs}{enter}");
    expect("bs kana", c.take_commit(), u"か");
    keys(c, "kanji{bs}{enter}");
    expect("bs after n", c.take_commit(), u"かん");
    keys(c, "itta{bs}{enter}");
    expect("bs after sokuon", c.take_commit(), u"いっ");
    keys(c, "kanji{bs}ji{enter}");
    expect("bs then type", c.take_commit(), u"かんじ");
    keys(c, "ka{bs}ki{enter}");
    expect("bs to empty", c.take_commit(), u"き");

    // ---- 英字・数字・記号
    keys(c, "Google{enter}");
    expect("uppercase run", c.take_commit(), u"Google");
    keys(c, "03-1234{enter}");
    expect("digits", c.take_commit(), u"03-1234");
    keys(c, "hai,sou.{enter}");
    expect("punct", c.take_commit(), u"はい、そう。");
    keys(c, "{sp}");
    expect("idle space", c.take_commit(), u"　");

    // ---- 変換中に次を打つと確定してから
    {   // 確定した文は既定では残さない
        std::error_code ec;
        std::filesystem::remove(work / RECENT_FILE, ec);
        keys(c, "neko{sp}{enter}");
        c.take_commit();
        std::string rec;
        read_file(work / RECENT_FILE, rec);
        if (rec.find("\tC\t") != std::string::npos) { printf("FAIL learn_log default\n"); failures++; }
        c.options.learn_log = true;
        keys(c, "neko{sp}{enter}");
        c.take_commit();
        read_file(work / RECENT_FILE, rec);
        if (rec.find("\tC\t") == std::string::npos) { printf("FAIL learn_log on\n"); failures++; }
        c.options.secure_field = true;
        const size_t was = rec.size();
        keys(c, "neko{sp}{enter}");
        c.take_commit();
        read_file(work / RECENT_FILE, rec);
        if (rec.size() != was) { printf("FAIL learn_log secure\n"); failures++; }
        c.options.learn_log = false;
        c.options.secure_field = false;
    }
    keys(c, "neko{sp}ga");
    expect("type after convert", c.take_commit(), u"猫");
    expect("new input", c.view().text, u"が");
    keys(c, "{esc}");

    // ---- 学習: 選び直した文節を覚える (前の文の文脈で 1 位が変わらないよう、毎回文脈を消す)
    c.reset_history();
    keys(c, "kawa{sp}");
    u16 first = c.view().text;
    keys(c, "{sp}");
    u16 second = c.view().text;
    keys(c, "{enter}");
    c.take_commit();
    // 手で 1 度選び直すと 2 回分で先頭へ (2026-10-30)
    c.reset_history();
    keys(c, "kawa{sp}");
    expect("learned once: chosen one goes first", c.view().text, second);
    printf("     (first was %s)\n", u8(first).c_str());
    keys(c, "{esc}{esc}");
    {   // 手で選ぶと 2 回分。選び直さずに確定すると 1 回分足し、回数の多い方が先頭 (2026-10-30)
        Learning l(work / "learned_once.json");
        l.clear();
        l.record(u"かわ", u"革");
        auto b = l.apply_order(u"かわ", {u"川", u"皮", u"革"});
        expect("learned once: chosen goes first", b[0], u"革");
        expect("learned once: top()", l.top(u"かわ"), u"革");
        l.confirm(u"かわ", u"川");
        l.confirm(u"かわ", u"川");
        l.confirm(u"かわ", u"川");
        b = l.apply_order(u"かわ", {u"川", u"皮", u"革"});
        expect("learned: confirmed takes it back", b[0], u"川");
        l.confirm(u"やま", u"山");
        if (!l.counts(u"やま").empty()) { printf("FAIL confirm unlearned reading\n"); failures++; }
        // 前の版のファイルの回数は手で選んだ回数なので 2 倍に
        { std::ofstream f(work / "learned_v1.json", std::ios::binary); f << "{\"" << to_utf8(u"かわ") << "\":{\"" << to_utf8(u"革") << "\":1}}"; }
        Learning v1(work / "learned_v1.json");
        expect("learned: old file", v1.counts(u"かわ")[0].second == 2 ? u16(u"2") : u16(u"?"), u"2");
        expect("learned: old file has no context", v1.top(u"かわ"), u16());
        // 語の学習は文脈ごと: 同じ文脈なら手で 1 回、別の文脈は手で 3 回で、その文脈に反対の票が無いときだけ
        Learning lc(work / "learned_ctx.json");
        lc.clear();
        lc.record(u"とった", u"撮った", 2, u"写真を");
        expect("context: same", lc.top(u"とった", u"古い写真を"), u"撮った");
        expect("context: other", lc.top(u"とった", u"メモを"), u16());
        lc.record(u"とった", u"撮った", 2, u"景色を");
        auto undone = lc.record(u"とった", u"撮った", 2, u"動画を");
        expect("context: three picks spread", lc.top(u"とった", u"メモを"), u"撮った");
        lc.restore({undone});
        expect("context: undo", lc.top(u"とった", u"メモを"), u16());
        lc.record(u"とった", u"撮った", 2, u"景色を");
        lc.confirm(u"とった", u"取った", u"メモを");
        expect("context: vote against", lc.top(u"とった", u"メモを"), u16());
        expect("context: other place stays", lc.top(u"とった", u"人物を"), u"撮った");
        Learning lc2(work / "learned_ctx.json");
        expect("context: saved", lc2.top(u"とった", u"写真を"), u"撮った");
        lc2.forget(u"とった", u"撮った");
        expect("context: forget", lc2.top(u"とった", u"写真を"), u16());
        {   // 用途の違う表記を両方選ぶ短い読み (か/化): 初めての文脈では取り合わずモデルに任せ、選んだ文脈ではそれぞれ (2026-11-06)
            Learning lk(work / "learned_ka.json");
            lk.clear();
            for (const char16_t* left : {u"行く", u"本当", u"そう"}) lk.record(u"か", u"か", 2, left);
            for (const char16_t* left : {u"自動", u"電子", u"映画"}) lk.record(u"か", u"化", 2, left);
            lk.record(u"か", u"化", 2, u"高齢");
            expect("shared reading: new context", lk.top(u"か", u"できる"), u16());
            expect("shared reading: its context (か)", lk.top(u"か", u"本当"), u"か");
            expect("shared reading: its context (化)", lk.top(u"か", u"電子"), u"化");
            lk.record(u"かわ", u"革", 2, u"牛");
            lk.record(u"かわ", u"革", 2, u"本");
            bool ka = false, kawa = false;
            for (auto& st : lk.strong(4)) { ka |= st.reading == u"か"; kawa |= st.reading == u"かわ" && st.surface == u"革"; }
            if (!ka && kawa) printf("ok   shared reading: not strong\n");
            else { printf("FAIL shared reading strong: ka %d kawa %d\n", int(ka), int(kawa)); failures++; }
        }
        {   // 時間を指定して消すと文脈の票も戻る
            auto d = work / "ctx_recent";
            std::filesystem::remove_all(d);
            std::filesystem::create_directories(d);
            {
                Learning a(d / L"learned.json");
                for (int i = 0; i < 3; i++) a.record(u"かみ", u"紙", 2, u"白い");
            }
            forget_recent(d, 0);
            Learning a(d / L"learned.json");
            a.record(u"かみ", u"神", 2, u"白い");
            expect("context: forget_recent drops votes", a.top(u"かみ", u"白い"), u"神");
        }
        {   // 枠から押し出した表記・消した表記の票は残らない。取り消すと押し出した表記が戻る
            std::filesystem::remove(work / "learned_ev.json");
            Learning a(work / "learned_ev.json");
            a.record(u"かい", u"表1", 2, u"右の");
            for (int i = 2; i <= 5; i++) a.record(u"かい", u"表" + u16(1, char16_t(u'0' + i)), 4, u"左の");
            Learning::Undo u = a.record(u"かい", u"表6", 4, u"下の");
            bool gone = true;
            for (auto& c : a.counts(u"かい")) if (c.first == u"表1") gone = false;
            if (!gone) { printf("FAIL evict\n"); failures++; }
            a.record(u"かい", u"表2", 2, u"右の");
            expect("context: evicted votes dropped", a.top(u"かい", u"右の"), u"表2");
            a.record(u"かい", u"表3", 4, u"右の");
            a.hide(u"かい", u"表3");
            expect("context: hidden votes dropped", a.top(u"かい", u"右の"), u"表2");
            std::filesystem::remove(work / "learned_ev2.json");
            Learning b(work / "learned_ev2.json");
            for (int i = 1; i <= 5; i++) b.record(u"かい", u"表" + u16(1, char16_t(u'0' + i)), 4);
            Learning::Undo v = b.record(u"かい", u"表6", 4);
            b.restore({v});
            int n = 0;
            for (auto& c : b.counts(u"かい")) n += c.second > 0;
            if (n != 5 || v.evicted.empty()) { printf("FAIL evict undo %d\n", n); failures++; }
            (void)u;
        }
    }

    // ---- ユーザー辞書
    std::string err = dict.add(u"しゅんてぃ", u"shunti", "noun", "");
    if (!err.empty()) { printf("FAIL userdict add: %s\n", err.c_str()); failures++; }
    keys(c, "shunnthiha{sp}");
    printf("     userdict: %s\n", u8(c.view().text).c_str());
    keys(c, "{esc}{esc}");
    // 変換中の Ctrl+Delete: 選んでいる候補を消すと、その読みでは以後出ない (2026-10-16)
    {
        keys(c, "kawa{sp}");
        const u16 top = c.view().text;
        KeyEvent del;
        del.key = Key::Delete;
        del.ctrl = true;
        c.press(del);
        keys(c, "{esc}{esc}{esc}");
        keys(c, "kawa{sp}");
        const u16 after = c.view().text;
        printf("     hide: %s -> %s\n", u8(top).c_str(), u8(after).c_str());
        if (after == top) { printf("FAIL hide candidate\n"); failures++; }
        keys(c, "{esc}{esc}{esc}");
        learning.record(u"かわ", top);   // 自分で選び直すと戻る (2 回選ぶと 1 位)
        learning.record(u"かわ", top);
        keys(c, "kawa{sp}");
        if (c.view().text != top) { printf("FAIL unhide by record\n"); failures++; }
        keys(c, "{esc}{esc}{esc}");
    }
    // かなを直接入れる (かな入力の配列): ゛ ゜ は前のかなに合わせる
    c.type_kana(u"か");
    c.type_kana(u"゛");
    c.type_kana(u"は゜");
    expect("type_kana", c.view().text, u"がぱ");
    keys(c, "{esc}{esc}{esc}");
    // 確定の取り消し: 確定の直後だけ (表記, 読み) を覚えていて、読みに戻して変換中にできる (2026-10-16)
    {
        keys(c, "kawa{sp}{enter}");
        const u16 t = c.take_commit();
        if (c.undoable().first != t || c.undoable().second != u"かわ") { printf("FAIL undoable\n"); failures++; }
        if (!c.undo_commit() || !c.composing()) { printf("FAIL undo commit\n"); failures++; }
        keys(c, "{esc}");   // 取り消しをやめたら元の文に戻る。読みに置き換えた字を消さない
        expect("undo cancelled", c.take_commit(), t);
        // 確定の取り消しは、その確定で覚えたことも戻す (2026-10-30)
        keys(c, "yamaba{sp}{sp}{enter}");
        const u16 picked = c.take_commit();
        int before = 0;
        for (auto& p : learning.counts(u"やまば")) if (p.first == picked) before = p.second;
        if (before > 0 && c.undo_commit()) {
            int after = 0;
            for (auto& p : learning.counts(u"やまば")) if (p.first == picked) after = p.second;
            if (after != before - 2) { printf("FAIL undo learning %d %d\n", before, after); failures++; }
        }
        keys(c, "{esc}{esc}{esc}");
        c.take_commit();
        expect("romaji reading", c.romaji_reading(u"konnichiha Sekai"), u"こんにちはせかい");
        expect("romaji reading bad", c.romaji_reading(u"hello2"), u16());
        expect("romaji reading word", c.romaji_reading(u"xyz"), u16());
        // 確定してすぐ全部消し、同じ読みで別の表記を確定したら、前の表記の票を減らす。確定の取り消しで戻る (2026-11-06)
        {
            auto count_of = [&](const u16& s) { int n = 0; for (auto& p : learning.counts(u"やまば")) if (p.first == s) n = p.second; return n; };
            KeyEvent bs;
            bs.key = Key::Backspace;
            keys(c, "yamaba{sp}{sp}{enter}");
            const u16 a = c.take_commit();
            const int before = count_of(a);
            for (size_t i = 0; i < a.size(); i++) c.passthrough(bs);
            keys(c, "yamaba{sp}");
            if (c.view().text == a) keys(c, "{sp}");
            keys(c, "{enter}");
            const u16 b = c.take_commit();
            const int after = count_of(a);
            printf("     retype: %d -> %d\n", before, after);
            if (b == a || before <= 0 || after != std::max(0, before - 2)) { printf("FAIL retype demote\n"); failures++; }
            if (!c.undo_commit()) { printf("FAIL retype undo\n"); failures++; }
            else if (count_of(a) != before) { printf("FAIL retype restore %d %d\n", before, count_of(a)); failures++; }
            keys(c, "{esc}{esc}{esc}");
            c.take_commit();
            // 同じ表記を打ち直したとき・一部だけ消したときは減らさない
            keys(c, "yamaba{sp}");
            const u16 t1 = c.view().text;
            keys(c, "{enter}");
            c.take_commit();
            const int b1 = count_of(t1);
            for (size_t i = 0; i < t1.size(); i++) c.passthrough(bs);
            keys(c, "yamaba{sp}{enter}");
            const u16 t2 = c.take_commit();
            if (t2 == t1 && count_of(t1) < b1) { printf("FAIL retype same\n"); failures++; }
            keys(c, "yamaba{sp}{enter}");
            const u16 t3 = c.take_commit();
            const int b3 = count_of(t3);
            c.passthrough(bs);
            keys(c, "yamaba{sp}{sp}{enter}");
            c.take_commit();
            if (count_of(t3) < b3) { printf("FAIL retype partial\n"); failures++; }
            keys(c, "{esc}{esc}{esc}");
        }
        keys(c, "kawa{sp}{enter}a{esc}");   // ほかのキーを押したら取り消せない
        c.take_commit();
        if (!c.undoable().first.empty()) { printf("FAIL undoable after key\n"); failures++; }
    }
    // 空白の変換も、打っている間の一覧 (Tab・スマホ) と同じ 1 位: 読み全体に当たるユーザー辞書の語 (2026-10-16)
    dict.add(u"てすとよみ", u"試験読語", "noun", "");
    keys(c, "tesutoyomi{sp}");
    expect("space = live top (userdict)", c.view().text, u"試験読語");
    keys(c, "{esc}{esc}");

    // ---- まとめて登録 (MS-IME の一覧の出力の形)・読みに記号と英数字
    {
        auto r = dict.import_text(u"!Microsoft IME Dictionary Tool\r\nあっとほーむ\tアットホーム株式会社\t固有名詞\r\n＠ほーむ\t@home\t短縮よみ\r\nｗｗ\t(笑)\t顔文字\r\nかいしゃ\t会社\r\nだめ\t駄目\t動詞\r\n");
        printf("     import: %d ok, %zu bad\n", r.first, r.second.size());
        if (r.first != 4 || r.second.size() != 1) { printf("FAIL import\n"); failures++; }
        auto hit = dict.lookup(u"@ほーむ");   // 半角で引いても全角の登録に当たる
        if (hit.empty() || hit[0] != u"@home") { printf("FAIL symbol reading\n"); failures++; }
        if (UserDict::pos_from_label(u"顔文字").first != "kaomoji") { printf("FAIL pos label\n"); failures++; }
        if (UserDict::pos_from_label(u"感動詞").first != "interjection") { printf("FAIL pos label 感動詞\n"); failures++; }
    }

    // ---- 詳細設定: 記号ごとの半角、テンキー、プライベート、再変換、ローマ字の表、辞書の書き出し
    c.options.half_symbols = u"!?@";
    keys(c, "hai!{enter}");
    expect("half symbol !", c.take_commit(), u"はい!");
    keys(c, "hai#{enter}");
    expect("full symbol #", c.take_commit(), u"はい＃");
    c.options.half_symbols.clear();
    {
        KeyEvent np;
        np.key = Key::Char;
        np.ch = u'5';
        np.raw = true;
        c.options.numpad = 2;
        c.press(np);
        keys(c, "{enter}");
        expect("numpad full", c.take_commit(), u"５");
        c.options.numpad = 0;
        c.press(np);
        keys(c, "{enter}");
        expect("numpad half", c.take_commit(), u"5");
    }
    c.reset_history();   // 前の試験の確定 (数字など) を文脈にしない (モデルによって 1 位が揺れる)
    keys(c, "kaigiwotyuusisuru{sp}{enter}");
    u16 rc_text = c.take_commit();
    c.reset_history();
    u16 back = c.reading_of(rc_text);
    printf("     reconvert: %s -> %s\n", u8(rc_text).c_str(), u8(back).c_str());
    if (back != u"かいぎをちゅうしする") { printf("FAIL reading_of\n"); failures++; }
    if (!c.start_reconvert(back) || c.view().text.empty()) { printf("FAIL start_reconvert\n"); failures++; }
    keys(c, "{enter}");
    expect("reconvert commit", c.take_commit(), rc_text);
    if (!c.start_reconvert(back, rc_text)) { printf("FAIL start_reconvert original\n"); failures++; }
    keys(c, "{bs}{esc}");   // 読みに戻してからやめても元の文に戻る
    expect("reconvert cancelled", c.take_commit(), rc_text);
    if (c.reading_of(u"ほかで入れた漢字") != u"") { printf("FAIL reading_of unknown\n"); failures++; }
    c.options.private_mode = true;
    keys(c, "himitunokotoba{sp}{enter}");
    u16 secret = c.take_commit();
    if (!c.reading_of(secret).empty() && secret.find(u'密') != u16::npos) { printf("FAIL private kept\n"); failures++; }
    c.options.private_mode = false;
    c.add_romaji(u"xn", u"ん");
    keys(c, "kaxn{enter}");
    expect("custom romaji", c.take_commit(), u"かん");
    c.reset_romaji();   // 足した綴りを外すと元の表 (xn は ん にならない)
    keys(c, "kaxn{enter}");
    if (c.take_commit() == u"かん") { printf("FAIL reset romaji\n"); failures++; }
    {
        u16 ex = dict.export_text();
        if (ex.find(u"\t@home\t短縮よみ") == u16::npos) { printf("FAIL export\n"); failures++; }
    }

    // ---- Tab は予測 (読みの続く語) から選び始める、↓ は 1 位から
    keys(c, "kaisi{tab}");
    printf("     tab sel %d: %s\n", c.view().cand_sel, c.view().cand_sel >= 0 && c.view().cand_sel < int(c.view().cands.size()) ? u8(c.view().cands[size_t(c.view().cand_sel)]).c_str() : "-");
    keys(c, "{esc}{esc}");

    // ---- 長い文
    std::string longr;
    for (int i = 0; i < 12; i++) longr += "kyouhaiitenkidesune";
    t0 = std::chrono::steady_clock::now();
    keys(c, longr + "{sp}");
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("     long (%zu kana) convert %.1f ms: %s\n", size_t(12 * 11), ms, u8(c.view().text).substr(0, 60).c_str());
    keys(c, "{esc}{esc}");

    // ---- 括弧は全種類を候補に出す (24 種類。打った字が 1 位、かっこ は組で)
    {
        auto count_in = [](const std::vector<u16>& cands, std::initializer_list<const char16_t*> want) {
            int n = 0;
            for (auto* w : want) n += std::find(cands.begin(), cands.end(), u16(w)) != cands.end();
            return n;
        };
        auto check = [&](const char* name, const std::string& seq, const char16_t* top, std::initializer_list<const char16_t*> want) {
            keys(c, seq);
            const std::vector<u16>& cs = c.view().cands;
            expect(name, cs.empty() ? u"" : cs[0], top);
            int n = count_in(cs, want);
            if (n != int(want.size())) { printf("FAIL %s: %d / %zu kinds in cands\n", name, n, want.size()); failures++; }
            keys(c, "{esc}{esc}");
        };
        std::initializer_list<const char16_t*> opens = {u"（", u"(", u"「", u"『", u"【", u"［", u"[", u"｛", u"{", u"〔", u"〈", u"《",
                                                         u"〖", u"〘", u"〚", u"｢", u"＜", u"<", u"«", u"‹", u"“", u"‘", u"〝", u"｟"};
        std::initializer_list<const char16_t*> closes = {u"）", u")", u"」", u"』", u"】", u"］", u"]", u"｝", u"}", u"〕", u"〉", u"》",
                                                          u"〗", u"〙", u"〛", u"｣", u"＞", u">", u"»", u"›", u"”", u"’", u"〟", u"｠"};
        check("bracket open (live)", "[", u"「", opens);
        check("bracket open (convert)", "[{sp}", u"「", opens);
        check("bracket close (convert)", "]{sp}", u"」", closes);
        check("bracket paren (convert)", "({sp}", u"（", opens);
        keys(c, "kakko{sp}");
        int pairs = count_in(c.view().cands, {u"（）", u"「」", u"『』", u"【】", u"〘〙", u"〚〛", u"｟｠", u"«»", u"“”", u"〝〟"});
        if (pairs != 10) { printf("FAIL kakko: %d / 10 pairs in cands\n", pairs); failures++; }
        else printf("ok   kakko: all pairs in cands\n");
        keys(c, "{esc}{esc}");
    }

    // ---- 空白キーで変換しない (space_literal): 空白も読みに入れて変換を続け、幅は前後の字で決める
    {
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        s.options.space_literal = true;
        // 英字の語の判定はモデル次第 (S8 は空白で区切った英語の文を学んでいない: push -> puSH)。ここでは空白で途切れず、
        // 英字の後の空白が半角になることだけ見る
        keys(s, "git push{enter}");
        {
            u16 got = s.take_commit();
            if (got.size() == 8 && got.compare(0, 4, u"git ") == 0) printf("ok   space literal latin: %s\n", u8(got).c_str());
            else { printf("FAIL space literal latin: got [%s]\n", u8(got).c_str()); failures++; }
        }
        keys(s, "kyouha iitenki{enter}");
        expect("space literal kana", s.take_commit(), u"今日は　いい天気");
        keys(s, "kyouha{S-sp}iitenki{enter}");   // Shift+空白も空白 (この設定では変換に使わない。ユーザー 2026-10-27)
        expect("space literal Shift+space", s.take_commit(), u"今日は　いい天気");
        keys(s, "kyouhaiitenki{left}{sp}");   // 矢印で入った変換中の空白は、確定して空白
        expect("space literal space after arrow", s.take_commit(), u"今日はいい天気　");
        keys(s, "kyouhaiitenki{left}");   // 矢印キーで文節に分ける変換に入る (← は最後の文節から)
        {
            const View& v = s.view();
            const bool last = !v.spans.empty() && v.spans.back().kind == SpanKind::Focused && v.spans.size() >= 2;
            if (last) printf("ok   space literal: left arrow enters phrases (%d)\n", int(v.spans.size()));
            else { printf("FAIL space literal: left arrow did not enter phrases\n"); failures++; }
        }
        keys(s, "{esc}{esc}");
    }

    // ---- 句読点で確定 (punct_commit): 、。 を打ったら句読点を添えて確定する。変換中の句読点も、選んだ候補と句読点で確定
    {
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        s.options.punct_commit = true;
        s.options.live_display = true;
        keys(s, "kyouhaiitenki.");
        expect("punct commit live", s.take_commit(), u"今日はいい天気。");
        if (s.composing()) { printf("FAIL punct commit: still composing\n"); failures++; }
        s.options.live_display = false;
        keys(s, "kyouhaiitenki,");   // ライブ変換がオフでも変換して確定する (空白 → Enter と同じ)
        expect("punct commit converts", s.take_commit(), u"今日はいい天気、");
        if (s.composing()) { printf("FAIL punct commit converts: still composing\n"); failures++; }
        keys(s, "kyouha{sp}");
        {
            const u16 shown = s.view().text;
            keys(s, ",");
            expect("punct commit after convert", s.take_commit(), shown + u"、");
        }
        keys(s, "honntou?");   // 感嘆符・疑問符でも切る
        {
            u16 got = s.take_commit();
            if (!got.empty() && got.back() == u'？' && !s.composing()) printf("ok   punct commit question: %s\n", u8(got).c_str());
            else { printf("FAIL punct commit question: got [%s]\n", u8(got).c_str()); failures++; }
        }
        keys(s, "3.5{enter}");   // 数字の後の . は句読点にならないので確定しない
        {
            u16 got = s.take_commit();
            if (got.find(u'5') != u16::npos) printf("ok   punct commit: decimal kept (%s)\n", u8(got).c_str());
            else { printf("FAIL punct commit decimal: got [%s]\n", u8(got).c_str()); failures++; }
        }
        s.options.punct_commit = false;
        keys(s, "kyouha,");
        if (s.composing()) printf("ok   punct commit off: still composing\n");
        else { printf("FAIL punct commit off: committed\n"); failures++; }
        keys(s, "{esc}{esc}");
    }

    // ---- ユーザー辞書の英字の語: 読みを空にして登録すると、綴りどおりに打ったとき候補の 2 番目に出る
    {
        std::string err = dict.add(u"", u"ShuntiLettuce", "noun", "");
        if (!err.empty()) { printf("FAIL latin user word add: %s\n", err.c_str()); failures++; }
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        keys(s, "shuntilettuce");
        const auto& cs = s.view().cands;
        if (cs.size() >= 2 && (cs[0] == u"ShuntiLettuce" || cs[1] == u"ShuntiLettuce")) printf("ok   latin user word top 2: %s / %s\n", u8(cs[0]).c_str(), u8(cs[1]).c_str());
        else { printf("FAIL latin user word not 2nd (%d cands)\n", int(cs.size())); failures++; }
        keys(s, "{esc}");
        dict.remove(u"shuntilettuce", u"ShuntiLettuce");
        {   // 網の英字 (2026-11-05): かなの途中で終わる英字の語 (minecraft|apuri)・l で打った語 (lettuce -> ぇっつせ)
            auto has = [](const std::vector<u16>& v, const u16& w) { return std::find(v.begin(), v.end(), w) != v.end(); };
            keys(s, "minecraftapuri");
            const auto c3 = s.view().cands;
            if (has(c3, u"minecraftアプリ")) printf("ok   latin mid-kana: %s (rank %d)\n", u8(c3[0]).c_str(), int(std::find(c3.begin(), c3.end(), u16(u"minecraftアプリ")) - c3.begin()) + 1);
            else { printf("FAIL latin mid-kana: %s\n", c3.empty() ? "" : u8(c3[0]).c_str()); failures++; }
            keys(s, "{esc}");
            keys(s, "lettuce");
            const auto c4 = s.view().cands;
            if (has(c4, u"lettuce")) printf("ok   latin l: %s (rank %d)\n", u8(c4[0]).c_str(), int(std::find(c4.begin(), c4.end(), u16(u"lettuce")) - c4.begin()) + 1);
            else { printf("FAIL latin l: %s\n", c4.empty() ? "" : u8(c4[0]).c_str()); failures++; }
            keys(s, "{esc}");
        }
        keys(s, "openai");   // 辞書の大文字の混ざる英字の語も 2 番目に
        const auto& c2 = s.view().cands;
        if (c2.size() >= 2 && (c2[0] == u"OpenAI" || c2[1] == u"OpenAI")) printf("ok   dict latin word top 2: %s / %s\n", u8(c2[0]).c_str(), u8(c2[1]).c_str());
        else { printf("FAIL dict latin word not 2nd (%d cands)\n", int(c2.size())); failures++; }
        keys(s, "{esc}");
        keys(s, "openainohougaii");   // 文の途中の英字の語: キーの並びを渡して網に入れる (順位はモデル次第なので表示だけ)
        {
            int at = -1;
            const auto& c3 = s.view().cands;
            for (size_t i = 0; i < c3.size(); i++) if (c3[i].find(u"OpenAI") != u16::npos) { at = int(i); break; }
            printf("     probe openai in sentence: 1st %s, OpenAI at %d\n", c3.empty() ? "" : u8(c3[0]).c_str(), at);
        }
        keys(s, "{esc}");
    }

    // ---- 打ちすぎた字を消してもキーの並びを使い続ける (英字が出なくならない) / 長く打っても前の打ち間違いの直しが消えない
    {
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        s.options.live_display = true;
        keys(s, "minecraftt{bs}{enter}");
        {   // 英字が出ればよい (辞書の書き方 Minecraft でも、打ったまま minecraft でも)
            const u16 got = s.take_commit();
            if (got == u"Minecraft" || got == u"minecraft") printf("ok   latin after backspace: %s\n", u8(got).c_str());
            else { printf("FAIL latin after backspace: got [%s]\n", u8(got).c_str()); failures++; }
        }
        keys(s, "yorosikuonegaisimasu{enter}");
        u16 base = s.take_commit();
        keys(s, "yorisikuonegaisimasu{enter}");
        u16 short_typo = s.take_commit();
        keys(s, "yorisikuonegaisimasukyouhaiitenkidesunesanponiikimasenka{enter}");
        u16 long_typo = s.take_commit();
        printf("     typo short: %s / long: %s\n", u8(short_typo).c_str(), u8(long_typo).c_str());
        if (short_typo.compare(0, 4, u"よろしく") == 0 && long_typo.compare(0, 4, u"よろしく") != 0) {
            printf("FAIL typo correction lost when typing longer\n");
            failures++;
        }
        (void)base;
    }

    // ---- 大文字で英字のまま (caps_alpha) を切ると、大文字も小文字と同じくローマ字になる
    {
        Composer s(&conv, &learning, &dict);
        s.options.live_commit = false;
        s.options.private_mode = true;
        keys(s, "Kyou{enter}");
        expect("caps alpha on", s.take_commit(), u"Kyou");
        s.options.caps_alpha = false;
        keys(s, "IMEwotukau{enter}");
        expect("caps alpha off", s.take_commit(), u"IMEをつかう");
        keys(s, "IMEwotukau{sp}{enter}");   // 変換しても大文字の英字は変わらない
        {
            u16 got = s.take_commit();
            if (got.compare(0, 4, u"IMEを") == 0) printf("ok   caps alpha off convert: %s\n", u8(got).c_str());
            else { printf("FAIL caps alpha off convert: %s\n", u8(got).c_str()); failures++; }
        }
    }

    {   // 書き方の癖 (style.h): 覚えた書き方に 1 位を入れ替える。記録が少ない・半々なら入れ替えない
        StyleModel sm;
        sm.set_variants("できない\t出来ない\t出来る\t1\nこと\t事\t事\t2\n");
        Conversion c;
        c.cands = {u"今日はできないこと", u"今日は出来ないこと"};
        c.words = {{u"きょう", u"今日"}, {u"は", u"は"}, {u"できない", u"できない"}, {u"こと", u"こと"}};
        Conversion c0 = c;
        expect("restyle: no record", sm.restyle(u"", c0) ? c0.cands[0] : u"(そのまま)", u"(そのまま)");
        StyleLog log(work / "style.json");
        log.clear();
        const auto w = sm.weight_of(u"");
        for (int i = 0; i < 3; i++) log.record(u"出来ない", w);
        sm.set_counts(log.counts());
        Conversion c1 = c;
        sm.restyle(u"", c1);
        expect("restyle: kanji learned", c1.cands[0], u"今日は出来ないこと");
        expect("restyle: original kept 2nd", c1.cands.size() > 1 ? c1.cands[1] : u"", u"今日はできないこと");
        expect("restyle: words follow", c1.words[2].surface, u"出来ない");
        Conversion c3;   // 網が でき|ない と 2 語に分けても、つないで表を引く (S8 の網)
        c3.cands = {u"それはできない"};
        c3.words = {{u"それ", u"それ"}, {u"は", u"は"}, {u"でき", u"でき"}, {u"ない", u"ない"}};
        sm.restyle(u"", c3);
        expect("restyle: joined words", c3.cands[0], u"それは出来ない");
        for (int i = 0; i < 3; i++) log.record(u"できない", w);
        sm.set_counts(log.counts());
        Conversion c2 = c;
        expect("restyle: split record stays", sm.restyle(u"", c2) ? c2.cands[0] : u"(そのまま)", u"(そのまま)");
        std::string bin;   // 判定器: 学習したときの確率と同じ (int8 にした分のずれは 0.03 まで)
        std::ifstream f(fs::path(argv[1]).parent_path() / "kkc_register.bin", std::ios::binary);
        bin.assign(std::istreambuf_iterator<char>(f), {});
        if (!sm.set_register(bin)) { printf("FAIL register: kkc_register.bin を読めない\n"); failures++; }
        for (auto [ctx, p1] : std::vector<std::pair<u16, float>>{{u"日本の漫画家。鹿児島県出身。", .0554f}, {u"それマジで草www", .9805f}, {u"お世話になっております。", .0113f}}) {
            const float got = sm.register_of(ctx)[1];
            if (std::abs(got - p1) <= .03f) printf("ok   register %s: %.3f\n", u8(ctx).c_str(), got);
            else { printf("FAIL register %s: %.3f (Python %.3f)\n", u8(ctx).c_str(), got, p1); failures++; }
        }
        StyleLog again(work / "style.json");
        expect("style log reload", again.counts().count(u"出来ない") && again.counts().at(u"出来ない").n == 3 ? u"3" : u"?", u"3");
    }

    {   // 更新の確認: Release の一覧から最新の版と主な変更点を読む (2026-10-28)
        std::ifstream jf(fs::path(__FILE__).parent_path() / "release_test.json", std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        auto join = [](const std::vector<std::string>& v) { std::string o; for (auto& x : v) o += "[" + x + "]"; return from_utf8(o); };
        ReleaseInfo w = latest_release(json, "windows-v", "shunti-IME-windows-", ".zip");
        expect("release: windows version", from_utf8(w.version), u"0.7.3");
        expect("release: bullets (headings only, until next section)", join(w.changes), u"[書き方][修正]");
        ReleaseInfo l = latest_release(json, "linux-v", "fcitx5-shunti_", ".deb");
        expect("release: hidden list wins", join(l.changes), u"[小数]");
        expect("release: older", version_older("0.7.2", "0.7.3") && !version_older("0.7.10", "0.7.9") ? u"ok" : u"ng", u"ok");
    }

    printf(failures ? "\n%d FAILED\n" : "\nall ok\n", failures);
    return failures ? 1 : 0;
}

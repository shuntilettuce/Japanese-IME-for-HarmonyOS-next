// PC 版の共通部分 (desktop/core) の試験。IME なしで打鍵を流し込み、入力中の見せ方と確定した文を確かめる。
//   core_test.exe <kkc_lex.bin> <kkc_model.bin> [作業用のフォルダ]
// 期待と違えば FAIL を出し、終了コードを 1 にする。変換の時間も出す。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../core/composer.h"
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

// 打鍵の列: 普通の字はそのまま、{sp} {enter} {esc} {bs} {del} {left} {right} {up} {down} {home} {end} {tab}
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
        expect("datetime 20260230 (ありえない日)", u16(pred("20260230").empty() ? u"なし" : u"あり"), u"なし");
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
    keys(c, "k");
    expect("pending romaji", c.view().text, u"きょうはk");
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
    keys(c, "kyouhaiiten{left}{enter}");   // 読みの途中にカーソルを動かしたら、かなで見せてかなで確定
    expect("live display falls back to kana mid-reading", c.take_commit(), u"きょうはいいてん");
    keys(c, "kyouhaiitenki");   // 英語入力に切り替えた (finish): 見えている変換のまま確定する
    shown = c.view().text;
    c.finish();
    expect("live display finish commits what is shown", c.take_commit(), shown);
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
    // 1 回だけ選んだ表記はモデルの 1 位を覆さない (2 位に上がる)。2 回選ぶと 1 位 (2026-10-20 学習の審査)
    c.reset_history();
    keys(c, "kawa{sp}");
    expect("learned once: model top stays", c.view().text, first);
    keys(c, "{sp}");
    expect("learned once: next is the chosen one", c.view().text, second);
    keys(c, "{enter}");
    c.take_commit();
    keys(c, "kawa{sp}");
    expect("learned twice", c.view().text, second);
    printf("     (first was %s)\n", u8(first).c_str());
    keys(c, "{esc}{esc}");

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
        keys(c, "{esc}{esc}{esc}");
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

    printf(failures ? "\n%d FAILED\n" : "\nall ok\n", failures);
    return failures ? 1 : 0;
}

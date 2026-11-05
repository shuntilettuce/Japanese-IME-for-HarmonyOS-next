// 架空の打ち手で Composer を動かして、使い勝手の引っかかりを数える (IME なし)。
//   sim_user <kkc_lex.bin> <kkc_model.bin> <cases.tsv> [--out 書き出しの頭] [--jobs N] [--gold-alts]
// 書き出し: <頭>_examples.tsv (引っかかった例)・<頭>_cases.tsv (文ごとの結果)。並列は文ごとに分ける (既定はコアの数)
// cases.tsv: 1 行 1 文で 名前 \t 左の文脈 \t 読み (ひらがな) \t 正解 (空白で区切って複数可)。読みをローマ字の打鍵にして、打ち方を変えて流し込む。
// 数えるもの:
//   後退     ライブ変換で、正解と合っていた所が後から合わなくなった (打ち進めたら良い変換が崩れた)
//   一致     Enter で確定した文が正解と同じ (句読点・空白を除いて比べる「ゆるい一致」も)
//   打ち方   同じ文を別の打ち方 (ん を n 1 打・打ち間違えて Backspace・手が止まってから変換) で打ったとき、確定が変わった
//   空白     空白で変換したときに見える文が正解か
//   遅さ     1 打の最大の時間
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../core/composer.h"
#include "../core/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace shunti;

static std::string u8(const u16& s) { return to_utf8(s); }

static std::vector<char> slurp(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- 読み → ローマ字の打鍵
static std::map<u16, std::string> romaji_table() {
    std::map<u16, std::string> t;
    const char* base[][2] = {
        {"あ", "a"}, {"い", "i"}, {"う", "u"}, {"え", "e"}, {"お", "o"}, {"か", "ka"}, {"き", "ki"}, {"く", "ku"}, {"け", "ke"}, {"こ", "ko"},
        {"さ", "sa"}, {"し", "shi"}, {"す", "su"}, {"せ", "se"}, {"そ", "so"}, {"た", "ta"}, {"ち", "chi"}, {"つ", "tsu"}, {"て", "te"}, {"と", "to"},
        {"な", "na"}, {"に", "ni"}, {"ぬ", "nu"}, {"ね", "ne"}, {"の", "no"}, {"は", "ha"}, {"ひ", "hi"}, {"ふ", "fu"}, {"へ", "he"}, {"ほ", "ho"},
        {"ま", "ma"}, {"み", "mi"}, {"む", "mu"}, {"め", "me"}, {"も", "mo"}, {"や", "ya"}, {"ゆ", "yu"}, {"よ", "yo"},
        {"ら", "ra"}, {"り", "ri"}, {"る", "ru"}, {"れ", "re"}, {"ろ", "ro"}, {"わ", "wa"}, {"を", "wo"}, {"ん", "nn"},
        {"が", "ga"}, {"ぎ", "gi"}, {"ぐ", "gu"}, {"げ", "ge"}, {"ご", "go"}, {"ざ", "za"}, {"じ", "ji"}, {"ず", "zu"}, {"ぜ", "ze"}, {"ぞ", "zo"},
        {"だ", "da"}, {"ぢ", "di"}, {"づ", "du"}, {"で", "de"}, {"ど", "do"}, {"ば", "ba"}, {"び", "bi"}, {"ぶ", "bu"}, {"べ", "be"}, {"ぼ", "bo"},
        {"ぱ", "pa"}, {"ぴ", "pi"}, {"ぷ", "pu"}, {"ぺ", "pe"}, {"ぽ", "po"}, {"ぁ", "xa"}, {"ぃ", "xi"}, {"ぅ", "xu"}, {"ぇ", "xe"}, {"ぉ", "xo"},
        {"ゃ", "xya"}, {"ゅ", "xyu"}, {"ょ", "xyo"}, {"ゎ", "xwa"}, {"ゔ", "vu"}, {"っ", "xtu"},
        {"、", ","}, {"。", "."}, {"ー", "-"}, {"！", "!"}, {"？", "?"}, {"「", "["}, {"」", "]"}, {"・", "/"}, {"～", "~"},
        {"てぃ", "thi"}, {"でぃ", "dhi"}, {"でゅ", "dhu"}, {"とぅ", "twu"}, {"うぃ", "wi"}, {"うぇ", "we"}, {"うぉ", "who"},
    };
    for (auto& b : base) t[from_utf8(b[0])] = b[1];
    const char* y[][2] = {{"き", "ky"}, {"ぎ", "gy"}, {"に", "ny"}, {"ひ", "hy"}, {"び", "by"}, {"ぴ", "py"}, {"み", "my"}, {"り", "ry"},
                          {"し", "sh"}, {"ち", "ch"}, {"じ", "j"}};
    for (auto& b : y) {
        t[from_utf8(b[0]) + u"ゃ"] = std::string(b[1]) + "a";
        t[from_utf8(b[0]) + u"ゅ"] = std::string(b[1]) + "u";
        t[from_utf8(b[0]) + u"ょ"] = std::string(b[1]) + "o";
    }
    for (const char* s : {"し", "ち", "じ"}) t[from_utf8(s) + u"ぇ"] = t[from_utf8(s) + u"ゃ"].substr(0, t[from_utf8(s) + u"ゃ"].size() - 1) + "e";
    const char* lit[] = {"ぁ", "ぃ", "ぇ", "ぉ"};
    const char* vow[] = {"a", "i", "e", "o"};
    for (int i = 0; i < 4; i++) {
        t[u"ふ" + from_utf8(lit[i])] = std::string("f") + vow[i];
        t[u"ゔ" + from_utf8(lit[i])] = std::string("v") + vow[i];
    }
    return t;
}

// 読みを打鍵の列 (1 要素 = その字の分のキー) に。打てない字があれば空。n1 = ん を続く子音の前では n 1 打で
static std::vector<std::string> to_keys(const u16& kana, bool n1) {
    static const auto T = romaji_table();
    std::vector<std::string> out;
    for (size_t i = 0; i < kana.size();) {
        char16_t ch = kana[i];
        if (ch >= u'0' && ch <= u'9') { out.push_back(std::string(1, char(ch))); i++; continue; }
        if (ch >= 0xFF10 && ch <= 0xFF19) { out.push_back(std::string(1, char(ch - 0xFEE0))); i++; continue; }
        std::string k;
        size_t len = 0;
        for (size_t n = 2; n >= 1 && !len; n--) {
            if (i + n > kana.size()) continue;
            auto it = T.find(kana.substr(i, n));
            if (it != T.end()) { k = it->second; len = n; }
        }
        if (!len) return {};
        out.push_back(k);
        i += len;
    }
    // っ: 続く字の子音を重ねる。ん: n 1 打 (続く字が母音・な行・や行・ん でない子音のとき)
    for (size_t i = 0; i < out.size(); i++) {
        const bool next_cons = i + 1 < out.size() && !out[i + 1].empty() && std::string("bcdfghjkmpqrstvwxz").find(out[i + 1][0]) != std::string::npos;
        if (out[i] == "xtu" && next_cons && out[i + 1][0] != 'x') out[i] = std::string(1, out[i + 1][0]);
        if (out[i] == "nn" && n1 && next_cons && out[i + 1][0] != 'y') out[i] = "n";
    }
    return out;
}

// ---------------------------------------------------------------- 打つ
struct Run {
    u16 commit;
    int regressions = 0;      // 正解と合っていた所が後から崩れた回数
    u16 reg_before, reg_after;
    double max_ms = 0;
    int rank = -1;            // 空白で変換したときの 1 位が正解なら 0、違えば -1
};

static double press(Composer& c, KeyEvent ev) {
    auto t = std::chrono::steady_clock::now();
    c.press(ev);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

static double type_str(Composer& c, const std::string& s) {
    double m = 0;
    for (char ch : s) {
        KeyEvent ev;
        ev.key = Key::Char;
        ev.ch = char16_t(ch);
        m = std::max(m, press(c, ev));
    }
    return m;
}

static size_t common(const u16& a, const u16& b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
    return n;
}

static u16 loose(const u16& s) {
    u16 o;
    for (char16_t ch : s) if (u16(u"、。，．,. 　").find(ch) == u16::npos) o += ch;
    return o;
}

// how: 0 = 1 打ごとに変換、1 = 手が止まってから (3 字ごと)、2 = 打ち間違えて Backspace (字の途中で 1 回)、3 = 空白で変換
static Run type_sentence(Composer& c, const std::vector<std::string>& keys, const std::vector<u16>& golds, int how, unsigned seed) {
    Run r;
    c.reset_history();
    c.options.live_display = how != 3;
    c.options.live_wait = how == 1;
    // 打ち間違える所: 前の字の打ちかけ (っ の子音 1 打) が残っていない所だけ。Backspace はかなを 1 字ずつ消すので、
    // っ の直後で打ち間違えると「ぽ」ごと消え、人は p を打ち直す (道具はそこまで真似ない)
    size_t typo_at = keys.empty() ? 0 : seed % keys.size();
    while (typo_at > 0 && typo_at < keys.size() && keys[typo_at - 1].size() == 1 && keys[typo_at - 1][0] >= 'a' && keys[typo_at - 1][0] <= 'z') typo_at++;
    size_t best = 0;   // ここまでに正解と合っていた長さ (見えていた変換の頭)
    for (size_t i = 0; i < keys.size(); i++) {
        if (how == 2 && i == typo_at && !keys[i].empty() && keys[i][0] >= 'a' && keys[i][0] <= 'z') {
            // 隣のキーを打って気づき、消して打ち直す
            static const std::string rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
            char k = keys[i][0], wrong = 'q';
            for (auto& row : rows) {
                size_t p = row.find(k);
                if (p != std::string::npos) wrong = row[p + 1 < row.size() ? p + 1 : p - 1];
            }
            r.max_ms = std::max(r.max_ms, type_str(c, std::string(1, wrong)));
            KeyEvent bs;
            bs.key = Key::Backspace;
            r.max_ms = std::max(r.max_ms, press(c, bs));
        }
        r.max_ms = std::max(r.max_ms, type_str(c, keys[i]));
        if (how == 1 && (i % 3 == 2 || i + 1 == keys.size())) c.flush_live();
        if (how == 3) continue;
        const u16& shown = c.view().text;
        size_t now = 0, alt = 0;
        for (size_t g = 0; g < golds.size(); g++) if (common(shown, golds[g]) > now) { now = common(shown, golds[g]); alt = g; }
        // 3 字より前で合っていた所が合わなくなった (打っている最中の末尾の揺れは数えない)
        if (best >= 3 && now + 3 < best) {
            if (!r.regressions) { r.reg_before = golds[alt].substr(0, best); r.reg_after = shown; }
            r.regressions++;
        }
        best = std::max(best, now);
    }
    KeyEvent ev;
    if (how == 3) {
        ev.key = Key::Space;
        r.max_ms = std::max(r.max_ms, press(c, ev));
        // 空白を押したときに見える文 (文節に分けた変換の 1 位) が正解か
        if (std::find(golds.begin(), golds.end(), c.view().text) != golds.end()) r.rank = 0;
    }
    ev.key = Key::Enter;
    r.max_ms = std::max(r.max_ms, press(c, ev));
    r.commit = c.take_commit();
    return r;
}

struct Case {
    std::string name;
    u16 ctx, kana;
    std::vector<u16> golds;
};

// 1 文の結果 (文ごとの表の 1 行) と、例の行
struct Result {
    bool done = false, kana_bad = false, exact = false, loose_ok = false, n1 = false, wait = false, typo = false, space_ok = false;
    int regressions = 0;
    double max_ms = 0;
    std::vector<std::string> examples;
};

static void simulate(Composer& c, u16& ctx, const Case& k, Result& out) {
    auto example = [&](const char* kind, const u16& gold, const u16& got) {
        out.examples.push_back(k.name + "\t" + kind + "\t" + u8(gold) + "\t" + u8(got));
    };
    ctx = k.ctx;
    const auto keys = to_keys(k.kana, false), keys1 = to_keys(k.kana, true);
    if (keys.empty()) return;
    // 打鍵がちゃんと読みになるか (F6 でひらがなにして比べる)
    c.options.live_display = false;
    c.options.live_commit = false;
    for (auto& s : keys) type_str(c, s);
    KeyEvent f6;
    f6.key = Key::F6;
    c.press(f6);
    const u16 got = c.view().text;
    KeyEvent esc;
    esc.key = Key::Escape;
    for (int i = 0; i < 5 && c.composing(); i++) c.press(esc);
    c.take_commit();
    c.options.live_commit = true;
    out.done = true;
    if (got != k.kana) { out.kana_bad = true; example("読み", k.kana, got); return; }
    const unsigned seed = unsigned(std::hash<std::string>()(k.name));
    auto any = [&](const u16& s, bool lz) {
        for (auto& g : k.golds) if (lz ? loose(s) == loose(g) : s == g) return true;
        return false;
    };
    const u16& gold = k.golds[0];
    Run a = type_sentence(c, keys, k.golds, 0, seed);
    out.max_ms = a.max_ms;
    out.exact = any(a.commit, false);
    out.loose_ok = any(a.commit, true);
    if (!out.loose_ok) example("外れ", gold, a.commit);
    out.regressions = a.regressions;
    if (a.regressions) example("後退", a.reg_before, a.reg_after);
    Run b = type_sentence(c, keys1, k.golds, 0, seed);
    if ((out.n1 = b.commit != a.commit)) example("ん1打", a.commit, b.commit);
    Run w = type_sentence(c, keys, k.golds, 1, seed);
    if ((out.wait = w.commit != a.commit)) example("待ち", a.commit, w.commit);
    Run t = type_sentence(c, keys, k.golds, 2, seed);
    if ((out.typo = t.commit != a.commit)) example("打ち直し", a.commit, t.commit);
    Run s = type_sentence(c, keys, k.golds, 3, seed);
    if (!(out.space_ok = s.rank == 0)) example("空白", gold, s.commit);
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> f;
    for (size_t a = 0;;) {
        size_t b = s.find(sep, a);
        f.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // 引数: <lex> <model> <cases.tsv> [--out 書き出しの頭] [--jobs N] [--keep N] [--gold-alts]
    std::vector<std::string> pos;
    std::string out_prefix;
    int jobs = int(std::max(1u, std::thread::hardware_concurrency()));
    bool gold_alts = false;   // 正解の欄を空白で区切って複数の正解とみなす (AJIMEE の形)
    int keep = 32;            // 固定せずに変換に渡す字数。IME は PC の速さを測って 8〜32 に決めるが、それだと負荷で結果が揺れるので決め打ち (速い PC は 32)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_prefix = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = std::max(1, atoi(argv[++i]));
        else if (a == "--gold-alts") gold_alts = true;
        else if (a == "--keep" && i + 1 < argc) keep = std::max(1, atoi(argv[++i]));
        else pos.push_back(a);
    }
    if (pos.size() < 3) { printf("usage: sim_user <lex> <model> <cases.tsv> [--out prefix] [--jobs N] [--keep N] [--gold-alts]\n"); return 2; }
    auto lex = slurp(pos[0].c_str());
    auto model = slurp(pos[1].c_str());
    std::vector<Case> cases;
    {
        std::ifstream in(pos[2], std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto f = split(line, '\t');
            if (f.size() < 4 || f[2].empty() || f[3].empty()) continue;
            Case k{f[0], from_utf8(f[1]), from_utf8(f[2]), {}};
            for (auto& g : gold_alts ? split(f[3], ' ') : std::vector<std::string>{f[3]})
                if (!g.empty()) k.golds.push_back(from_utf8(g));
            if (!k.golds.empty()) cases.push_back(std::move(k));
        }
    }
    // 文を jobs 本で分ける。変換のエンジンと Composer は 1 本ずつ (辞書とモデルの中身は共有して読むだけ)
    std::vector<Result> res(cases.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> open_failed{false};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (int j = 0; j < jobs; j++)
        pool.emplace_back([&] {
            Converter conv;
            if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) { open_failed = true; return; }
            u16 ctx;
            Composer c(&conv, nullptr, nullptr);   // 学習なし (打ち方の違いだけを見る)
            c.set_context_provider([&ctx] { return ctx; });
            c.set_keep(size_t(keep));
            for (size_t i; (i = next++) < cases.size();) {
                simulate(c, ctx, cases[i], res[i]);
                if (i % 500 == 0) fprintf(stderr, "%zu / %zu\n", i, cases.size());
            }
        });
    for (auto& t : pool) t.join();
    if (open_failed) { printf("engine open failed\n"); return 2; }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    int n = 0, skipped = 0, kana_bad = 0, exact = 0, loose_ok = 0, reg_sent = 0, reg_total = 0, n1 = 0, wait = 0, typo = 0, space_ok = 0;
    std::vector<double> lat;
    FILE* ex = out_prefix.empty() ? nullptr : fopen((out_prefix + "_examples.tsv").c_str(), "wb");
    FILE* per = out_prefix.empty() ? nullptr : fopen((out_prefix + "_cases.tsv").c_str(), "wb");
    if (ex) fprintf(ex, "name\tkind\tgold\tgot\n");
    if (per) fprintf(per, "name\texact\tloose\tregressions\tn1_diff\twait_diff\ttypo_diff\tspace_ok\tmax_ms\n");
    for (size_t i = 0; i < cases.size(); i++) {
        const Result& r = res[i];
        if (ex) for (auto& e : r.examples) fprintf(ex, "%s\n", e.c_str());
        if (!r.done) { skipped++; continue; }
        if (r.kana_bad) { kana_bad++; continue; }
        n++;
        exact += r.exact;
        loose_ok += r.loose_ok;
        reg_sent += r.regressions > 0;
        reg_total += r.regressions;
        n1 += r.n1;
        wait += r.wait;
        typo += r.typo;
        space_ok += r.space_ok;
        lat.push_back(r.max_ms);
        if (per) fprintf(per, "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.1f\n", cases[i].name.c_str(), r.exact, r.loose_ok, r.regressions, r.n1, r.wait, r.typo, r.space_ok, r.max_ms);
    }
    if (ex) fclose(ex);
    if (per) fclose(per);
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat.empty() ? 0.0 : lat[size_t(p * double(lat.size() - 1))]; };
    const double d = std::max(1, n) / 100.0;
    printf("文 %d (打てない字で飛ばした %d・打鍵が読みにならない %d)  %.0f 秒・%d 本\n", n, skipped, kana_bad, secs, jobs);
    printf("一致 %d (%.1f%%)  ゆるい一致 %d (%.1f%%)\n", exact, exact / d, loose_ok, loose_ok / d);
    printf("後退 %d 文 (%.1f%%)・計 %d 回\n", reg_sent, reg_sent / d, reg_total);
    printf("打ち方で確定が変わった: ん1打 %d・手が止まってから %d・打ち直し %d\n", n1, wait, typo);
    printf("空白で変換して 1 位が正解: %d (%.1f%%)\n", space_ok, space_ok / d);
    printf("1 打の最大 (文ごと): 中央 %.1f ms・95%% %.1f ms・最悪 %.1f ms (並列に回すと遅く出る)\n", pct(0.5), pct(0.95), lat.empty() ? 0.0 : lat.back());
    return 0;
}

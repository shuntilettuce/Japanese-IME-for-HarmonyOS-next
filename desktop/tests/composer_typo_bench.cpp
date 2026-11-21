// QWERTY のローマ字の打ち間違いの直しを、アプリと同じ組み立て (Composer) で測る (2026-11-15)。
//   composer_typo_bench.exe <kkc_lex.bin> <kkc_model.bin> <問題.tsv> <作業用のフォルダ> [件数] [種]
// 問題.tsv: 種類<TAB>左の文脈<TAB>読み<TAB>正解 (layout_typo_bench と同じ)。読みをローマ字にして 1 か所打ち間違え
// (隣のキー・抜け・余分な隣のキー・入れ替え)、Space → Enter で確定した文が、打ち間違えずに打ったときと同じになるか。
// off = 物理キーの打ち直しなし (今までのローマ字の打ち直し)、on = あり (Composer::options.typo_keys)。
// 読みに英字が残る打ち間違い (ありgsとう) は別に数える (キーの並びの直しの所)
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "../core/composer.h"
#include "../core/romaji.h"
#include "../core/text.h"

using namespace shunti;
namespace fs = std::filesystem;

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string neighbors(char c) {
    static const char* const ROWS[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    std::string out;
    for (int r = 0; r < 3; r++) {
        const std::string row = ROWS[r];
        const size_t j = row.find(c);
        if (j == std::string::npos) continue;
        auto put = [&](int rr, long k) { const std::string o = ROWS[rr]; if (k >= 0 && size_t(k) < o.size()) out += o[size_t(k)]; };
        put(r, long(j) - 1); put(r, long(j) + 1);
        if (r > 0) { put(r - 1, long(j)); put(r - 1, long(j) + 1); }
        if (r < 2) { put(r + 1, long(j) - 1); put(r + 1, long(j)); }
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: composer_typo_bench lex model cases.tsv workdir [n] [seed]\n"); return 2; }
    const int N = argc > 5 ? atoi(argv[5]) : 500;
    std::mt19937 rng(argc > 6 ? unsigned(atoi(argv[6])) : 1);
    const std::string lex = slurp(argv[1]), model = slurp(argv[2]);
    Converter conv;
    if (getenv("KKC_MID2")) conv.set_latin_mid2(true);   // 網 mid2 で学んだモデル (S9 など)
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) { fprintf(stderr, "engine open failed\n"); return 2; }
    fs::create_directories(argv[4]);
    Learning learning(fs::path(argv[4]) / "learned.json");
    UserDict dict(fs::path(argv[4]) / "userdict.json");
    struct Case { u16 ctx, kana; };
    std::vector<Case> cases;
    {
        const std::string t = slurp(argv[3]);
        size_t i = 0;
        while (i < t.size()) {
            size_t j = t.find('\n', i);
            if (j == std::string::npos) j = t.size();
            const std::string line = t.substr(i, j - i);
            i = j + 1;
            const size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
            cases.push_back({from_utf8(line.substr(a + 1, b - a - 1)), from_utf8(line.substr(b + 1, c - b - 1))});
        }
    }
    auto run = [&](const Case& c, const std::string& typed, bool on, u16* reading) {
        Composer s(&conv, &learning, &dict);
        s.options.private_mode = true;
        s.options.typo_keys = on;
        const u16 ctx = c.ctx;
        s.set_context_provider([ctx] { return ctx; });
        for (char ch : typed) {
            KeyEvent ev;
            ev.key = Key::Char;
            ev.ch = char16_t(ch);
            s.press(ev);
        }
        if (reading) *reading = s.view().text;
        KeyEvent sp, en;
        sp.key = Key::Space;
        en.key = Key::Enter;
        s.press(sp);
        s.press(en);
        return s.take_commit();
    };
    Romaji rom;
    int n[2] = {}, off_ok[2] = {}, on_ok[2] = {}, clean_chg = 0, clean_n = 0;
    double ms_off = 0, ms_on = 0;
    for (size_t ci = 0; ci < cases.size() && n[0] + n[1] < N; ci++) {
        const Case& c = cases[(ci * 7919) % cases.size()];
        bool kana = true;
        for (char16_t ch : c.kana) kana = kana && ch >= 0x3041 && ch <= 0x30FA;
        if (!kana) continue;
        const u16 r16 = rom.to_romaji(c.kana);
        std::string clean;
        for (char16_t ch : r16) { if (ch >= 0x80) { clean.clear(); break; } clean += char(ch); }
        if (clean.size() < 4) continue;
        // 打ち間違い 1 つ
        std::string t = clean;
        const size_t i = std::uniform_int_distribution<size_t>(0, clean.size() - 1)(rng);
        const int op = std::uniform_int_distribution<int>(0, 9)(rng);
        const std::string nb = neighbors(clean[i]);
        if (op < 4) { if (nb.empty()) continue; t[i] = nb[rng() % nb.size()]; }
        else if (op < 6) t.erase(i, 1);
        else if (op < 8) { if (nb.empty()) continue; t.insert(i + (rng() & 1), 1, nb[rng() % nb.size()]); }
        else { if (i + 1 >= t.size() || t[i] == t[i + 1]) continue; std::swap(t[i], t[i + 1]); }
        const u16 base = run(c, clean, false, nullptr);
        // 打ち間違いの無い文: 打ち直しを渡しても変わらないか
        clean_n++;
        if (run(c, clean, true, nullptr) != base) {
            clean_chg++;
            if (getenv("CT_SHOW")) fprintf(stderr, "変化 %s → %s\n", to_utf8(base).c_str(), to_utf8(run(c, clean, true, nullptr)).c_str());
        }
        u16 rd;
        auto t0 = std::chrono::steady_clock::now();
        const u16 off = run(c, t, false, &rd);
        auto t1 = std::chrono::steady_clock::now();
        const u16 on = run(c, t, true, nullptr);
        auto t2 = std::chrono::steady_clock::now();
        ms_off += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms_on += std::chrono::duration<double, std::milli>(t2 - t1).count();
        bool latin = false;
        for (char16_t ch : rd) latin = latin || (ch < 0x80 && ((ch | 0x20) >= u'a' && (ch | 0x20) <= u'z'));
        const int k = latin ? 1 : 0;
        n[k]++;
        off_ok[k] += off == base;
        on_ok[k] += on == base;
        if (getenv("CT_SHOW") && latin && (off == base) != (on == base))
            fprintf(stderr, "英字 %s: off %s / on %s (正 %s)\n", t.c_str(), to_utf8(off).c_str(), to_utf8(on).c_str(), to_utf8(base).c_str());
    }
    auto pct = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
    printf("かなだけになる打ち間違い %d: off %.1f%% → on %.1f%%\n", n[0], pct(off_ok[0], n[0]), pct(on_ok[0], n[0]));
    printf("英字が残る打ち間違い %d: off %.1f%% → on %.1f%%\n", n[1], pct(off_ok[1], n[1]), pct(on_ok[1], n[1]));
    printf("打ち間違いの無い文 %d: 確定した文が変わった %d\n", clean_n, clean_chg);
    printf("時間 (打って確定まで、1 文): off %.1f ms / on %.1f ms\n", ms_off / (n[0] + n[1]), ms_on / (n[0] + n[1]));
    return 0;
}

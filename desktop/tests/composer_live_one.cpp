// 1 文をキーごとに打って、ライブ表示 (live_display) の見え方を 1 打ごとに出す (2026-11-15、打ち間違いの直しの調べ用)
//   composer_live_one.exe <lex> <model> <work> <keys> [typo_keys 0/1] [variants.tsv]
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "../core/composer.h"
#include "../core/text.h"
using namespace shunti;
namespace fs = std::filesystem;
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string lex = slurp(argv[1]), model = slurp(argv[2]);
    Converter conv;
    if (getenv("KKC_MID2")) conv.set_latin_mid2(true);
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) return 2;
    if (argc > 6) { const std::string v = slurp(argv[6]); conv.set_variants(v); }
    fs::create_directories(argv[3]);
    Learning learning(fs::path(argv[3]) / "learned.json");
    UserDict dict(fs::path(argv[3]) / "userdict.json");
    Composer s(&conv, &learning, &dict);
    s.options.private_mode = true;
    s.options.live_display = true;
    s.options.live_commit = true;
    s.options.typo_keys = argc > 5 ? atoi(argv[5]) != 0 : true;
    for (char ch : std::string(argv[4])) {
        KeyEvent ev; ev.key = Key::Char; ev.ch = char16_t(ch);
        s.press(ev);
        s.flush_live();
        printf("%c  %s\n", ch, to_utf8(s.view().text).c_str());
    }
    KeyEvent en; en.key = Key::Enter; s.press(en);
    printf("確定 %s\n", to_utf8(s.take_commit()).c_str());
}

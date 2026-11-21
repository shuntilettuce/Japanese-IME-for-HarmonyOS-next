// キーの並びを Composer に打って Space → Enter で確定した文を出す。左の文脈つき (2026-11-15、IME ベンチの JA-CTX の調べ用)
//   composer_ctx_run.exe <lex> <model> <work> <問題.tsv: id<TAB>文脈<TAB>キー> [variants.tsv]  → id<TAB>確定した文
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) return 2;
    if (argc > 5) conv.set_variants(slurp(argv[5]));
    fs::create_directories(argv[3]);
    Learning learning(fs::path(argv[3]) / "learned.json");
    UserDict dict(fs::path(argv[3]) / "userdict.json");
    // PERSONAL=1: アプリと同じく学習・書き方の癖・AI学習 (作業用のフォルダの learned.json・style.json・ai_learn_*.bin) をつなぐ
    const bool personal = getenv("PERSONAL") && *getenv("PERSONAL") == '1';
    StyleLog style(fs::path(argv[3]) / "style.json");
    if (personal) conv.set_ai_learn(fs::path(argv[3]) / "ai_learn.bin");
    std::istringstream in(slurp(argv[4]));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        const u16 ctx = from_utf8(line.substr(a + 1, b - a - 1));
        Composer s(&conv, &learning, &dict);
        s.options.private_mode = !personal;
        if (personal) s.set_style_log(&style);
        s.set_context_provider([ctx] { return ctx; });
        for (char ch : line.substr(b + 1)) { KeyEvent ev; ev.key = Key::Char; ev.ch = char16_t(ch); s.press(ev); }
        KeyEvent sp, en; sp.key = Key::Space; en.key = Key::Enter;
        s.press(sp); s.press(en);
        printf("%s\t%s\n", line.substr(0, a).c_str(), to_utf8(s.take_commit()).c_str());
    }
}

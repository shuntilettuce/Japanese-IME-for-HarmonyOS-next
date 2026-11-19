// 読み<TAB>表記 の各行が、読みから書けるか (Converter::path_pos が経路を返すか) を出す (2026-11-15、学習の記録の点検用)
//   path_ok_check.exe <lex> <model> <pairs.tsv>  → 書けない行だけ出す
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include "../core/converter.h"
#include "../core/text.h"
using namespace shunti;
static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string lex = slurp(argv[1]), model = slurp(argv[2]);
    Converter conv;
    if (!conv.open(lex.data(), lex.size(), model.data(), model.size(), 1)) return 2;
    std::istringstream in(slurp(argv[3]));
    std::string line;
    int n = 0, bad = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t');
        if (a == std::string::npos) continue;
        n++;
        if (conv.path_pos(from_utf8(line.substr(0, a)), from_utf8(line.substr(a + 1))).empty()) { bad++; printf("%s\n", line.c_str()); }
    }
    fprintf(stderr, "%d 件中 書けない %d\n", n, bad);
}

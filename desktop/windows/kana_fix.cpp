// 更新のときに 1 度だけ走る、変換の学習の直し (install.ps1 が呼ぶ)。
// 以前はかなのままの確定を数えなかったので、1 度選んだ漢字が短い読みの 1 位を取り続けていた。
// モデルが読みだけでかなを出す読みは、漢字の回数を下げて順位を AI に戻す (Learning::release_kana)。消す項目は無く、手で 1 回選べば戻る。
//   shunti_kana_fix.exe [学習のフォルダ]   (省略すると %APPDATA%\shunti IME)
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>
#include "../core/converter.h"
#include "../core/store.h"
using namespace shunti;
namespace fs = std::filesystem;

static std::vector<char> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    fs::path dir;
    if (cmd && *cmd) {
        dir = cmd;
    } else {
        PWSTR app = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) dir = fs::path(app) / L"shunti IME";
        CoTaskMemFree(app);
    }
    if (dir.empty()) return 1;
    const fs::path learned = dir / L"learned.json", done = dir / L"kana_fix.done";
    std::error_code ec;
    if (!fs::exists(learned, ec) || fs::exists(done, ec)) return 0;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path here = fs::path(self).parent_path();
    auto lex = slurp(here / L"kkc_lex.bin"), model = slurp(here / L"kkc_model.bin");
    Converter conv;
    if (lex.empty() || model.empty() || !conv.open(lex.data(), lex.size(), model.data(), model.size(), 2)) return 1;

    fs::copy_file(learned, dir / L"learned.json.pre-kana", fs::copy_options::overwrite_existing, ec);
    if (ec) return 1;
    Learning learning(learned);
    learning.release_kana([&](const u16& reading) {
        Conversion c = conv.convert(u16(), reading, 1);
        return !c.cands.empty() && c.cands[0] == reading;
    });
    std::ofstream(done, std::ios::binary).put('\n');
    return 0;
}

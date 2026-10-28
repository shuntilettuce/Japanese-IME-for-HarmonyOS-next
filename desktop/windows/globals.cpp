#include "globals.h"

#include <shlobj.h>

#include <mutex>

namespace shunti {
namespace win {

HINSTANCE g_inst = nullptr;
static LONG g_refs = 0;

void dll_add_ref() { InterlockedIncrement(&g_refs); }
void dll_release() { InterlockedDecrement(&g_refs); }
LONG dll_refs() { return g_refs; }

std::filesystem::path module_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_inst, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path user_dir() {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) out = std::filesystem::path(p) / L"shunti IME";
    CoTaskMemFree(p);
    return out;
}

namespace {
// ファイルを読み取り専用でメモリに写す (閉じない。プロセスが終わるまで使う)
const void* map_file(const std::filesystem::path& p, size_t& size) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz;
    const void* view = nullptr;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0) {
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (m) {
            view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(m);
            size = size_t(sz.QuadPart);
        }
    }
    CloseHandle(f);
    return view;
}
}  // namespace

Core::Core()
    : learning(user_dir() / L"learned.json"), dict(user_dir() / L"userdict.json"), settings(user_dir() / L"settings.json"),
      style(user_dir() / L"style.json") {
    dir_ = module_dir();
    flow.load(user_dir() / L"flow.tsv");
    // 計測用: 別のフォルダの辞書とモデルを使う (同じ DLL のまま、モデルを替えて比べるため)
    if (const wchar_t* d = _wgetenv(L"SHUNTI_DATA_DIR"); d && *d) dir_ = d;
    lex_ = map_file(dir_ / L"kkc_lex.bin", lex_size_);
    settings.refresh();
    use_model(settings.get().model);
    // エンジンの作業用のスレッドを持つので、プロセスが終わるまで DLL を外さない
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&core), &self);
}

void Core::use_model(const std::string& name) {
    if (!lex_) return;
    std::string file = model_file_name(name);
    std::error_code ec;
    if (!std::filesystem::exists(dir_ / file, ec)) file = model_file_name("standard");
    if (file == model_file_) return;
    size_t ms = 0;
    const void* model = map_file(dir_ / file, ms);
    if (!model) return;
    if (!conv.open(lex_, lex_size_, model, ms, 2)) {
        UnmapViewOfFile(model);
        return;
    }
    if (model_) UnmapViewOfFile(model_);   // 前のモデル (エンジンは開き直したので、もう使っていない)
    model_ = model;
    model_file_ = file;
    // 書き方の対の表 (kkc_variants.tsv は辞書と同じフォルダ)。書き方の癖はどのモデルでも使う
    // (網に書き方の違う候補を足すのは、個人化を知っている M5 系のモデルのときだけ。engine.cpp の kkc_convert)
    std::string tsv;
    if (read_file(dir_ / L"kkc_variants.tsv", tsv)) conv.set_variants(tsv);
    // 書き方の癖: 文の種類 (丁寧・崩れ) の判定器 (kkc_register.bin も同じフォルダ。無ければ文の種類を見ずに数える)
    std::string reg;
    if (read_file(dir_ / L"kkc_register.bin", reg)) conv.set_register(reg);
}

Core& core() {
    static Core* c = nullptr;
    static std::once_flag once;
    std::call_once(once, [] { c = new Core(); });   // 閉じない (プロセスが終わるまで使う)
    return *c;
}

static bool reg_dark(const wchar_t* value) {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", value,
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}
bool apps_dark() { return reg_dark(L"AppsUseLightTheme"); }
bool system_dark() { return reg_dark(L"SystemUsesLightTheme"); }

}  // namespace win
}  // namespace shunti

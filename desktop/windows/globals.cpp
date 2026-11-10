#include "globals.h"

#include <shellapi.h>
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
    conv.set_latin_mid2(file == "kkc_model_m5.bin");   // M5 は網 mid2 で学んでいる
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

void open_clip_history() {
    HWND target = GetFocus();   // このスレッド (入力欄のアプリ) で今フォーカスのある窓
    if (!target) target = GetForegroundWindow();
    if (target) target = GetAncestor(target, GA_ROOT);
    AllowSetForegroundWindow(ASFW_ANY);   // 常駐している履歴の窓が前に出られるように
    const std::wstring exe = (module_dir() / L"shunti_clip.exe").wstring();
    const std::wstring args = L"--show " + std::to_wstring(reinterpret_cast<UINT_PTR>(target));
    ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

void start_clip_listener() {
    static LONG tried = 0;   // プロセスに 1 回だけ
    if (InterlockedExchange(&tried, 1)) return;
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\shunti_clip")) { CloseHandle(m); return; }   // もう常駐している
    // 管理者として動くアプリ・サンドボックス (AppContainer) の中からは起動しない (記録するのは普通の権限の 1 つだけ)
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION el{};
        DWORD ac = 0, n = 0;
        const bool elevated = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n) && el.TokenIsElevated;
        const bool container = GetTokenInformation(tok, TokenIsAppContainer, &ac, sizeof ac, &n) && ac;
        CloseHandle(tok);
        if (elevated || container) return;
    }
    std::wstring cmd = L"\"" + (module_dir() / L"shunti_clip.exe").wstring() + L"\" --listen";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // 起動したアプリが閉じても残るように、アプリのジョブから外して起動する (外せなければそのまま)
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
bool system_dark() { return reg_dark(L"SystemUsesLightTheme"); }

}  // namespace win
}  // namespace shunti

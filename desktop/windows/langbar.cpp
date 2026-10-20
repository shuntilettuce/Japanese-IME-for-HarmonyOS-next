#include "langbar.h"

#include <shellapi.h>

#include <string>

#include "globals.h"
#include "strings.h"
#include "text_service.h"

namespace shunti {
namespace win {

namespace {
constexpr DWORD SINK_COOKIE = 0x5348;   // 流し先は 1 つだけ

// Zori-chan のアイコン (オン = 起きている、オフ = 寝ている)。タスクバーがダークなら白、ライトなら黒。
// .ico には 16〜256px が入っているので、いまの表示倍率に合う大きさを選んで読む
HICON load_mode_icon(bool open) {
    int id = open ? (system_dark() ? 201 : 202) : (system_dark() ? 203 : 204);
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    return static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

// 設定画面 (この DLL と同じフォルダの shunti_settings.exe) を開く
void open_settings() {
    std::wstring exe = (module_dir() / L"shunti_settings.exe").wstring();
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}  // namespace


LangBarButton::LangBarButton(TextService* ts) : ts_(ts) { dll_add_ref(); }
LangBarButton::~LangBarButton() { dll_release(); }

STDMETHODIMP LangBarButton::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) || IsEqualIID(riid, IID_ITfLangBarItemButton))
        *ppv = static_cast<ITfLangBarItemButton*>(this);
    else if (IsEqualIID(riid, IID_ITfSource))
        *ppv = static_cast<ITfSource*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) LangBarButton::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
STDMETHODIMP_(ULONG) LangBarButton::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return ULONG(n);
}

STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_INVALIDARG;
    info->clsidService = CLSID_TextService;
    info->guidItem = GUID_LBI_INPUTMODE;   // タスクバーの入力モードの表示になる
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    info->ulSort = 0;
    wcscpy_s(info->szDescription, T(131));
    return S_OK;
}
STDMETHODIMP LangBarButton::GetStatus(DWORD* status) {
    if (!status) return E_INVALIDARG;
    *status = 0;
    return S_OK;
}
STDMETHODIMP LangBarButton::GetTooltipString(BSTR* tip) {
    if (!tip) return E_INVALIDARG;
    *tip = SysAllocString(ts_ && ts_->is_open() ? T(132) : T(133));
    return *tip ? S_OK : E_OUTOFMEMORY;
}
// メニューを出すための見えない窓 (この IME を動かしているスレッドのもの。TrackPopupMenu は同じスレッドの窓が要る)
HWND menu_owner() {
    static thread_local HWND h = nullptr;
    if (!h || !IsWindow(h))
        h = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    return h;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick click, POINT pt, const RECT*) {
    if (click == TF_LBI_CLK_LEFT && ts_) {
        ts_->set_open(!ts_->is_open());
        return S_OK;
    }
    if (click != TF_LBI_CLK_RIGHT) return S_OK;
    // 右クリック: 小さなメニュー (オン・オフと設定)
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, ts_ && ts_->is_open() ? T(58) : T(57));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, T(134));
    HWND owner = menu_owner();
    SetForegroundWindow(owner);   // こうしないと、外をクリックしてもメニューが閉じない
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == 1 && ts_) ts_->set_open(!ts_->is_open());
    if (cmd == 2) open_settings();
    return S_OK;
}
// タスクバーのアイコンを右クリックすると、Windows がここで中身を頼んできて、Windows 自身がメニューを出す
enum { MENU_TOGGLE = 1, MENU_SETTINGS = 2 };

STDMETHODIMP LangBarButton::InitMenu(ITfMenu* menu) {
    if (!menu) return E_INVALIDARG;
    const wchar_t* toggle = ts_ && ts_->is_open() ? T(58) : T(57);
    menu->AddMenuItem(MENU_TOGGLE, 0, nullptr, nullptr, toggle, ULONG(wcslen(toggle)), nullptr);
    menu->AddMenuItem(UINT(-1), TF_LBMENUF_SEPARATOR, nullptr, nullptr, L"", 0, nullptr);
    const wchar_t* settings = T(134);
    menu->AddMenuItem(MENU_SETTINGS, 0, nullptr, nullptr, settings, ULONG(wcslen(settings)), nullptr);
    return S_OK;
}

STDMETHODIMP LangBarButton::OnMenuSelect(UINT id) {
    if (id == MENU_TOGGLE && ts_) ts_->set_open(!ts_->is_open());
    if (id == MENU_SETTINGS) open_settings();
    return S_OK;
}

STDMETHODIMP LangBarButton::GetIcon(HICON* icon) {
    if (!icon) return E_INVALIDARG;
    *icon = load_mode_icon(ts_ && ts_->is_open());
    return *icon ? S_OK : E_FAIL;
}
STDMETHODIMP LangBarButton::GetText(BSTR* text) {
    if (!text) return E_INVALIDARG;
    *text = SysAllocString(ts_ && ts_->is_open() ? T(135) : L"A");
    return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) {
    if (!IsEqualIID(riid, IID_ITfLangBarItemSink)) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    if (FAILED(sink->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&sink_)))) { sink_ = nullptr; return E_NOINTERFACE; }
    *cookie = SINK_COOKIE;
    return S_OK;
}
STDMETHODIMP LangBarButton::UnadviseSink(DWORD cookie) {
    if (cookie != SINK_COOKIE || !sink_) return CONNECT_E_NOCONNECTION;
    sink_->Release();
    sink_ = nullptr;
    return S_OK;
}

void LangBarButton::update() {
    if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

}  // namespace win
}  // namespace shunti

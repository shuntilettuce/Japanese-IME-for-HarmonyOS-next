// shunti IME の設定 (shunti_settings.exe)。Windows の標準の部品だけで作った小さな窓。
// 変えるとすぐ %APPDATA%\shunti IME\settings.json に書き、IME は次に打ち始めたときに読み直す。
// ユーザー辞書 (userdict.json) の登録・削除と、学習 (learned.json) のリセットもここで行う。
#include "update.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <fstream>
#include <iterator>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/flow.h"
#include "../core/settings.h"
#include "../core/store.h"
#include "../core/text.h"
#include "shortcut_key.h"
#include "strings.h"

using namespace shunti;
using namespace shunti::win;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t VERSION[] = L"0.8.2";

enum Id {
    ID_PUNCT = 100, ID_SPACE, ID_DIGITS, ID_LIVE, ID_LIVEDISPLAY, ID_LIVECOMMIT, ID_SUGGEST,
    ID_ADVANCED, ID_UPDATE, ID_RESET, ID_RECENT_SPAN, ID_RECENT_FORGET, ID_RECENT_SPAN_AI, ID_RECENT_FORGET_AI, ID_LEARN_LOG, ID_LEARN_CLEAR, ID_LICENSE, ID_GITHUB, ID_MODEL, ID_DICT, ID_CANDTREE, ID_SPACELIT, ID_LATIN, ID_CAPS, ID_TYPO, ID_PUNCTCOMMIT, ID_AILEARN, ID_CLIPHIST, ID_LOG_CUT, ID_DICTMGR,
};

// 変換モデル (settings.json の model の値と、画面の表示)。名前は配布の呼び名 (開発名は出さない)
std::vector<std::string> g_models = {"light", "standard", "s2", "m5", "pure"};   // 変換モデルの欄の並び

struct PosItem { const wchar_t* label; const char* pos; const char* group; };
// 品詞は MS-IME の分類にそろえる (MS-IME の一覧の出力をそのまま読み込めるように)
const std::vector<PosItem>& pos_items() {
    static const std::vector<PosItem> POS = {
        {T(1), "noun", ""}, {T(164), "latin", ""}, {T(2), "abbrev", ""}, {T(3), "kaomoji", ""}, {T(4), "person", ""},
        {T(5), "place", ""}, {T(6), "org", ""},
        {T(7), "verb", "godan"}, {T(8), "verb", "ichidan"}, {T(9), "adjective", ""},
        {T(10), "adverb", ""}, {T(11), "interjection", ""},
    };
    return POS;
}

fs::path g_dir;
Settings g_s;
UserDict* g_dict = nullptr;
HWND g_wnd = nullptr;
HFONT g_font = nullptr;
UINT g_dpi = 96;
bool g_loading = false;

int S(int v) { return MulDiv(v, int(g_dpi), 96); }
std::wstring W(const u16& s) { return std::wstring(s.begin(), s.end()); }
u16 U(const std::wstring& s) { return u16(s.begin(), s.end()); }
std::wstring W8(const std::string& s) { return W(from_utf8(s)); }

std::wstring text_of(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(size_t(n));
    return s;
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(g_font), TRUE);
    return c;
}

void group(const wchar_t* title, int y, int h) { make(L"BUTTON", title, BS_GROUPBOX, 10, y, 460, h, 0); }

HWND combo(int x, int y, int w, int id, std::initializer_list<const wchar_t*> items) {
    HWND c = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
    for (auto it : items) SendMessageW(c, CB_ADDSTRING, 0, LPARAM(it));
    return c;
}

void check(int id, bool on) { SendDlgItemMessageW(g_wnd, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
bool checked(int id) { return SendDlgItemMessageW(g_wnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void select(int id, int i) { SendDlgItemMessageW(g_wnd, id, CB_SETCURSEL, WPARAM(i), 0); }
int selected(int id) { return int(SendDlgItemMessageW(g_wnd, id, CB_GETCURSEL, 0, 0)); }

void save() {
    if (g_loading) return;
    g_s.punct = std::max(0, selected(ID_PUNCT));
    g_s.space_fullwidth = checked(ID_SPACE);
    g_s.digits_fullwidth = checked(ID_DIGITS);
    g_s.live = checked(ID_LIVE);
    g_s.live_display = selected(ID_LIVEDISPLAY) > 0;
    g_s.live_wait = selected(ID_LIVEDISPLAY) == 1;
    g_s.cand_tree = checked(ID_CANDTREE);
    g_s.space_literal = checked(ID_SPACELIT);
    g_s.latin_auto = checked(ID_LATIN);
    g_s.caps_alpha = checked(ID_CAPS);
    g_s.typo_fix = checked(ID_TYPO);
    g_s.punct_commit = checked(ID_PUNCTCOMMIT);
    g_s.clip_history = checked(ID_CLIPHIST);
    g_s.ai_learn = checked(ID_AILEARN);
    g_s.live_commit = checked(ID_LIVECOMMIT);
    g_s.model = g_models[size_t(std::clamp(selected(ID_MODEL), 0, int(g_models.size()) - 1))];
    // PureXS2 の間は欄を止めてクリーンを見せているだけなので、前に選んだ辞書を残す
    if (IsWindowEnabled(GetDlgItem(g_wnd, ID_DICT)) && g_s.model != "pure") g_s.dict = selected(ID_DICT) == 1 ? "clean" : "standard";
    if (!save_settings(g_dir / L"settings.json", g_s))
        MessageBoxW(g_wnd, T(12), L"shunti IME", MB_ICONWARNING);
}

std::wstring pos_label(const UserDict::Entry& e) {
    for (auto& p : pos_items())
        if (e.pos == p.pos && (e.pos != "verb" || e.group == p.group || (e.group.empty() && std::string(p.group) == "godan"))) return p.label;
    return W8(e.pos);
}

// ---------------- 名前を入れる小さな窓 (パックの名前。読み込みのときは「パックとして保存する」のチェックつき) ----------------
struct NamePrompt {
    std::wstring title, value;
    bool has_check = false, check = true;   // has_check: 読み込みの確認 (チェックを外すと未収録に入れる)
    const wchar_t* ok = nullptr;
    bool done = false, accepted = false;
};
NamePrompt* g_np = nullptr;
enum { NP_CHECK = 10, NP_EDIT = 11 };   // 1・2 は IDOK・IDCANCEL と重なる (欄が選ばれた知らせで「やめる」になっていた)

LRESULT CALLBACK np_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && g_np) {
        const int id = LOWORD(w);
        if (id == NP_CHECK && HIWORD(w) == BN_CLICKED) {
            EnableWindow(GetDlgItem(h, NP_EDIT), SendDlgItemMessageW(h, NP_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == IDOK) {
            HWND e = GetDlgItem(h, NP_EDIT);
            std::wstring v(size_t(GetWindowTextLengthW(e)) + 1, L'\0');
            v.resize(size_t(GetWindowTextW(e, v.data(), int(v.size()))));
            while (!v.empty() && (v.back() == L' ' || v.back() == 0x3000)) v.pop_back();
            while (!v.empty() && (v[0] == L' ' || v[0] == 0x3000)) v.erase(0, 1);
            g_np->check = !g_np->has_check || SendDlgItemMessageW(h, NP_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_np->check && v.empty()) { MessageBoxW(h, T(272), g_np->title.c_str(), MB_ICONINFORMATION); return 0; }
            g_np->value = v;
            g_np->accepted = g_np->done = true;
        } else if (id == IDCANCEL) {
            g_np->done = true;
        }
        return 0;
    }
    if (m == WM_CLOSE) { if (g_np) g_np->done = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}

// 開いている間は owner を使えなくする。OK なら true (p.value に名前、p.check にチェック)
bool ask_name(HWND owner, NamePrompt& p) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = np_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"ShuntiImeName";
        RegisterClassExW(&wc);
        reg = true;
    }
    const int top = p.has_check ? 34 : 0;
    RECT r = {0, 0, S(400), S(top + 92)};
    AdjustWindowRectExForDpi(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT o = {};
    GetWindowRect(owner, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ShuntiImeName", p.title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (o.left + o.right - ww) / 2, (o.top + o.bottom - wh) / 2, ww, wh, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return false;
    HWND keep = g_wnd;
    g_wnd = h;
    if (p.has_check) {
        make(L"BUTTON", T(257), BS_AUTOCHECKBOX | WS_TABSTOP, 14, 12, 376, 22, NP_CHECK);
        check(NP_CHECK, p.check);
    }
    make(L"STATIC", T(258), 0, 14, top + 17, 90, 20, 0);
    HWND e = make(L"EDIT", p.value.c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 106, top + 14, 280, 23, NP_EDIT);
    make(L"BUTTON", p.ok ? p.ok : T(273), BS_DEFPUSHBUTTON | WS_TABSTOP, 210, top + 54, 86, 25, IDOK);
    make(L"BUTTON", T(249), BS_PUSHBUTTON | WS_TABSTOP, 302, top + 54, 86, 25, IDCANCEL);
    g_wnd = keep;
    EnableWindow(e, p.check);
    g_np = &p;
    p.done = p.accepted = false;
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(e);
    SendMessageW(e, EM_SETSEL, 0, -1);
    MSG msg;
    while (!p.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(0); break; }   // 終わりの知らせは外の繰り返しに渡す
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    g_np = nullptr;
    EnableWindow(owner, TRUE);
    DestroyWindow(h);
    SetForegroundWindow(owner);
    return p.accepted;
}

// ---------------- 辞書の管理 (別の窓): 左にすべて・未収録・パック、右にその語の一覧 ----------------
HWND g_dm = nullptr, g_dm_packs = nullptr, g_dm_list = nullptr;
std::vector<std::string> g_dm_names;             // 左の一覧の 3 行目から (パックの名前)
std::vector<std::string> g_dm_empty;             // 作ったばかりで語がまだないパック (窓を閉じると消える)
std::vector<uint32_t> g_dm_rows;                 // 右の一覧の行 → 辞書の語の番号 (一覧は表示する行だけ字を問い合わせる形。10 万語でも止まらないように)
int g_dm_sel = 0;                                // 0 = すべて、1 = 未収録、2 以降 = パック
std::string g_dm_target;                         // 新規登録の登録先のパック (空 = 未収録)。左でパック・未収録を選ぶとそれに合わせる
enum { DM_PACKS = 700, DM_LIST, DM_READING, DM_WORD, DM_POS, DM_ADD, DM_DEL, DM_MOVETO, DM_MOVE, DM_NEW, DM_RENAME, DM_DROP, DM_IMPORT, DM_FORMAT, DM_EXPORT, DM_TARGET, DM_MOVELBL };

// 今の左の選択に当たるパックの名前。すべてなら null
const std::string* dm_filter() {
    static const std::string none;
    if (g_dm_sel <= 0) return nullptr;
    if (g_dm_sel == 1) return &none;
    return size_t(g_dm_sel - 2) < g_dm_names.size() ? &g_dm_names[size_t(g_dm_sel - 2)] : nullptr;
}

void dm_move_visibility();

void dm_refresh(const std::string* select_pack = nullptr) {
    if (!g_dm) return;
    g_dict->refresh();
    const auto& es = g_dict->entries();
    std::string keep_name = select_pack ? *select_pack : (g_dm_sel >= 2 && size_t(g_dm_sel - 2) < g_dm_names.size() ? g_dm_names[size_t(g_dm_sel - 2)] : "");
    const bool keep_pack = select_pack || g_dm_sel >= 2;
    g_dm_names = g_dict->packs();
    for (auto it = g_dm_empty.begin(); it != g_dm_empty.end();) {   // 語が入ったら普通のパック
        if (std::find(g_dm_names.begin(), g_dm_names.end(), *it) != g_dm_names.end()) it = g_dm_empty.erase(it);
        else g_dm_names.push_back(*it++);
    }
    if (keep_pack) {
        auto f = std::find(g_dm_names.begin(), g_dm_names.end(), keep_name);
        g_dm_sel = f == g_dm_names.end() ? 0 : 2 + int(f - g_dm_names.begin());
    }
    if (select_pack) g_dm_target = *select_pack;   // 作った・読み込んだパックを登録先にも
    // 左の一覧 (語の数つき)。数は 1 回なめて数える
    std::unordered_map<std::string, int> counts;
    for (auto& e : es) counts[e.pack]++;
    SendMessageW(g_dm_packs, LB_RESETCONTENT, 0, 0);
    auto add_row = [&](const std::wstring& s, int n) { SendMessageW(g_dm_packs, LB_ADDSTRING, 0, LPARAM((s + L" (" + std::to_wstring(n) + L")").c_str())); };
    add_row(T(254), int(es.size()));
    add_row(T(255), counts[""]);
    for (auto& k : g_dm_names) add_row(W8(k), counts[k]);
    SendMessageW(g_dm_packs, LB_SETCURSEL, WPARAM(g_dm_sel), 0);
    // 移動先と新規登録の登録先 (未収録 + パック)。登録先は左で選んだパックに合わせる (すべてなら前のまま)
    auto fill = [&](int id, int sel) {
        HWND c = GetDlgItem(g_dm, id);
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        SendMessageW(c, CB_ADDSTRING, 0, LPARAM(T(255)));
        for (auto& k : g_dm_names) SendMessageW(c, CB_ADDSTRING, 0, LPARAM(W8(k).c_str()));
        SendMessageW(c, CB_SETCURSEL, WPARAM(std::clamp(sel, 0, int(g_dm_names.size()))), 0);
    };
    fill(DM_MOVETO, int(SendDlgItemMessageW(g_dm, DM_MOVETO, CB_GETCURSEL, 0, 0)));
    {
        const auto f = std::find(g_dm_names.begin(), g_dm_names.end(), g_dm_target);
        if (f == g_dm_names.end()) g_dm_target.clear();   // パックが消えた・名前が変わった
        fill(DM_TARGET, f == g_dm_names.end() ? 0 : 1 + int(f - g_dm_names.begin()));
    }
    // パックを選んでいるときだけ、名前を変える・消すを使える
    EnableWindow(GetDlgItem(g_dm, DM_RENAME), g_dm_sel >= 2);
    EnableWindow(GetDlgItem(g_dm, DM_DROP), g_dm_sel >= 2);
    // 右の一覧
    const std::string* k = dm_filter();
    static int shown_sel = -1;   // 左の選択が変わったら一覧の頭に戻す
    ListView_SetItemState(g_dm_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    g_dm_rows.clear();
    for (size_t i = 0; i < es.size(); i++)
        if (!k || es[i].pack == *k) g_dm_rows.push_back(uint32_t(i));
    ListView_SetItemCountEx(g_dm_list, int(g_dm_rows.size()), LVSICF_NOSCROLL);
    if (shown_sel != g_dm_sel && !g_dm_rows.empty()) ListView_EnsureVisible(g_dm_list, 0, FALSE);
    shown_sel = g_dm_sel;
    InvalidateRect(g_dm_list, nullptr, FALSE);
    dm_move_visibility();
}

// 一覧の行の字 (表示する行だけ聞かれる)
void dm_dispinfo(NMLVDISPINFOW* di) {
    if (!(di->item.mask & LVIF_TEXT) || !di->item.pszText || di->item.cchTextMax <= 0) return;
    const auto& es = g_dict->entries();
    const size_t row = size_t(di->item.iItem);
    std::wstring t;
    if (row < g_dm_rows.size() && g_dm_rows[row] < es.size()) {
        const auto& e = es[g_dm_rows[row]];
        switch (di->item.iSubItem) {
            case 0: t = W(e.reading); break;
            case 1: t = W(e.word); break;
            case 2: t = pos_label(e); break;
            default: t = e.pack.empty() ? std::wstring(T(255)) : W8(e.pack); break;
        }
    }
    wcsncpy_s(di->item.pszText, size_t(di->item.cchTextMax), t.c_str(), _TRUNCATE);
}

std::vector<std::pair<u16, u16>> dm_selected() {
    std::vector<std::pair<u16, u16>> out;
    const auto& es = g_dict->entries();
    for (int i = ListView_GetNextItem(g_dm_list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(g_dm_list, i, LVNI_SELECTED))
        if (size_t(i) < g_dm_rows.size() && g_dm_rows[size_t(i)] < es.size()) out.push_back({es[g_dm_rows[size_t(i)]].reading, es[g_dm_rows[size_t(i)]].word});
    return out;
}

// パックの移動 (移動先・移す) と削除は、語を選んでいるときだけ使える (それ以外は灰色。2026-11-18 ユーザー)
void dm_move_visibility() {
    const BOOL on = ListView_GetSelectedCount(g_dm_list) > 0;
    for (int id : {DM_MOVELBL, DM_MOVETO, DM_MOVE, DM_DEL}) EnableWindow(GetDlgItem(g_dm, id), on);
}

std::wstring dlg_text(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    std::wstring s(size_t(GetWindowTextLengthW(c)) + 1, L'\0');
    s.resize(size_t(GetWindowTextW(c, s.data(), int(s.size()))));
    return s;
}

void dm_add() {
    const int p = std::max(0, int(SendDlgItemMessageW(g_dm, DM_POS, CB_GETCURSEL, 0, 0)));
    const auto& pi = pos_items()[size_t(p)];
    const std::wstring r = std::string(pi.pos) == "latin" ? L"" : dlg_text(g_dm, DM_READING), w = dlg_text(g_dm, DM_WORD);
    const std::string err = g_dict->add(U(r), U(w), pi.pos, pi.group, g_dm_target);
    if (!err.empty()) { MessageBoxW(g_dm, W8(err).c_str(), T(13), MB_ICONINFORMATION); return; }
    SetDlgItemTextW(g_dm, DM_READING, L"");
    SetDlgItemTextW(g_dm, DM_WORD, L"");
    dm_refresh();
    SetFocus(GetDlgItem(g_dm, std::string(pi.pos) == "latin" ? DM_WORD : DM_READING));
}

void dm_delete() {
    auto items = dm_selected();
    if (items.empty()) return;
    g_dict->remove_many(items);   // 保存は 1 回
    dm_refresh();
}

void dm_move() {
    auto items = dm_selected();
    if (items.empty()) return;
    const int t = int(SendDlgItemMessageW(g_dm, DM_MOVETO, CB_GETCURSEL, 0, 0));
    const std::string to = t >= 1 && size_t(t - 1) < g_dm_names.size() ? g_dm_names[size_t(t - 1)] : "";
    g_dict->move_to_pack(items, to);
    dm_refresh();
}

void dm_new_pack() {
    NamePrompt p;
    p.title = T(261);
    if (!ask_name(g_dm, p)) return;
    const std::string name = to_utf8(U(p.value));
    if (std::find(g_dm_names.begin(), g_dm_names.end(), name) == g_dm_names.end()) g_dm_empty.push_back(name);
    dm_refresh(&name);
}

void dm_rename() {
    const std::string* k = dm_filter();
    if (!k || k->empty()) return;
    const std::string from = *k;
    NamePrompt p;
    p.title = T(262);
    p.value = W8(from);
    if (!ask_name(g_dm, p)) return;
    const std::string to = to_utf8(U(p.value));
    g_dict->rename_pack(from, to);
    for (auto& e : g_dm_empty) if (e == from) e = to;
    dm_refresh(&to);
}

void dm_drop() {
    const std::string* k = dm_filter();
    if (!k || k->empty()) return;
    const std::string name = *k;
    const int r = MessageBoxW(g_dm, T(271), W8(name).c_str(), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (r == IDCANCEL) return;
    g_dict->remove_pack(name, r == IDYES);
    g_dm_empty.erase(std::remove(g_dm_empty.begin(), g_dm_empty.end(), name), g_dm_empty.end());
    g_dm_sel = 0;
    dm_refresh();
}

void dm_export() {
    const int fi = std::clamp(int(SendDlgItemMessageW(g_dm, DM_FORMAT, CB_GETCURSEL, 0, 0)), 0, 2);
    const UserDict::Format f = fi == 1 ? UserDict::Format::Csv : fi == 2 ? UserDict::Format::Mozc : UserDict::Format::MsIme;
    const std::string* k = dm_filter();
    g_dict->refresh();
    int n = 0;
    const std::string b = g_dict->export_bytes(f, k, &n);
    if (n == 0) { MessageBoxW(g_dm, T(274), T(13), MB_ICONINFORMATION); return; }
    // ファイル名の初め: パックならその名前 (ファイル名に使えない字は _ に)
    std::wstring base = k && !k->empty() ? W8(*k) : k ? std::wstring(T(255)) : L"shunti_userdict";
    for (auto& c : base) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const wchar_t* ext = f == UserDict::Format::Csv ? L"csv" : L"txt";
    wchar_t path[MAX_PATH] = L"";
    wcsncpy_s(path, (base + L"." + ext).c_str(), _TRUNCATE);
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_dm;
    of.lpstrFilter = f == UserDict::Format::Csv ? L"CSV (*.csv)\0*.csv\0" : L"テキスト (*.txt)\0*.txt\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = ext;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    std::ofstream o(path, std::ios::binary);
    o.write(b.data(), std::streamsize(b.size()));
    o.close();
    if (!o) { MessageBoxW(g_dm, T(36), T(13), MB_ICONWARNING); return; }
    MessageBoxW(g_dm, (std::to_wstring(n) + T(37)).c_str(), T(13), MB_ICONINFORMATION);
}

void import_words(HWND owner);

LRESULT CALLBACK dm_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == IDCANCEL) DestroyWindow(h);
        else if (id == IDOK) {   // 読み・単語の欄で Enter → 追加
            const HWND f = GetFocus();
            if (f == GetDlgItem(h, DM_READING) || f == GetDlgItem(h, DM_WORD)) dm_add();
        }
        else if (id == DM_PACKS && code == LBN_SELCHANGE) {
            g_dm_sel = std::max(0, int(SendMessageW(g_dm_packs, LB_GETCURSEL, 0, 0)));
            if (const std::string* k = dm_filter()) g_dm_target = *k;   // すべてなら登録先は前のまま
            dm_refresh();
        }
        else if (id == DM_TARGET && code == CBN_SELCHANGE) {
            const int t = int(SendDlgItemMessageW(h, DM_TARGET, CB_GETCURSEL, 0, 0));
            g_dm_target = t >= 1 && size_t(t - 1) < g_dm_names.size() ? g_dm_names[size_t(t - 1)] : "";
        }
        else if (id == DM_POS && code == CBN_SELCHANGE) {
            const int p = std::max(0, int(SendDlgItemMessageW(h, DM_POS, CB_GETCURSEL, 0, 0)));
            const bool latin = std::string(pos_items()[size_t(p)].pos) == "latin";
            if (latin) SetDlgItemTextW(h, DM_READING, L"");
            EnableWindow(GetDlgItem(h, DM_READING), !latin);
        }
        else if (id == DM_ADD) dm_add();
        else if (id == DM_DEL) dm_delete();
        else if (id == DM_MOVE) dm_move();
        else if (id == DM_NEW) dm_new_pack();
        else if (id == DM_RENAME) dm_rename();
        else if (id == DM_DROP) dm_drop();
        else if (id == DM_IMPORT) import_words(h);
        else if (id == DM_EXPORT) dm_export();
        return 0;
    }
    if (m == WM_NOTIFY) {
        auto* nm = reinterpret_cast<NMHDR*>(l);
        if (nm->idFrom == DM_LIST && nm->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey == VK_DELETE) dm_delete();
        if (nm->idFrom == DM_LIST && nm->code == LVN_ITEMCHANGED && (reinterpret_cast<NMLISTVIEW*>(l)->uChanged & LVIF_STATE)) dm_move_visibility();
        if (nm->idFrom == DM_LIST && nm->code == LVN_ODSTATECHANGED) dm_move_visibility();   // Shift で範囲を選んだとき
        if (nm->idFrom == DM_LIST && nm->code == LVN_GETDISPINFOW) dm_dispinfo(reinterpret_cast<NMLVDISPINFOW*>(l));
        return 0;
    }
    if (m == WM_DESTROY) {
        g_dm = nullptr;
        g_dm_empty.clear();
        EnableWindow(g_wnd, TRUE);
        SetForegroundWindow(g_wnd);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void open_dict_manager() {
    if (g_dm) { SetForegroundWindow(g_dm); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = dm_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        wc.lpszClassName = L"ShuntiImeDict";
        RegisterClassExW(&wc);
        reg = true;
    }
    RECT r = {0, 0, S(800), S(500)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
    g_dm = CreateWindowExW(0, L"ShuntiImeDict", T(253), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_dm) return;
    HWND keep = g_wnd;
    g_wnd = g_dm;
    // 左: すべて・未収録・パック と、パックの操作
    g_dm_packs = make(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 10, 10, 200, 380, DM_PACKS);
    make(L"BUTTON", T(261), BS_PUSHBUTTON | WS_TABSTOP, 10, 398, 200, 25, DM_NEW);
    make(L"BUTTON", T(262), BS_PUSHBUTTON | WS_TABSTOP, 10, 429, 97, 25, DM_RENAME);
    make(L"BUTTON", T(263), BS_PUSHBUTTON | WS_TABSTOP, 113, 429, 97, 25, DM_DROP);
    // 右: 語の一覧 (いくつも選べる)
    g_dm_list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 222, 10, 568, 242, DM_LIST);
    ListView_SetExtendedListViewStyle(g_dm_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {T(115), T(116), T(117), T(256)};
    const int widths[] = {140, 160, 130, 120};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = S(widths[i]);
        ListView_InsertColumn(g_dm_list, i, &c);
    }
    // 一覧の下: 選んだ語の削除と、ほかのパックへの移動
    make(L"STATIC", T(264), 0, 222, 266, 140, 20, DM_MOVELBL);
    make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 366, 262, 220, 240, DM_MOVETO);
    make(L"BUTTON", T(265), BS_PUSHBUTTON | WS_TABSTOP, 592, 261, 84, 25, DM_MOVE);
    make(L"BUTTON", T(79), BS_PUSHBUTTON | WS_TABSTOP, 706, 261, 84, 25, DM_DEL);
    // 新規登録 (仕切った欄。登録先のパックも選ぶ)
    make(L"BUTTON", T(276), BS_GROUPBOX, 222, 296, 568, 118, 0);
    make(L"STATIC", T(118), 0, 236, 321, 44, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 282, 318, 190, 23, DM_READING);
    make(L"STATIC", T(119), 0, 486, 321, 44, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 532, 318, 244, 23, DM_WORD);
    make(L"STATIC", T(120), 0, 236, 352, 44, 20, 0);
    HWND pos = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 282, 348, 190, 240, DM_POS);
    for (auto& p : pos_items()) SendMessageW(pos, CB_ADDSTRING, 0, LPARAM(p.label));
    SendMessageW(pos, CB_SETCURSEL, 0, 0);
    make(L"STATIC", T(277), 0, 486, 352, 60, 20, 0);
    make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 548, 348, 140, 240, DM_TARGET);
    make(L"BUTTON", T(278), BS_PUSHBUTTON | WS_TABSTOP, 692, 347, 84, 25, DM_ADD);
    make(L"STATIC", T(279), 0, 236, 384, 540, 20, 0);
    // ファイル: 読み込み (パックとして保存するかを聞く)・書き出し (左で選んでいるもの: すべて・未収録・そのパック)
    make(L"BUTTON", T(121), BS_PUSHBUTTON | WS_TABSTOP, 222, 428, 200, 25, DM_IMPORT);
    make(L"BUTTON", T(266), BS_PUSHBUTTON | WS_TABSTOP, 530, 427, 98, 25, DM_EXPORT);
    combo(634, 428, 156, DM_FORMAT, {T(268), T(269), T(270)});
    SendDlgItemMessageW(g_dm, DM_FORMAT, CB_SETCURSEL, 0, 0);
    make(L"BUTTON", T(71), BS_PUSHBUTTON | WS_TABSTOP, 706, 465, 84, 25, IDCANCEL);
    g_wnd = keep;
    g_dm_sel = 0;
    g_dm_target.clear();
    dm_refresh();
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_dm, SW_SHOW);
    SetFocus(GetDlgItem(g_dm, DM_READING));
}

// まとめて登録: MS-IME の「一覧の出力」の .txt (UTF-16) や、読み,語句,品詞 の CSV (UTF-8 / Shift_JIS)、Google 日本語入力の .txt。
// 読む前に、パックとして保存するか (名前の初めはファイル名、書き出したパックなら書いてある名前) を聞く
void import_words(HWND owner) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = owner;
    of.lpstrFilter = L"辞書の一覧 (*.txt;*.csv;*.tsv)\0*.txt;*.csv;*.tsv\0すべてのファイル\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        MessageBoxW(owner, T(14), T(13), MB_ICONWARNING);
        return;
    }
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::wstring text;
    if (b.size() >= 2 && uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xFE) {   // UTF-16 (MS-IME の出力)
        text.assign(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
    } else {
        size_t off = b.size() >= 3 && uint8_t(b[0]) == 0xEF && uint8_t(b[1]) == 0xBB && uint8_t(b[2]) == 0xBF ? 3 : 0;
        // UTF-8 として読めなければ Shift_JIS
        UINT cp = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data() + off, int(b.size() - off), nullptr, 0) > 0 ? CP_UTF8 : 932;
        int n = MultiByteToWideChar(cp, 0, b.data() + off, int(b.size() - off), nullptr, 0);
        text.resize(size_t(std::max(n, 0)));
        if (n > 0) MultiByteToWideChar(cp, 0, b.data() + off, int(b.size() - off), text.data(), n);
    }
    NamePrompt p;
    p.title = T(259);
    p.has_check = true;
    p.ok = T(260);
    p.value = fs::path(path).stem().wstring();
    const size_t at = text.find(L"!パック: ");   // このアプリで書き出したパック
    if (at != std::wstring::npos) {
        const size_t s = at + 6, e = text.find_first_of(L"\r\n", s);
        if (e != std::wstring::npos && e > s) p.value = text.substr(s, e - s);
    }
    if (!ask_name(owner, p)) return;
    const std::string pack = p.check ? to_utf8(U(p.value)) : "";
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    auto res = g_dict->import_text(U(text), pack);
    SetCursor(old);
    dm_refresh(p.check ? &pack : nullptr);
    std::wstring msg = std::to_wstring(res.first) + T(15);
    if (!res.second.empty()) {
        msg += T(16) + std::to_wstring(res.second.size()) + L"):";
        for (size_t i = 0; i < res.second.size() && i < 12; i++) msg += L"\n" + W8(res.second[i]);
        if (res.second.size() > 12) msg += L"\n…";
    }
    MessageBoxW(owner, msg.c_str(), T(13), MB_ICONINFORMATION);
}

// ---------------- 詳細設定 (別の窓) ----------------
// 記号ごとの全角/半角・テンキー・候補の数・無変換キー・再変換・プライベート・ローマ字の表・辞書の書き出し
const wchar_t SYMS[] = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
enum { ADV_SYM0 = 500, ADV_NUMPAD = 600, ADV_PAGE, ADV_APPOPEN, ADV_PRIVATE, ADV_ROMAJI, ADV_EXPORT, ADV_CLOSE, ADV_TYPOLV, ADV_CTXL, ADV_CTXR,
       ADV_KEYMAP = 710, ADV_LAYOUT = 711, ADV_LAYOUT_NOTE = 712, ADV_LTHUMBS = 713, ADV_LCONT, ADV_LTAP, ADV_LREP, ADV_RLIST = 720, ADV_RKEY, ADV_RKANA, ADV_RADD, ADV_RDEL, ADV_ALIST = 730, ADV_ATOGGLE, ADV_ADEL, ADV_THEME = 740, ADV_ICON, ADV_STRIP,
       ADV_SLIST = 800, ADV_SKEY, ADV_SFUNC, ADV_SADD, ADV_SCHG, ADV_SDEL, ADV_SUP, ADV_SDOWN, ADV_SRESET };
// キー割り当ての型 (Google 日本語入力 = Mozc の keymap と同じ字)
struct Keymap { const char* id; const wchar_t* name; };
const std::vector<Keymap>& keymaps() {
    static const std::vector<Keymap> KEYMAPS = {{"msime", L"MS-IME"}, {"atok", L"ATOK"}, {"kotoeri", T(17)}};
    return KEYMAPS;
}
// キー配列: 置いてある layout_<id>.tsv を全部 (設定のフォルダ → インストール先)。名前・作者・公式ページは表の #! の行から
struct LayoutEntry { std::string id; std::wstring name, note; bool thumbs = false; };   // thumbs: 親指シフトの配列 (表に lthumb・rthumb がある)
HWND g_lgroup = nullptr, g_lthumb_label = nullptr, g_lother = nullptr;
HWND g_alist = nullptr, g_agroup = nullptr;   // 詳細設定のソフトごとの日本語入力の欄   // 詳細設定のキー配列の欄 (並べ直す部品)
int g_lgroup_y = 0;
std::vector<LayoutEntry> g_layouts;
void load_layouts() {
    g_layouts.clear();
    g_layouts.push_back({"", T(18), L""});
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::vector<LayoutEntry> found;
    for (const std::filesystem::path& dir : {g_dir, std::filesystem::path(exe).parent_path()}) {
        std::error_code ec;
        for (auto& de : std::filesystem::directory_iterator(dir, ec)) {
            const std::wstring fn = de.path().filename().wstring();
            if (fn.size() < 12 || fn.compare(0, 7, L"layout_") != 0 || de.path().extension() != L".tsv") continue;
            const std::wstring idw = fn.substr(7, fn.size() - 11);
            const std::string id(idw.begin(), idw.end());
            bool dup = false;
            for (auto& f : found) dup = dup || f.id == id;
            if (dup) continue;   // 設定のフォルダに同じ名前があればそちら
            std::string t;
            if (!read_file(de.path(), t)) continue;
            LayoutEntry e{id, idw, L""};
            std::wstring author, url;
            size_t i = 0;
            while (i < t.size()) {
                size_t j = t.find('\n', i);
                if (j == std::string::npos) j = t.size();
                std::string line = t.substr(i, j - i);
                i = j + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.compare(0, 8, "#! name ") == 0) e.name = W8(line.substr(8));
                else if (line.compare(0, 10, "#! author ") == 0) author = W8(line.substr(10));
                else if (line.compare(0, 7, "#! url ") == 0) url = W8(line.substr(7));
                else if (line.compare(0, 7, "lthumb+") == 0 || line.compare(0, 7, "rthumb+") == 0) e.thumbs = true;
            }
            e.note = author.empty() ? url : (T(19) + author + (url.empty() ? L"" : L"  " + url));
            found.push_back(std::move(e));
        }
    }
    std::sort(found.begin(), found.end(), [](const LayoutEntry& a, const LayoutEntry& b) { return a.name < b.name; });
    g_layouts.insert(g_layouts.end(), found.begin(), found.end());
}
int layout_index(const std::string& id);

void adv_layout_relayout();

int layout_index(const std::string& id) {
    for (size_t i = 0; i < g_layouts.size(); i++)
        if (g_layouts[i].id == id) return int(i);
    return 0;
}
HWND g_rlist = nullptr;   // ローマ字の表に足した綴りの一覧
HWND g_adv = nullptr;
std::string g_half;   // 詳細設定の窓で選んでいる「半角にする記号」

// その記号を打ったときに入る字 (半角ならそのまま、全角なら 、。・ー「」￥ や全角の記号)
std::wstring sym_shown(wchar_t c, bool half) {
    if (half) return std::wstring(1, c);
    switch (c) {
        case L',': return T(20);
        case L'.': return T(21);
        case L'/': return T(22);
        case L'-': return T(23);
        case L'[': return T(24);
        case L']': return T(25);
        case L'\\': return T(26);
        default: return std::wstring(1, wchar_t(c + 0xFEE0));
    }
}

void sym_label(int i) {
    const bool half = g_half.find(char(SYMS[i])) != std::string::npos;
    SetDlgItemTextW(g_adv, ADV_SYM0 + i, (sym_shown(SYMS[i], half) + (half ? T(27) : T(28))).c_str());
}

// 詳細設定の右の列の下端 (窓の高さを決める)。キー割り当て (341) + 間 (10) + ローマ字の表 (172)
constexpr int ADV_ROMAJI_LIST_H = 176;   // ローマ字の表の高さ (66 では 2 行しか見えなかった、ユーザー 2026-10-20)
constexpr int ADV_RIGHT_BOTTOM = 8 + 341 + 10 + ADV_ROMAJI_LIST_H + 66;   // 右の列の下端 (キー割り当て・ローマ字の表)

// 詳細設定のキー配列の欄を並べ直す: 親指シフトの配列のときだけ親指の設定 (親指キー・連続シフト・単独打鍵) を出し、
// 欄と窓の高さをそれに合わせる (ユーザー 2026-10-16)
void adv_layout_relayout() {
    if (!g_adv || !g_lgroup) return;
    const int idx = layout_index(g_s.layout);
    const bool any = !g_s.layout.empty();
    const bool thumb = idx > 0 && idx < int(g_layouts.size()) && g_layouts[size_t(idx)].thumbs;
    auto place = [&](HWND h, int x, int y, int w, int hh, bool show) {
        if (!h) return;
        SetWindowPos(h, nullptr, S(x), S(y), S(w), S(hh), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    };
    place(GetDlgItem(g_adv, ADV_LAYOUT_NOTE), 24, g_lgroup_y + 52, 430, 20, any);   // 作者・公式ページ (配列を選んだときだけ)
    int y = g_lgroup_y + (any ? 74 : 52);   // 配列の名前 (と作者) の下
    place(g_lthumb_label, 24, y + 4, 80, 20, thumb);
    place(GetDlgItem(g_adv, ADV_LTHUMBS), 110, y, 200, 200, thumb);
    if (thumb) y += 28;
    place(GetDlgItem(g_adv, ADV_LCONT), 24, y, 430, 22, thumb);
    if (thumb) y += 22;
    place(GetDlgItem(g_adv, ADV_LTAP), 24, y, 430, 22, thumb);
    if (thumb) y += 22;
    place(GetDlgItem(g_adv, ADV_LREP), 24, y, 430, 22, any);
    if (any) y += 24;
    place(g_lother, 24, y + 2, 440, 20, true);
    y += 26;
    place(g_lgroup, 10, g_lgroup_y, 460, y - g_lgroup_y, true);
    // ソフトごとの日本語入力: 右の列のローマ字の表の下 (左の列が変換の欄で長くなり、右下が空いた。ユーザー 2026-11-06)。
    // 一覧は左の列の下端まで伸ばす (最低でも 6 行ほど。64 では 2 行しか見えなかった、ユーザー 2026-10-20)
    const int ay = ADV_RIGHT_BOTTOM + 10;
    const int list_h = std::max(130, y - ay - 56);
    place(g_agroup, 490, ay, 460, list_h + 56, true);
    place(g_alist, 506, ay + 20, 428, list_h, true);
    place(GetDlgItem(g_adv, ADV_ATOGGLE), 506, ay + 24 + list_h, 160, 25, true);
    place(GetDlgItem(g_adv, ADV_ADEL), 674, ay + 24 + list_h, 120, 25, true);
    // 閉じるボタンと窓の高さ (両方の列の下端より下)
    const int bottom = std::max(y, ay + list_h + 56) + 10;
    place(GetDlgItem(g_adv, ADV_CLOSE), 870, bottom, 80, 25, true);
    RECT r = {0, 0, S(960), S(bottom + 35)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
    SetWindowPos(g_adv, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    InvalidateRect(g_adv, nullptr, TRUE);
}

// IME を登録し直すと、登録アイコンをいまのタスクバーの色 (ダーク = 白、ライト = 黒) で選び直す (管理者の許可が要る)
void reregister_icon() {
    wchar_t exe[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring args = L"/s \"" + (fs::path(exe).parent_path() / L"shunti_ime_x64.dll").wstring() + L"\"";
    std::wstring reg = std::wstring(sys) + L"\\regsvr32.exe";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_adv, L"runas", reg.c_str(), args.c_str(), nullptr, SW_HIDE)) > 32)
        MessageBoxW(g_adv, T(128),
                    L"shunti IME", MB_ICONINFORMATION);
}

void adv_save() {
    if (g_loading) return;
    HWND keep = g_wnd;
    g_wnd = g_adv;
    const char* themes[] = {"auto", "light", "dark"};
    g_s.theme = themes[std::max(0, selected(ADV_THEME))];
    g_s.typed_strip = checked(ADV_STRIP);
    g_s.typo_level = std::clamp(selected(ADV_TYPOLV), 0, 2);
    g_s.ctx_left = std::clamp(int(GetDlgItemInt(g_adv, ADV_CTXL, nullptr, FALSE)), 0, CTX_LIMIT);
    g_s.ctx_right = std::clamp(int(GetDlgItemInt(g_adv, ADV_CTXR, nullptr, FALSE)), 0, CTX_LIMIT);
    g_s.half_symbols = g_half;
    g_s.numpad = std::max(0, selected(ADV_NUMPAD));
    g_s.page_size = 5 + std::max(0, selected(ADV_PAGE));
    g_s.private_mode = checked(ADV_PRIVATE);
    g_s.app_default_open = SendDlgItemMessageW(g_adv, ADV_APPOPEN, CB_GETCURSEL, 0, 0) != 1;
    if (!g_layouts.empty()) {
        g_s.layout = g_layouts[size_t(std::clamp(selected(ADV_LAYOUT), 0, int(g_layouts.size()) - 1))].id;
        SetDlgItemTextW(g_adv, ADV_LAYOUT_NOTE, g_layouts[size_t(layout_index(g_s.layout))].note.c_str());
    }
    g_s.layout_thumbs = std::clamp(selected(ADV_LTHUMBS), 0, 2);
    g_s.layout_continuous = checked(ADV_LCONT);
    g_s.layout_thumb_tap = checked(ADV_LTAP);
    g_s.layout_repeat = checked(ADV_LREP);
    g_wnd = keep;
    if (!save_settings(g_dir / L"settings.json", g_s))
        MessageBoxW(g_adv, T(12), L"shunti IME", MB_ICONWARNING);
}

void open_romaji_table() {
    fs::path p = g_dir / L"romaji.tsv";
    std::error_code ec;
    if (!fs::exists(p, ec))
        write_file_atomic(p, "# ローマ字の表に足す綴り: 綴り<TAB>かな (1 行に 1 つ。既にある綴りは上書き)\n# 例:\n# xn\tん\n# la\tぁ\n");
    ShellExecuteW(g_adv, L"open", L"notepad.exe", (L"\"" + p.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ローマ字の表に足す綴り (romaji.tsv: 綴り<TAB>かな)
std::vector<std::pair<std::wstring, std::wstring>> romaji_rows() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::string t;
    if (!read_file(g_dir / L"romaji.tsv", t)) return out;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size();
        std::string line = t.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        out.push_back({W8(line.substr(0, tab)), W8(line.substr(tab + 1))});
    }
    return out;
}

void romaji_save(const std::vector<std::pair<std::wstring, std::wstring>>& rows) {
    std::string t = "# ローマ字の表に足す綴り: 綴り<TAB>かな (詳細設定で編集できます)\n";
    for (auto& r : rows) t += to_utf8(U(r.first)) + "\t" + to_utf8(U(r.second)) + "\n";
    write_file_atomic(g_dir / L"romaji.tsv", t);
}

// ---- ソフトごとの日本語入力 (IME が書く app_open.tsv: 実行ファイルの名前<TAB>1 か 0)

// app_open.tsv の行: 名前 	 オン(1)/オフ(0) [	 窓の名前 [	 印]]。印 noshortcut = そのソフトではショートカットを使わない
struct AppRowS { std::wstring name; bool open = false; std::string title, flags; };
std::vector<AppRowS> app_rows() {
    std::vector<AppRowS> rows;
    std::string t;
    if (!read_file(g_dir / L"app_open.tsv", t)) return rows;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size();
        std::string line = t.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        for (size_t a = 0;;) {
            size_t b = line.find('\t', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() < 2 || f[0].empty()) continue;
        AppRowS r;
        r.name = W8(f[0]);
        for (auto& c : r.name) c = wchar_t(towlower(c));   // 大文字小文字の違う同じソフト (Claude.exe・claude.exe) は 1 つに (後の行が新しい)
        r.open = f[1].substr(0, 1) == "1";
        if (f.size() > 2) r.title = f[2];
        if (f.size() > 3) r.flags = f[3];
        else if (r.name == L"javaw.exe" || r.name == L"java.exe") r.flags = "noshortcut";   // 古い行の Java のソフト (IME と同じ既定)
        bool dup = false;
        for (auto& x : rows) if (x.name == r.name) { x = r; dup = true; }
        if (!dup) rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const AppRowS& a, const AppRowS& b) { return a.name < b.name; });
    return rows;
}

void app_write(const std::vector<AppRowS>& rows) {
    std::string o;
    for (auto& r : rows) o += to_utf8(U(r.name)) + "\t" + (r.open ? "1" : "0") + "\t" + r.title + "\t" + r.flags + "\n";
    write_file_atomic(g_dir / L"app_open.tsv", o);
}

bool app_no_shortcut(const AppRowS& r) { return r.flags.find("noshortcut") != std::string::npos; }

void apps_refresh() {
    if (!g_alist) return;
    ListView_DeleteAllItems(g_alist);
    int i = 0;
    for (auto& r : app_rows()) {
        // 窓の名前があれば先に (javaw.exe が Minecraft だと分かるように)
        std::wstring a = r.title.empty() ? r.name : W8(r.title) + L" (" + r.name + L")";
        // オン / オフ / 常にオフ (オフにしてショートカットにも反応しない。ゲーム向け)
        std::wstring b = app_no_shortcut(r) ? T(162) : r.open ? T(29) : T(30);
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = a.data();
        ListView_InsertItem(g_alist, &it);
        ListView_SetItemText(g_alist, i, 1, b.data());
        i++;
    }
}

int app_selected() { return g_alist ? ListView_GetNextItem(g_alist, -1, LVNI_SELECTED) : -1; }

void app_toggle() {
    const int k = app_selected();
    auto rows = app_rows();
    if (k < 0 || k >= int(rows.size())) return;
    // オン → オフ → 常にオフ → オン
    auto& r = rows[size_t(k)];
    if (app_no_shortcut(r)) {
        r.flags.erase(r.flags.find("noshortcut"), 10);
        r.open = true;
    } else if (r.open) {
        r.open = false;
    } else {
        r.flags += "noshortcut";
    }
    app_write(rows);
    apps_refresh();
    ListView_SetItemState(g_alist, k, LVIS_SELECTED, LVIS_SELECTED);
}


void app_del() {
    const int k = app_selected();
    auto rows = app_rows();
    if (k < 0 || k >= int(rows.size())) return;
    rows.erase(rows.begin() + k);   // 消したソフトは次から「初めて使うソフト」の設定になる
    app_write(rows);
    apps_refresh();
}

void romaji_refresh() {
    ListView_DeleteAllItems(g_rlist);
    int i = 0;
    for (auto& r : romaji_rows()) {
        std::wstring a = r.first, b = r.second;
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = a.data();
        ListView_InsertItem(g_rlist, &it);
        ListView_SetItemText(g_rlist, i, 1, b.data());
        i++;
    }
}

std::wstring adv_text(int id) {
    HWND h = GetDlgItem(g_adv, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(size_t(n));
    return s;
}

void romaji_add() {
    std::wstring k = adv_text(ADV_RKEY), v = adv_text(ADV_RKANA);
    if (k.empty() || v.empty()) return;
    for (auto& c : k) if (c >= L'A' && c <= L'Z') c = wchar_t(c + 32);
    auto rows = romaji_rows();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](auto& r) { return r.first == k; }), rows.end());
    rows.push_back({k, v});
    romaji_save(rows);
    SetDlgItemTextW(g_adv, ADV_RKEY, L"");
    SetDlgItemTextW(g_adv, ADV_RKANA, L"");
    romaji_refresh();
}

void romaji_del() {
    int i = ListView_GetNextItem(g_rlist, -1, LVNI_SELECTED);
    auto rows = romaji_rows();
    if (i < 0 || i >= int(rows.size())) return;
    rows.erase(rows.begin() + i);
    romaji_save(rows);
    romaji_refresh();
}

// ---- ショートカットキー (既定も自分で追加も同じ一覧) ----
std::wstring shortcut_line(const std::string& key, const std::string& func) {
    const wchar_t* n = shortcut_func_name(func);
    const std::wstring name = n && *n ? n : W8(func);
    // 日本語キーボードのキーは日本語の名前で見せる (保存は英字の名前のまま)
    std::wstring k = W8(key);
    for (auto [en, ja] : {std::pair{L"Muhenkan", T(31)}, {L"Henkan", T(32)}, {L"Hiragana", T(33)}, {L"Kana", T(34)}})
        if (size_t i = k.rfind(en); i != std::wstring::npos && i + wcslen(en) == k.size()) { k = k.substr(0, i) + ja; break; }
    return k + L" → " + name;
}

void shortcut_refresh() {
    HWND lb = GetDlgItem(g_adv, ADV_SLIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (auto& p : g_s.shortcuts)
        SendMessageW(lb, LB_ADDSTRING, 0, LPARAM(shortcut_line(p.first, p.second).c_str()));
}

// キーと機能の欄の値を読む (キーが押されていなければ false)
bool shortcut_fields(std::string& key, std::string& func) {
    const WORD hot = WORD(SendMessageW(GetDlgItem(g_adv, ADV_SKEY), HKM_GETHOTKEY, 0, 0));
    const UINT vk = LOBYTE(hot);
    if (!vk) return false;
    key = shortcut_key_string(vk, (HIBYTE(hot) & HOTKEYF_CONTROL) != 0, (HIBYTE(hot) & HOTKEYF_SHIFT) != 0,
                              (HIBYTE(hot) & HOTKEYF_ALT) != 0);
    const int fi = int(SendDlgItemMessageW(g_adv, ADV_SFUNC, CB_GETCURSEL, 0, 0));
    if (fi < 0 || fi >= int(shortcut_funcs().size())) return false;
    func = shortcut_funcs()[size_t(fi)].id;
    return true;
}

// 一覧で選ぶとキーと機能の欄に入れる
void shortcut_select() {
    HWND lb = GetDlgItem(g_adv, ADV_SLIST);
    const int i = int(SendMessageW(lb, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= int(g_s.shortcuts.size())) return;
    const ShortcutKey k = parse_shortcut_key(g_s.shortcuts[size_t(i)].first);
    if (k.ok) {
        const WORD mods = WORD((k.ctrl ? HOTKEYF_CONTROL : 0) | (k.shift ? HOTKEYF_SHIFT : 0) | (k.alt ? HOTKEYF_ALT : 0));
        SendMessageW(GetDlgItem(g_adv, ADV_SKEY), HKM_SETHOTKEY, MAKEWORD(k.vk, mods), 0);
    }
    const std::string& func = g_s.shortcuts[size_t(i)].second;
    for (int f = 0; f < int(shortcut_funcs().size()); f++)
        if (shortcut_funcs()[size_t(f)].id == func) { SendDlgItemMessageW(g_adv, ADV_SFUNC, CB_SETCURSEL, WPARAM(f), 0); break; }
}

void shortcut_add() {
    std::string key, func;
    if (!shortcut_fields(key, func)) return;
    for (auto& p : g_s.shortcuts) {   // 同じキーは上書き
        if (p.first == key) { p.second = func; shortcut_refresh(); adv_save(); return; }
    }
    g_s.shortcuts.push_back({key, func});
    shortcut_refresh();
    adv_save();
}

void shortcut_change() {
    HWND lb = GetDlgItem(g_adv, ADV_SLIST);
    const int i = int(SendMessageW(lb, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= int(g_s.shortcuts.size())) return;
    std::string key, func;
    if (!shortcut_fields(key, func)) return;
    g_s.shortcuts[size_t(i)] = {key, func};
    shortcut_refresh();
    SendMessageW(lb, LB_SETCURSEL, WPARAM(i), 0);
    adv_save();
}

void shortcut_del() {
    HWND lb = GetDlgItem(g_adv, ADV_SLIST);
    const int i = int(SendMessageW(lb, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= int(g_s.shortcuts.size())) return;
    g_s.shortcuts.erase(g_s.shortcuts.begin() + i);
    shortcut_refresh();
    adv_save();
}

void shortcut_move(int delta) {
    HWND lb = GetDlgItem(g_adv, ADV_SLIST);
    const int i = int(SendMessageW(lb, LB_GETCURSEL, 0, 0));
    const int j = i + delta;
    if (i < 0 || i >= int(g_s.shortcuts.size()) || j < 0 || j >= int(g_s.shortcuts.size())) return;
    std::swap(g_s.shortcuts[size_t(i)], g_s.shortcuts[size_t(j)]);
    shortcut_refresh();
    SendMessageW(lb, LB_SETCURSEL, WPARAM(j), 0);
    adv_save();
}

void shortcut_reset() {
    const int kmi = std::clamp(int(SendDlgItemMessageW(g_adv, ADV_KEYMAP, CB_GETCURSEL, 0, 0)), 0, 2);
    if (MessageBoxW(g_adv, T(35), L"shunti IME",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    g_s.keymap = keymaps()[size_t(kmi)].id;
    g_s.shortcuts = default_shortcuts(g_s.keymap);
    shortcut_refresh();
    adv_save();
}

LRESULT CALLBACK adv_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE) load_settings(g_dir / L"settings.json", g_s);   // メインの窓と同じ。古い値で書き戻さない
    if (m == WM_COMMAND) {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ADV_CLOSE || id == IDCANCEL) DestroyWindow(h);
        else if ((id == ADV_NUMPAD || id == ADV_PAGE || id == ADV_APPOPEN || id == ADV_THEME || id == ADV_TYPOLV) && code == CBN_SELCHANGE) adv_save();
        else if ((id == ADV_CTXL || id == ADV_CTXR) && code == EN_CHANGE) adv_save();
        else if (id == ADV_ICON) reregister_icon();
        else if (id == ADV_STRIP && code == BN_CLICKED) adv_save();
        else if (id >= ADV_SYM0 && id < ADV_SYM0 + 64 && code == BN_CLICKED) {   // 押すたびに全角 ⇔ 半角
            const char c = char(SYMS[id - ADV_SYM0]);
            const size_t at = g_half.find(c);
            if (at == std::string::npos) g_half += c; else g_half.erase(at, 1);
            sym_label(id - ADV_SYM0);
            adv_save();
        }
        else if (id == ADV_PRIVATE && code == BN_CLICKED) adv_save();
        else if (id == ADV_ROMAJI) open_romaji_table();
        else if ((id == ADV_LAYOUT || id == ADV_LTHUMBS) && code == CBN_SELCHANGE) { adv_save(); adv_layout_relayout(); }
        else if ((id == ADV_LCONT || id == ADV_LTAP || id == ADV_LREP) && code == BN_CLICKED) adv_save();
        else if (id == ADV_RADD) romaji_add();
        else if (id == ADV_RDEL) romaji_del();
        else if (id == ADV_ATOGGLE) app_toggle();
        else if (id == ADV_ADEL) app_del();
        else if (id == ADV_SLIST && code == LBN_SELCHANGE) shortcut_select();
        else if (id == ADV_SADD) shortcut_add();
        else if (id == ADV_SCHG) shortcut_change();
        else if (id == ADV_SDEL) shortcut_del();
        else if (id == ADV_SUP) shortcut_move(-1);
        else if (id == ADV_SDOWN) shortcut_move(1);
        else if (id == ADV_SRESET) shortcut_reset();
        return 0;
    }
    if (m == WM_DESTROY) { g_adv = nullptr; EnableWindow(g_wnd, TRUE); SetForegroundWindow(g_wnd); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// 更新を調べた結果を知らせる
void show_update(const shunti::update::Latest& l, bool interactive) {
    if (l.version.empty()) {
        if (interactive) MessageBoxW(g_wnd, (T(38) + l.error).c_str(), L"shunti IME", MB_ICONWARNING);
        return;
    }
    if (!shunti::update::older(VERSION, l.version)) {
        if (interactive) MessageBoxW(g_wnd, (std::wstring(T(39)) + VERSION + T(40)).c_str(), L"shunti IME", MB_ICONINFORMATION);
        return;
    }
    std::wstring changes;   // 変更点の箇条書き (Release の本文から)
    for (auto& c : l.changes) changes += L"・" + c + L"\n";
    if (!changes.empty()) changes = T(167) + changes + L"\n";
    const std::wstring q = T(41) + l.version + T(42) + VERSION + T(43) + changes + T(44) + T(45);
    const int a = MessageBoxW(g_wnd, q.c_str(), T(46), MB_ICONQUESTION | MB_YESNOCANCEL);
    if (a == IDNO) { ShellExecuteW(g_wnd, L"open", l.page_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (a != IDYES) return;
    std::wstring err;
    if (!shunti::update::download_and_install(l, err)) {
        MessageBoxW(g_wnd, (T(47) + err).c_str(), L"shunti IME", MB_ICONERROR);
        return;
    }
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);   // 設定画面も入れ直すので閉じる
}

// 更新の確認 (GitHub の最新の Windows 版)。interactive = ボタンから (結果を必ず知らせる)。開いたときは 1 日 1 回まで、新しい版があるときだけ知らせる
void check_update(bool interactive) {
    if (interactive) {
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        const shunti::update::Latest l = shunti::update::find_latest();
        SetCursor(old);
        show_update(l, true);
        return;
    }
    const std::filesystem::path stamp = g_dir / L"update_check.txt";
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(stamp, ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::hours(24)) return;
    write_file_atomic(stamp, "checked\n");
    // 開いたときの確認は裏で (ネットが遅くても画面を止めない)。結果は WM_APP + 8 で受け取る
    HWND wnd = g_wnd;
    std::thread([wnd] {
        auto* l = new shunti::update::Latest(shunti::update::find_latest());
        if (!PostMessageW(wnd, WM_APP + 8, 0, LPARAM(l))) delete l;
    }).detach();
}

void open_advanced() {
    if (g_adv) { SetForegroundWindow(g_adv); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = adv_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"ShuntiImeAdvanced";
        RegisterClassExW(&wc);
        reg = true;
    }
    RECT r = {0, 0, S(960), S(ADV_RIGHT_BOTTOM + 45)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
    g_adv = CreateWindowExW(0, L"ShuntiImeAdvanced", T(48), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_adv) return;   // 窓を作れなかった: 設定の窓を使えないままにしない
    HWND keep = g_wnd;
    g_wnd = g_adv;
    int y = 8;
    group(T(49), y, 138);
    for (int i = 0; SYMS[i]; i++)
        make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 24 + (i % 8) * 54, y + 24 + (i / 8) * 28, 50, 25, ADV_SYM0 + i);
    y += 146;
    group(T(50), y, 114);
    make(L"STATIC", T(51), 0, 24, y + 27, 120, 20, 0);
    combo(150, y + 23, 300, ADV_NUMPAD, {T(52), T(53), T(54)});
    make(L"STATIC", T(55), 0, 24, y + 57, 120, 20, 0);
    combo(150, y + 53, 100, ADV_PAGE, {L"5", L"6", L"7", L"8", L"9"});
    make(L"STATIC", T(56), 0, 24, y + 87, 120, 20, 0);
    combo(150, y + 83, 300, ADV_APPOPEN, {T(57), T(58)});
    y += 122;
    // 変換: 打ち間違いの直しの強さ・文脈を読む字数 (ユーザー 2026-11-06。打ち間違いは typo_bias_of、字数は数字で)
    group(T(222), y, 106);
    make(L"STATIC", T(223), 0, 24, y + 27, 120, 20, 0);
    combo(150, y + 23, 300, ADV_TYPOLV, {T(224), T(225), T(226)});
    // 文脈を読む字数は数字で (0〜80。ユーザー 2026-11-06「自分で入力できるように」)
    make(L"STATIC", T(227), 0, 24, y + 57, 120, 20, 0);
    make(L"STATIC", T(228), 0, 150, y + 57, 24, 20, 0);
    make(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 176, y + 54, 50, 23, ADV_CTXL);
    make(L"STATIC", T(229), 0, 240, y + 57, 36, 20, 0);
    make(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 278, y + 54, 50, 23, ADV_CTXR);
    make(L"STATIC", T(230), 0, 24, y + 82, 440, 20, 0);
    y += 114;
    group(T(95), y, 112);
    make(L"STATIC", T(96), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 250, ADV_THEME, {T(97), T(98), T(99)});
    make(L"BUTTON", T(100), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 52, 250, 25, ADV_ICON);
    make(L"BUTTON", T(188), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 82, 430, 22, ADV_STRIP);
    y += 120;
    group(T(59), y, 56);   // 辞書の書き出しは「辞書の管理」へ (2026-11-18 ユーザー)
    make(L"BUTTON", T(60), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 22, 430, 22, ADV_PRIVATE);
    y += 64;
    g_lgroup_y = y;
    g_lgroup = make(L"BUTTON", T(62), BS_GROUPBOX, 10, y, 460, 198, 0);
    {
        load_layouts();
        HWND cb = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 24, y + 23, 300, 400, ADV_LAYOUT);
        for (auto& l : g_layouts) SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(l.name.c_str()));
    }
    make(L"STATIC", L"", 0, 24, y + 52, 430, 20, ADV_LAYOUT_NOTE);
    // 流儀の分かれる所 (配列ソフトの設定と同じ)。位置は adv_layout_relayout が決める
    g_lthumb_label = make(L"STATIC", T(63), 0, 24, y + 78, 80, 20, 0);
    combo(110, y + 74, 200, ADV_LTHUMBS, {T(64), T(65), T(66)});
    make(L"BUTTON", T(67), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 102, 430, 22, ADV_LCONT);
    make(L"BUTTON", T(68), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 124, 430, 22, ADV_LTAP);
    make(L"BUTTON", T(69), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 146, 430, 22, ADV_LREP);
    g_lother = make(L"STATIC", T(70), 0, 24, y + 172, 440, 20, 0);
    make(L"BUTTON", T(71), BS_DEFPUSHBUTTON | WS_TABSTOP, 870, y, 80, 25, ADV_CLOSE);

    // ---- 右の列: ショートカットキー・ローマ字の表 ----
    auto rgroup = [](const wchar_t* t, int gy, int h) { make(L"BUTTON", t, BS_GROUPBOX, 490, gy, 460, h, 0); };
    int ry = 8;
    rgroup(T(72), ry, 341);
    make(L"STATIC", T(73), 0, 506, ry + 24, 30, 20, 0);
    {
        HWND cb = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 542, ry + 20, 120, 200, ADV_KEYMAP);
        for (auto& km : keymaps()) SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(km.name));
    }
    make(L"BUTTON", T(74), BS_PUSHBUTTON | WS_TABSTOP, 820, ry + 20, 114, 25, ADV_SRESET);
    // 既定も自分で追加も同じ一覧 (上から順に、使える状態のものが効く)
    make(WC_LISTBOXW, L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 506, ry + 52, 428, 210, ADV_SLIST);
    make(L"STATIC", T(75), 0, 506, ry + 270, 40, 20, 0);
    make(HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, 548, ry + 266, 130, 23, ADV_SKEY);
    make(L"STATIC", T(76), 0, 686, ry + 270, 40, 20, 0);
    {
        HWND cb = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 728, ry + 266, 206, 300, ADV_SFUNC);
        for (auto& f : shortcut_funcs()) SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(f.name));
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
    }
    make(L"BUTTON", T(77), BS_PUSHBUTTON | WS_TABSTOP, 506, ry + 296, 60, 25, ADV_SADD);
    make(L"BUTTON", T(78), BS_PUSHBUTTON | WS_TABSTOP, 574, ry + 296, 60, 25, ADV_SCHG);
    make(L"BUTTON", T(79), BS_PUSHBUTTON | WS_TABSTOP, 642, ry + 296, 60, 25, ADV_SDEL);
    make(L"BUTTON", T(80), BS_PUSHBUTTON | WS_TABSTOP, 800, ry + 296, 60, 25, ADV_SUP);
    make(L"BUTTON", T(81), BS_PUSHBUTTON | WS_TABSTOP, 868, ry + 296, 60, 25, ADV_SDOWN);
    shortcut_refresh();
    ry += 351;
    rgroup(T(82), ry, ADV_ROMAJI_LIST_H + 66);
    g_rlist = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 506, ry + 22, 428, ADV_ROMAJI_LIST_H, ADV_RLIST);
    const int rb = ry + 22 + ADV_ROMAJI_LIST_H;   // 表の下端 (入力欄はその下)
    ListView_SetExtendedListViewStyle(g_rlist, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        const wchar_t* cols[] = {T(83), T(84)};
        for (int i = 0; i < 2; i++) {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(cols[i]);
            c.cx = S(200);
            ListView_InsertColumn(g_rlist, i, &c);
        }
    }
    make(L"STATIC", T(85), 0, 506, rb + 11, 40, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 548, rb + 8, 100, 23, ADV_RKEY);
    make(L"STATIC", T(86), 0, 660, rb + 11, 40, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 702, rb + 8, 100, 23, ADV_RKANA);
    make(L"BUTTON", T(77), BS_PUSHBUTTON | WS_TABSTOP, 810, rb + 7, 60, 25, ADV_RADD);
    make(L"BUTTON", T(79), BS_PUSHBUTTON | WS_TABSTOP, 874, rb + 7, 60, 25, ADV_RDEL);
    romaji_refresh();
    // ソフトごとの日本語入力のオン・オフ (ソフトを閉じたときの状態を覚えて、次に起動したとき戻す。ユーザー 2026-10-20)
    // 右の列のローマ字の表の下 (位置は adv_layout_relayout が決める。一覧の高さは左の列の下端に合わせ、窓を縦に長くしない)
    g_agroup = make(L"BUTTON", T(87), BS_GROUPBOX, 10, 0, 460, 120, 0);
    g_alist = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 26, 20, 428, 64, ADV_ALIST);
    ListView_SetExtendedListViewStyle(g_alist, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        const wchar_t* cols[] = {T(88), T(89)};
        const int widths[] = {300, 100};
        for (int i = 0; i < 2; i++) {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(cols[i]);
            c.cx = S(widths[i]);
            ListView_InsertColumn(g_alist, i, &c);
        }
    }
    make(L"BUTTON", T(90), BS_PUSHBUTTON | WS_TABSTOP, 26, 88, 160, 25, ADV_ATOGGLE);
    make(L"BUTTON", T(91), BS_PUSHBUTTON | WS_TABSTOP, 194, 88, 120, 25, ADV_ADEL);
    apps_refresh();
    g_loading = true;
    g_half = g_s.half_symbols;
    for (int i = 0; SYMS[i]; i++) sym_label(i);
    select(ADV_THEME, g_s.theme == "light" ? 1 : g_s.theme == "dark" ? 2 : 0);
    check(ADV_STRIP, g_s.typed_strip);
    select(ADV_NUMPAD, g_s.numpad);
    select(ADV_TYPOLV, std::clamp(g_s.typo_level, 0, 2));
    SetDlgItemInt(g_adv, ADV_CTXL, UINT(std::clamp(g_s.ctx_left, 0, CTX_LIMIT)), FALSE);
    SetDlgItemInt(g_adv, ADV_CTXR, UINT(std::clamp(g_s.ctx_right, 0, CTX_LIMIT)), FALSE);
    select(ADV_PAGE, std::clamp(g_s.page_size, 5, 9) - 5);
    check(ADV_PRIVATE, g_s.private_mode);
    select(ADV_APPOPEN, g_s.app_default_open ? 0 : 1);
    select(ADV_KEYMAP, g_s.keymap == "atok" ? 1 : g_s.keymap == "kotoeri" ? 2 : 0);
    select(ADV_LAYOUT, layout_index(g_s.layout));
    select(ADV_LTHUMBS, g_s.layout_thumbs);
    check(ADV_LCONT, g_s.layout_continuous);
    check(ADV_LTAP, g_s.layout_thumb_tap);
    check(ADV_LREP, g_s.layout_repeat);
    adv_layout_relayout();
    if (!g_layouts.empty()) SetWindowTextW(GetDlgItem(g_wnd, ADV_LAYOUT_NOTE), g_layouts[size_t(layout_index(g_s.layout))].note.c_str());
    g_loading = false;
    g_wnd = keep;
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_adv, SW_SHOW);
}

// 「学習をリセット」は無くした (2026-11-06 ユーザー)。全部消すのは、期間の「全期間」で「左の期間の学習を消去」

// AI学習のチェック 1 つ (2026-11-06 ユーザー: 同意と「使う」を 1 つに)。AI学習は確定した文の記録から育てるので、
// 入れるときに記録の同意を聞き、記録 (learn_log) と AI学習 (ai_learn) を一緒に入れる。
// 切るときは、保存した文と AI学習で育てた中身を消すかを聞く
void toggle_ai_learn() {
    const bool on = checked(ID_AILEARN);
    if (on) {
        if (!g_s.learn_log && MessageBoxW(g_wnd, T(180), T(178), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            check(ID_AILEARN, false);
            return;
        }
        g_s.learn_log = true;
    } else {
        const int r = MessageBoxW(g_wnd, T(181), T(179), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (r == IDCANCEL) {
            check(ID_AILEARN, true);
            return;
        }
        if (r == IDYES) {
            recent_drop(g_dir, "C");
            forget_ai_learn(g_dir);   // どのモデルの分も
        }
        g_s.learn_log = false;
    }
    // 確認の窓を閉じると設定の窓がもう一度前に出て、そのとき設定を読み直してチェックを描き直す (WM_ACTIVATE)。
    // 押したチェックが元に戻り、もう一度押さないと切り替わらなかった (ユーザー 2026-11-06) ので、選んだ値に入れ直してから保存する
    check(ID_AILEARN, on);
    save();   // ai_learn はチェックから読む
}

// 選んだ期間の始まり (秒)。全期間なら 0。選べなければ -1
long long span_since() {
    static const long long SPANS[] = {600, 3600, 6 * 3600, 24 * 3600, 7 * 24 * 3600, 30 * 24 * 3600};
    const int i = selected(ID_RECENT_SPAN);
    const int n = int(sizeof(SPANS) / sizeof(SPANS[0]));
    if (i == n) return 0;   // 全期間
    if (i < 0 || i > n) return -1;
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now - SPANS[i];
}

// 選んだ期間の学習を消す (覚えた変換を元に戻し、保存した文も消す)。全期間なら学習を全部消す。
// どちらも AI学習で育てた重みは残す (消すのは AI学習を外すときだけ。ユーザー 2026-11-06)
void forget_recent_learning() {
    const long long since = span_since();
    if (since < 0) return;
    if (since == 0) {
        if (MessageBoxW(g_wnd, T(92), T(174), MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
        forget_all(g_dir);
        MessageBoxW(g_wnd, T(94), L"shunti IME", MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_wnd, T(175), T(174), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    const int n = forget_recent(g_dir, since, "LHSFC");
    const std::wstring msg = n > 0 ? std::to_wstring(n) + T(176) : std::wstring(T(177));
    MessageBoxW(g_wnd, msg.c_str(), L"shunti IME", MB_ICONINFORMATION);
}

// 選んだ期間の AI用データ (AI学習のために保存した確定した文) だけを消す。覚えた変換と AI学習の重みはそのまま
void cut_recent_log() {
    const long long since = span_since();
    if (since < 0) return;
    if (MessageBoxW(g_wnd, T(219), T(218), MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    const int n = recent_cut(g_dir, since);
    const std::wstring msg = n > 0 ? std::to_wstring(n) + T(220) : std::wstring(T(221));
    MessageBoxW(g_wnd, msg.c_str(), L"shunti IME", MB_ICONINFORMATION);
}

void show_settings();

void build() {
    int y = 8;
    group(T(32), y, 89);
    make(L"STATIC", T(101), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 250, ID_MODEL, {T(102), T(103), T(189), T(231), T(232)});
    make(L"STATIC", T(280), 0, 24, y + 56, 170, 20, 0);
    combo(200, y + 52, 250, ID_DICT, {T(281), T(282)});   // 辞書 (2026-11-19。クリーン (PureXS2) のモデルはいつもクリーン辞書)
    y += 97;

    group(T(50), y, 360);
    make(L"STATIC", T(104), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 120, ID_PUNCT, {T(105), T(106), T(107), T(108)});
    make(L"STATIC", T(112), 0, 24, y + 56, 170, 20, 0);
    combo(200, y + 52, 120, ID_LIVEDISPLAY, {T(191), T(192), T(193)});
    make(L"BUTTON", T(109), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 79, 430, 22, ID_SPACE);
    make(L"BUTTON", T(110), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 104, 430, 22, ID_DIGITS);
    make(L"BUTTON", T(111), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 129, 430, 22, ID_LIVE);
    make(L"BUTTON", T(113), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 154, 430, 22, ID_LIVECOMMIT);
    make(L"BUTTON", T(159), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 179, 430, 22, ID_CANDTREE);
    make(L"BUTTON", T(160), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 204, 430, 22, ID_SPACELIT);
    make(L"BUTTON", T(161), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 229, 430, 22, ID_LATIN);
    make(L"BUTTON", T(163), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 254, 430, 22, ID_CAPS);
    make(L"BUTTON", T(165), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 279, 430, 22, ID_TYPO);
    make(L"BUTTON", T(194), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 304, 430, 22, ID_PUNCTCOMMIT);
    make(L"BUTTON", T(216), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 329, 430, 22, ID_CLIPHIST);
    y += 368;

    // ユーザー辞書は「辞書の管理」の窓にまとめた (登録・ファイルから登録・パック。2026-11-18 ユーザー)
    group(T(13), y, 60);
    make(L"BUTTON", T(252), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 22, 190, 25, ID_DICTMGR);
    y += 68;

    // 学習: AI学習のチェック 1 つと、消去 (期間を選んで消す・全部消す) をひとまとめに (2026-11-06 ユーザー)
    group(T(187), y, 88);
    make(L"BUTTON", T(195), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 22, 430, 22, ID_AILEARN);
    combo(24, y + 53, 90, ID_RECENT_SPAN, {T(168), T(169), T(170), T(171), T(172), T(173), T(217)});
    select(ID_RECENT_SPAN, 1);
    make(L"BUTTON", T(174), BS_PUSHBUTTON | WS_TABSTOP, 120, y + 52, 165, 25, ID_RECENT_FORGET);
    make(L"BUTTON", T(218), BS_PUSHBUTTON | WS_TABSTOP, 291, y + 52, 165, 25, ID_LOG_CUT);
    y += 96;
    make(L"BUTTON", T(124), BS_PUSHBUTTON | WS_TABSTOP, 10, y, 120, 25, ID_ADVANCED);
    make(L"BUTTON", T(125), BS_PUSHBUTTON | WS_TABSTOP, 138, y, 120, 25, ID_UPDATE);
    y += 35;

    std::wstring about = std::wstring(L"shunti IME ") + VERSION;
#ifdef SHUNTI_INPUT_LOG
    about += T(126);
#endif
    make(L"STATIC", about.c_str(), 0, 14, y + 6, 200, 36, 0);
    make(L"BUTTON", T(127), BS_PUSHBUTTON | WS_TABSTOP, 222, y, 80, 25, ID_LICENSE);
    make(L"BUTTON", L"GitHub", BS_PUSHBUTTON | WS_TABSTOP, 308, y, 76, 25, ID_GITHUB);
    make(L"BUTTON", T(71), BS_DEFPUSHBUTTON | WS_TABSTOP, 390, y, 80, 25, IDCANCEL);

    show_settings();
}

// g_s を画面のチェックと選択に出す
void show_settings() {
    g_loading = true;
    select(ID_PUNCT, g_s.punct);
    {
        const auto it = std::find(g_models.begin(), g_models.end(), g_s.model);
        select(ID_MODEL, it == g_models.end() ? 1 : int(it - g_models.begin()));
    }
    select(ID_DICT, g_s.dict == "clean" || g_s.model == "pure" ? 1 : 0);
    EnableWindow(GetDlgItem(g_wnd, ID_DICT), g_s.model != "pure");   // PureXS2 はクリーン辞書と組
    check(ID_SPACE, g_s.space_fullwidth);
    check(ID_DIGITS, g_s.digits_fullwidth);
    check(ID_LIVE, g_s.live);
    select(ID_LIVEDISPLAY, !g_s.live_display ? 0 : g_s.live_wait ? 1 : 2);
    check(ID_CANDTREE, g_s.cand_tree);
    check(ID_SPACELIT, g_s.space_literal);
    check(ID_LATIN, g_s.latin_auto);
    check(ID_CAPS, g_s.caps_alpha);
    check(ID_TYPO, g_s.typo_fix);
    check(ID_PUNCTCOMMIT, g_s.punct_commit);
    check(ID_CLIPHIST, g_s.clip_history);
    check(ID_AILEARN, g_s.ai_learn && g_s.learn_log);
    check(ID_LIVECOMMIT, g_s.live_commit);
    g_loading = false;
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_APP + 8: {
            std::unique_ptr<shunti::update::Latest> latest(reinterpret_cast<shunti::update::Latest*>(l));
            show_update(*latest, false);
            return 0;
        }
        case WM_APP + 7:
        check_update(false);
        return 0;
    case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if ((id == ID_PUNCT || id == ID_MODEL || id == ID_DICT || id == ID_LIVEDISPLAY) && code == CBN_SELCHANGE) { save(); if (id == ID_MODEL) show_settings(); }
            else if (id == IDCANCEL) DestroyWindow(h);   // 「閉じる」と Esc
            else if (((id >= ID_SPACE && id <= ID_LIVECOMMIT) || id == ID_CANDTREE || id == ID_SPACELIT || id == ID_LATIN || id == ID_CAPS || id == ID_TYPO || id == ID_PUNCTCOMMIT || id == ID_CLIPHIST) && code == BN_CLICKED) save();
            else if (id == ID_AILEARN && code == BN_CLICKED) toggle_ai_learn();
            else if (id == ID_DICTMGR) open_dict_manager();
            else if (id == ID_ADVANCED) open_advanced();
            else if (id == ID_UPDATE) check_update(true);
            else if (id == ID_RECENT_FORGET) forget_recent_learning();
            else if (id == ID_LOG_CUT) cut_recent_log();
            else if (id == ID_LICENSE) {
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring lic = (fs::path(exe).parent_path() / L"LICENSE.txt").wstring();
                ShellExecuteW(h, L"open", L"notepad.exe", lic.c_str(), nullptr, SW_SHOWNORMAL);
            } else if (id == ID_GITHUB) {
                ShellExecuteW(h, L"open", L"https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next", nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_ACTIVATE:
            // 開いている間に IME 側 (内緒・モデルの切り替えのショートカット) や別の設定の画面が書いた分を読み直す。
            // 読み直さないと、次にチェックを押したとき古い値でまとめて書き戻していた (切り替えが効かない・戻る)
            if (LOWORD(w) != WA_INACTIVE) {
                load_settings(g_dir / L"settings.json", g_s);
                show_settings();
            }
            return DefWindowProcW(h, m, w, l);
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

}  // namespace

// 更新の後に 1 度だけ、確定した文を残してよいかを聞く (install.ps1 が --consent で呼ぶ。起動したときにも見る)
void ask_consent() {
    if (g_s.consent_asked) return;
    g_s.consent_asked = true;
    if (!g_s.learn_log)
        g_s.learn_log = MessageBoxW(nullptr, T(180), T(178), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES;
    if (g_s.learn_log) g_s.ai_learn = true;   // 同意 = AI学習を使う (チェック 1 つ)
    save_settings(g_dir / L"settings.json", g_s);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // ショートカットキー「ユーザー辞書を開く」: --dict なら起動後にユーザー辞書の欄に移る
    const bool open_dict = cmd && wcsstr(cmd, L"--dict") != nullptr;
    // 2 つ目は開かずに、開いている窓を前に出す
    HANDLE once = CreateMutexW(nullptr, TRUE, L"ShuntiImeSettings");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"ShuntiImeSettings", nullptr)) SetForegroundWindow(other);
        return 0;
    }
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);

    PWSTR app = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) g_dir = fs::path(app) / L"shunti IME";
    CoTaskMemFree(app);
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    load_settings(g_dir / L"settings.json", g_s);
    ask_consent();
    if (cmd && wcsstr(cmd, L"--consent")) return 0;
    UserDict dict(g_dir / L"userdict.json");
    g_dict = &dict;

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);   // ダイアログの既定の色
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.lpszClassName = L"ShuntiImeSettings";
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();
    RECT rc = {0, 0, S(480), S(816)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE, 0, g_dpi);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, T(129), WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    g_dpi = GetDpiForWindow(g_wnd);
    // Windows の既定のメッセージ用の文字 (ダイアログと同じ)
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    build();
    {   // 窓の高さを中身に合わせる (チェックを足すたびに下が見切れていた。ユーザー 2026-10-27)
        RECT b = {};
        GetWindowRect(GetDlgItem(g_wnd, IDCANCEL), &b);
        MapWindowPoints(nullptr, g_wnd, reinterpret_cast<POINT*>(&b), 2);
        RECT need = {0, 0, S(480), b.bottom + S(10)};
        AdjustWindowRectExForDpi(&need, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE, 0, g_dpi);
        SetWindowPos(g_wnd, nullptr, 0, 0, need.right - need.left, need.bottom - need.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    ShowWindow(g_wnd, show);
    PostMessageW(g_wnd, WM_APP + 7, 0, 0);   // 開いた後に更新を確かめる (1 日 1 回まで)
    if (open_dict) open_dict_manager();   // ショートカット「ユーザー辞書を開く」: 辞書の管理の新規登録の読みの欄へ

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_dm && IsDialogMessageW(g_dm, &msg)) continue;   // 辞書の管理の窓の Tab・Enter
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(once);
    return 0;
}

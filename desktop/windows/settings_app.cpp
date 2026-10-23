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
#include <string>
#include <vector>

#include "../core/settings.h"
#include "../core/store.h"
#include "../core/text.h"
#include "shortcut_key.h"
#include "strings.h"

using namespace shunti;
using namespace shunti::win;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t VERSION[] = L"0.7.0";

enum Id {
    ID_THEME = 100, ID_PUNCT, ID_SPACE, ID_DIGITS, ID_LIVE, ID_LIVEDISPLAY, ID_LIVECOMMIT, ID_SUGGEST,
    ID_LIST, ID_READING, ID_WORD, ID_POS, ID_ADD, ID_REMOVE, ID_IMPORT, ID_ADVANCED, ID_UPDATE, ID_RESET, ID_LICENSE, ID_GITHUB, ID_ICON, ID_MODEL, ID_CANDTREE, ID_SPACELIT, ID_LATIN, ID_CAPS,
};

// 変換モデル (settings.json の model の値と、画面の表示)。名前は配布の呼び名 (開発名は出さない)
const char* const MODELS[] = {"light", "standard"};

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
HWND g_wnd = nullptr, g_list = nullptr;
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
    const char* themes[] = {"auto", "light", "dark"};
    g_s.theme = themes[std::max(0, selected(ID_THEME))];
    g_s.punct = std::max(0, selected(ID_PUNCT));
    g_s.space_fullwidth = checked(ID_SPACE);
    g_s.digits_fullwidth = checked(ID_DIGITS);
    g_s.live = checked(ID_LIVE);
    g_s.live_display = checked(ID_LIVEDISPLAY);
    g_s.cand_tree = checked(ID_CANDTREE);
    g_s.space_literal = checked(ID_SPACELIT);
    g_s.latin_auto = checked(ID_LATIN);
    g_s.caps_alpha = checked(ID_CAPS);
    g_s.live_commit = checked(ID_LIVECOMMIT);
    g_s.model = MODELS[std::clamp(selected(ID_MODEL), 0, 1)];
    if (!save_settings(g_dir / L"settings.json", g_s))
        MessageBoxW(g_wnd, T(12), L"shunti IME", MB_ICONWARNING);
}

std::wstring pos_label(const UserDict::Entry& e) {
    for (auto& p : pos_items())
        if (e.pos == p.pos && (e.pos != "verb" || e.group == p.group || (e.group.empty() && std::string(p.group) == "godan"))) return p.label;
    return W8(e.pos);
}

void refresh_list() {
    g_dict->refresh();
    ListView_DeleteAllItems(g_list);
    int i = 0;
    for (auto& e : g_dict->entries()) {
        std::wstring r = W(e.reading), w = W(e.word), p = pos_label(e);
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = r.data();
        ListView_InsertItem(g_list, &it);
        ListView_SetItemText(g_list, i, 1, w.data());
        ListView_SetItemText(g_list, i, 2, p.data());
        i++;
    }
}

// 品詞が「英字の語」のときは読みの欄を使わない (灰色にして空に)
void update_reading_box() {
    const int p = std::max(0, selected(ID_POS));
    const bool latin = std::string(pos_items()[size_t(p)].pos) == "latin";
    HWND r = GetDlgItem(g_wnd, ID_READING);
    if (latin) SetWindowTextW(r, L"");
    EnableWindow(r, !latin);
}

void add_word() {
    int p = std::max(0, selected(ID_POS));
    std::wstring r = std::string(pos_items()[size_t(p)].pos) == "latin" ? L"" : text_of(ID_READING), w = text_of(ID_WORD);
    std::string err = g_dict->add(U(r), U(w), pos_items()[size_t(p)].pos, pos_items()[size_t(p)].group);
    if (!err.empty()) {
        MessageBoxW(g_wnd, W8(err).c_str(), T(13), MB_ICONINFORMATION);
        return;
    }
    SetDlgItemTextW(g_wnd, ID_READING, L"");
    SetDlgItemTextW(g_wnd, ID_WORD, L"");
    refresh_list();
    SetFocus(GetDlgItem(g_wnd, ID_READING));
}

// まとめて登録: MS-IME の「一覧の出力」の .txt (UTF-16) や、読み,語句,品詞 の CSV (UTF-8 / Shift_JIS)
void import_words() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"辞書の一覧 (*.txt;*.csv;*.tsv)\0*.txt;*.csv;*.tsv\0すべてのファイル\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        MessageBoxW(g_wnd, T(14), T(13), MB_ICONWARNING);
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
    auto res = g_dict->import_text(U(text));
    refresh_list();
    std::wstring msg = std::to_wstring(res.first) + T(15);
    if (!res.second.empty()) {
        msg += T(16) + std::to_wstring(res.second.size()) + L"):";
        for (size_t i = 0; i < res.second.size() && i < 12; i++) msg += L"\n" + W8(res.second[i]);
        if (res.second.size() > 12) msg += L"\n…";
    }
    MessageBoxW(g_wnd, msg.c_str(), T(13), MB_ICONINFORMATION);
}

// ---------------- 詳細設定 (別の窓) ----------------
// 記号ごとの全角/半角・テンキー・候補の数・無変換キー・再変換・プライベート・ローマ字の表・辞書の書き出し
const wchar_t SYMS[] = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
enum { ADV_SYM0 = 500, ADV_NUMPAD = 600, ADV_PAGE, ADV_APPOPEN, ADV_PRIVATE, ADV_ROMAJI, ADV_EXPORT, ADV_CLOSE,
       ADV_KEYMAP = 710, ADV_LAYOUT = 711, ADV_LAYOUT_NOTE = 712, ADV_LTHUMBS = 713, ADV_LCONT, ADV_LTAP, ADV_LREP, ADV_RLIST = 720, ADV_RKEY, ADV_RKANA, ADV_RADD, ADV_RDEL, ADV_ALIST = 730, ADV_ATOGGLE, ADV_ADEL,
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
    // ソフトごとの日本語入力 (キー配列の欄の下)
    y += 8;
    // 一覧は右の列の下端まで伸ばす (最低でも 6 行ほど。64 では 2 行しか見えなかった、ユーザー 2026-10-20)
    const int list_h = std::max(130, ADV_RIGHT_BOTTOM - y - 64);
    place(g_agroup, 10, y, 460, list_h + 56, true);
    place(g_alist, 26, y + 20, 428, list_h, true);
    place(GetDlgItem(g_adv, ADV_ATOGGLE), 26, y + 24 + list_h, 160, 25, true);
    place(GetDlgItem(g_adv, ADV_ADEL), 194, y + 24 + list_h, 120, 25, true);
    y += list_h + 56;
    // 閉じるボタンと窓の高さ (右の列の下端より下)
    const int bottom = std::max(y, ADV_RIGHT_BOTTOM) + 10;
    place(GetDlgItem(g_adv, ADV_CLOSE), 870, bottom, 80, 25, true);
    RECT r = {0, 0, S(960), S(bottom + 35)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
    SetWindowPos(g_adv, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    InvalidateRect(g_adv, nullptr, TRUE);
}

void adv_save() {
    if (g_loading) return;
    HWND keep = g_wnd;
    g_wnd = g_adv;
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
        SendMessageW(GetDlgItem(g_adv, ADV_SKEY), HKM_SETHOTKEY, MAKEWPARAM(k.vk, mods), 0);
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

void export_words() {
    wchar_t path[MAX_PATH] = L"shunti_userdict.txt";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_adv;
    of.lpstrFilter = L"テキスト (*.txt)\0*.txt\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"txt";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    g_dict->refresh();
    u16 t = g_dict->export_text();
    std::string b = "\xFF\xFE";   // MS-IME と同じ UTF-16 (読み込みにも使える)
    b.append(reinterpret_cast<const char*>(t.data()), t.size() * 2);
    std::ofstream f(path, std::ios::binary);
    f.write(b.data(), std::streamsize(b.size()));
    f.close();
    if (!f) {
        MessageBoxW(g_adv, T(36), T(13), MB_ICONWARNING);
        return;
    }
    MessageBoxW(g_adv, (std::to_wstring(g_dict->entries().size()) + T(37)).c_str(), T(13), MB_ICONINFORMATION);
}

LRESULT CALLBACK adv_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ADV_CLOSE || id == IDCANCEL) DestroyWindow(h);
        else if ((id == ADV_NUMPAD || id == ADV_PAGE || id == ADV_APPOPEN) && code == CBN_SELCHANGE) adv_save();
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
        else if (id == ADV_EXPORT) export_words();
        return 0;
    }
    if (m == WM_DESTROY) { g_adv = nullptr; EnableWindow(g_wnd, TRUE); SetForegroundWindow(g_wnd); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// 更新の確認 (GitHub の最新の Windows 版)。interactive = ボタンから (結果を必ず知らせる)。開いたときは 1 日 1 回まで、新しい版があるときだけ知らせる
void check_update(bool interactive) {
    const std::filesystem::path stamp = g_dir / L"update_check.txt";
    if (!interactive) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(stamp, ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::hours(24)) return;
        write_file_atomic(stamp, "checked\n");
    }
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    shunti::update::Latest l = shunti::update::find_latest();
    SetCursor(old);
    if (l.version.empty()) {
        if (interactive) MessageBoxW(g_wnd, (T(38) + l.error).c_str(), L"shunti IME", MB_ICONWARNING);
        return;
    }
    if (!shunti::update::older(VERSION, l.version)) {
        if (interactive) MessageBoxW(g_wnd, (std::wstring(T(39)) + VERSION + T(40)).c_str(), L"shunti IME", MB_ICONINFORMATION);
        return;
    }
    const std::wstring q = T(41) + l.version + T(42) + VERSION + T(43) + T(44) + T(45);
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
    group(T(59), y, 90);
    make(L"BUTTON", T(60), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 22, 430, 22, ADV_PRIVATE);
    make(L"BUTTON", T(61), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 54, 180, 25, ADV_EXPORT);
    y += 98;
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
    // 左の列のキー配列の欄の下 (位置は adv_layout_relayout が決める。右の列に置くと窓が画面より縦に長くなった)
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
    select(ADV_NUMPAD, g_s.numpad);
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

void remove_word() {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return;
    g_dict->refresh();
    if (i >= int(g_dict->entries().size())) return;
    UserDict::Entry e = g_dict->entries()[size_t(i)];
    g_dict->remove(e.reading, e.word);
    refresh_list();
}

void reset_learning() {
    if (MessageBoxW(g_wnd, T(92), T(93),
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    Learning(g_dir / L"learned.json").clear();
    MessageBoxW(g_wnd, T(94), L"shunti IME", MB_ICONINFORMATION);
}

void build() {
    int y = 8;
    group(T(95), y, 86);
    make(L"STATIC", T(96), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 250, ID_THEME, {T(97), T(98), T(99)});
    make(L"BUTTON", T(100), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 52, 250, 25, ID_ICON);
    y += 94;

    group(T(32), y, 60);
    make(L"STATIC", T(101), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 250, ID_MODEL, {T(102), T(103)});
    y += 68;

    group(T(50), y, 285);
    make(L"STATIC", T(104), 0, 24, y + 27, 170, 20, 0);
    combo(200, y + 23, 120, ID_PUNCT, {T(105), T(106), T(107), T(108)});
    make(L"BUTTON", T(109), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 54, 430, 22, ID_SPACE);
    make(L"BUTTON", T(110), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 79, 430, 22, ID_DIGITS);
    make(L"BUTTON", T(111), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 104, 430, 22, ID_LIVE);
    make(L"BUTTON", T(112), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 129, 430, 22, ID_LIVEDISPLAY);
    make(L"BUTTON", T(113), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 154, 430, 22, ID_LIVECOMMIT);
    make(L"BUTTON", T(159), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 179, 430, 22, ID_CANDTREE);
    make(L"BUTTON", T(160), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 204, 430, 22, ID_SPACELIT);
    make(L"BUTTON", T(161), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 229, 430, 22, ID_LATIN);
    make(L"BUTTON", T(163), BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 254, 430, 22, ID_CAPS);
    y += 293;

    group(T(13), y, 216);
    g_list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 24, y + 22, 432, 90, ID_LIST);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {T(115), T(116), T(117)};
    int widths[] = {130, 130, 150};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = S(widths[i]);
        ListView_InsertColumn(g_list, i, &c);
    }
    make(L"STATIC", T(118), 0, 24, y + 125, 44, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 70, y + 122, 150, 23, ID_READING);
    make(L"STATIC", T(119), 0, 232, y + 125, 44, 20, 0);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 278, y + 122, 178, 23, ID_WORD);
    make(L"STATIC", T(120), 0, 24, y + 156, 44, 20, 0);
    HWND pos = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 70, y + 152, 210, 200, ID_POS);
    for (auto& p : pos_items()) SendMessageW(pos, CB_ADDSTRING, 0, LPARAM(p.label));
    make(L"BUTTON", T(77), BS_PUSHBUTTON | WS_TABSTOP, 290, y + 151, 80, 25, ID_ADD);
    make(L"BUTTON", T(79), BS_PUSHBUTTON | WS_TABSTOP, 376, y + 151, 80, 25, ID_REMOVE);
    make(L"BUTTON", T(121), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 182, 190, 25, ID_IMPORT);
    make(L"STATIC", T(122),
         0, 222, y + 180, 240, 34, 0);
    y += 224;

    group(T(123), y, 60);
    make(L"BUTTON", T(93), BS_PUSHBUTTON | WS_TABSTOP, 24, y + 24, 140, 25, ID_RESET);
    y += 68;
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

    g_loading = true;
    select(ID_THEME, g_s.theme == "light" ? 1 : g_s.theme == "dark" ? 2 : 0);
    select(ID_PUNCT, g_s.punct);
    select(ID_MODEL, g_s.model == "light" ? 0 : 1);
    select(ID_POS, 0);
    check(ID_SPACE, g_s.space_fullwidth);
    check(ID_DIGITS, g_s.digits_fullwidth);
    check(ID_LIVE, g_s.live);
    check(ID_LIVEDISPLAY, g_s.live_display);
    check(ID_CANDTREE, g_s.cand_tree);
    check(ID_SPACELIT, g_s.space_literal);
    check(ID_LATIN, g_s.latin_auto);
    check(ID_CAPS, g_s.caps_alpha);
    check(ID_LIVECOMMIT, g_s.live_commit);
    g_loading = false;
    refresh_list();
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_APP + 7:
        check_update(false);
        return 0;
    case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if ((id == ID_THEME || id == ID_PUNCT || id == ID_MODEL) && code == CBN_SELCHANGE) save();
            else if (id == IDCANCEL) DestroyWindow(h);   // 「閉じる」と Esc
            else if (((id >= ID_SPACE && id <= ID_LIVECOMMIT) || id == ID_CANDTREE || id == ID_SPACELIT || id == ID_LATIN || id == ID_CAPS) && code == BN_CLICKED) save();
            else if (id == ID_ADD) add_word();
            else if (id == ID_POS && code == CBN_SELCHANGE) update_reading_box();
            else if (id == ID_IMPORT) import_words();
            else if (id == ID_ADVANCED) open_advanced();
            else if (id == ID_UPDATE) check_update(true);
            else if (id == ID_REMOVE) remove_word();
            else if (id == ID_RESET) reset_learning();
            else if (id == ID_ICON) {
                // IME を登録し直すと、登録アイコンをいまのタスクバーの色 (ダーク = 白、ライト = 黒) で選び直す (管理者の許可が要る)
                wchar_t exe[MAX_PATH], sys[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                GetSystemDirectoryW(sys, MAX_PATH);
                std::wstring args = L"/s \"" + (fs::path(exe).parent_path() / L"shunti_ime_x64.dll").wstring() + L"\"";
                std::wstring reg = std::wstring(sys) + L"\\regsvr32.exe";
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(h, L"runas", reg.c_str(), args.c_str(), nullptr, SW_HIDE)) > 32)
                    MessageBoxW(h, T(128),
                                L"shunti IME", MB_ICONINFORMATION);
            }
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
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

}  // namespace

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
    ShowWindow(g_wnd, show);
    PostMessageW(g_wnd, WM_APP + 7, 0, 0);   // 開いた後に更新を確かめる (1 日 1 回まで)
    if (open_dict) SetFocus(GetDlgItem(g_wnd, ID_READING));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(once);
    return 0;
}

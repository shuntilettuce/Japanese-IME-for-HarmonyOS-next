// shunti IME のクリップボードの履歴 (shunti_clip.exe)。2026-11-05 ユーザー: Android 版の履歴 (画像・動画の見本つき・保存する) を PC にも。
// 1 つのプロセスが常駐して、コピーされたもの (文・画像・ファイル) を %APPDATA%\shunti IME\clips に記録する。
// IME が使われ始めたときに --listen で起動する (text_service.cpp)。設定「クリップボードの履歴を残す」が切れていれば記録しない。
// --show <窓> で履歴の窓を出す (Zori-chan の右クリック・ショートカット)。押すと、その窓のアプリに貼り付ける
// (クリップボードに入れ直してから Ctrl+V を送る)。右クリックで見本のポップアップ (貼り付け・固定・消去。Android 版と同じ言葉)。
// 記録しないもの: コピーした側が「履歴に入れない」印を付けたもの (パスワード管理ソフトなど。Windows の履歴 Win+V と同じ印)
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../core/json.h"
#include "strings.h"

using namespace shunti;
namespace fs = std::filesystem;
namespace gp = Gdiplus;

namespace {

// ---------------------------------------------------------------- 決まり
constexpr int MAX_ITEMS = 50;                          // 固定したものは数えない
constexpr int64_t MAX_AGE_MS = 30LL * 86400 * 1000;   // 30 日 (固定したものは消さない)
constexpr size_t MAX_IMAGE = 100u << 20;               // 画像 1 枚 100MB まで
constexpr uint64_t MAX_MEDIA = 300ull << 20;           // 画像の合計 300MB まで (古いものから消す)
constexpr size_t MAX_TEXT = 200000;                    // 文は 20 万字まで
constexpr int THUMB_PX = 192;
constexpr UINT WM_SHOWLIST = WM_APP + 1;   // wParam = 貼り付け先の窓
constexpr UINT WM_THUMB = WM_APP + 2;      // 見本ができた (lParam = new ThumbDone*)
constexpr UINT_PTR TIMER_PASTE = 1;
constexpr UINT_PTR TIMER_READ = 2;   // コピーされてから少し待って読む (すぐ開くと、OLE で書くアプリの書き込みが失敗する)
constexpr wchar_t CLASS[] = L"shunti_clip";

enum Kind { TEXT = 0, IMAGE = 1, VIDEO = 2, FILES = 3 };

struct Clip {
    int64_t id = 0;
    int kind = TEXT;
    std::wstring text;                 // TEXT
    std::wstring file;                 // IMAGE: clips フォルダの png の名前
    std::vector<std::wstring> paths;   // VIDEO・FILES: コピーしたファイルの場所 (写しは取らない)
    std::wstring thumb;                // 見本の png の名前 (無ければ空)
    std::wstring name;
    std::string sig;                   // 同じものか (中身の FNV-1a)
    int64_t time = 0;
    int64_t size = 0;                  // IMAGE の png の大きさ
    bool pinned = false;
};

struct ThumbDone { int64_t id; std::wstring thumb; };

HWND g_wnd = nullptr;
HWND g_target = nullptr;               // 貼り付け先
fs::path g_dir;                        // %APPDATA%\shunti IME\clips
fs::path g_settings;
std::vector<Clip> g_clips;             // 新しい順 (固定したものも同じ並びの中。画面では固定を上に)
DWORD g_self_seq = 0;                  // 自分がクリップボードに入れたときの番号 (記録しない)
UINT g_dpi = 96;
HFONT g_font = nullptr, g_small = nullptr;
int g_scroll = 0;                      // 一覧の上から何 px ずらしているか
int g_hot = -1;                        // 選んでいる行 (マウス・矢印)
int g_pop = -1;                        // 見本のポップアップを出している行 (-1 = 出していない)
bool g_pasting = false;
const Clip* g_pending = nullptr;
std::map<std::wstring, std::unique_ptr<gp::Bitmap>> g_imgs;   // 見本の絵 (名前 → 絵)
UINT CF_PNG = 0, CF_EXCLUDE = 0, CF_CANINCLUDE = 0, CF_IGNORE = 0, CF_DROPEFFECT = 0;

int px(int v) { return MulDiv(v, int(g_dpi), 96); }

int64_t now_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return int64_t((ULARGE_INTEGER{{ft.dwLowDateTime, ft.dwHighDateTime}}.QuadPart - 116444736000000000ULL) / 10000);
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool dark() {
    DWORD v = 1, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                        RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS && v == 0;
}

// 設定「クリップボードの履歴を残す」(settings.json の clip_history。無ければ残す)
bool recording_on() {
    std::ifstream f(g_settings, std::ios::binary);
    if (!f) return true;
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (!json::Parser(ss.str()).parse(v)) return true;
    const json::Value* x = v.get("clip_history");
    return !(x && x->type == json::Value::Bool && !x->b);
}

// ---------------------------------------------------------------- 保存 (clips.json)
void save_index() {
    std::string o = "[\n";
    for (size_t i = 0; i < g_clips.size(); i++) {
        const Clip& c = g_clips[i];
        o += "{\"id\":" + std::to_string(c.id) + ",\"kind\":" + std::to_string(c.kind) + ",\"time\":" + std::to_string(c.time) +
             ",\"size\":" + std::to_string(c.size) + ",\"pinned\":" + (c.pinned ? "true" : "false") + ",\"text\":" + json::quote(to_utf8(c.text)) +
             ",\"file\":" + json::quote(to_utf8(c.file)) + ",\"thumb\":" + json::quote(to_utf8(c.thumb)) + ",\"name\":" + json::quote(to_utf8(c.name)) +
             ",\"sig\":" + json::quote(c.sig) + ",\"paths\":[";
        for (size_t k = 0; k < c.paths.size(); k++) o += (k ? "," : "") + json::quote(to_utf8(c.paths[k]));
        o += i + 1 < g_clips.size() ? "]},\n" : "]}\n";
    }
    o += "]\n";
    const fs::path tmp = g_dir / L"clips.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(o.data(), std::streamsize(o.size()));
    }
    MoveFileExW(tmp.c_str(), (g_dir / L"clips.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void load_index() {
    std::ifstream f(g_dir / L"clips.json", std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (!json::Parser(ss.str()).parse(v) || v.type != json::Value::Array) return;
    for (const auto& e : v.arr) {
        if (e.type != json::Value::Object) continue;
        Clip c;
        auto num = [&](const char* k) -> int64_t { const json::Value* x = e.get(k); return x && x->type == json::Value::Number ? int64_t(x->num) : 0; };
        c.id = num("id");
        c.kind = int(num("kind"));
        c.time = num("time");
        c.size = num("size");
        const json::Value* p = e.get("pinned");
        c.pinned = p && p->type == json::Value::Bool && p->b;
        c.text = from_utf8(e.get_str("text"));
        c.file = from_utf8(e.get_str("file"));
        c.thumb = from_utf8(e.get_str("thumb"));
        c.name = from_utf8(e.get_str("name"));
        c.sig = e.get_str("sig");
        if (const json::Value* a = e.get("paths"); a && a->type == json::Value::Array)
            for (const auto& s : a->arr) if (s.type == json::Value::String) c.paths.push_back(from_utf8(s.str));
        if (c.kind < TEXT || c.kind > FILES) continue;
        g_clips.push_back(std::move(c));
    }
}

// 名前は clips フォルダの中のものだけ (".." や区切りを含むものは消さない)
void drop_file(const std::wstring& name) {
    if (name.empty() || name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos || name.find(L"..") != std::wstring::npos) return;
    g_imgs.erase(name);
    DeleteFileW((g_dir / name).c_str());
}
void drop_files(const Clip& c) {
    drop_file(c.file);
    drop_file(c.thumb);
}

// 上限: 30 日・50 件・画像の合計 300MB (固定したものは残す。古いものから)
void prune() {
    const int64_t old = now_ms() - MAX_AGE_MS;
    std::vector<Clip> keep;
    int n = 0;
    uint64_t media = 0;
    for (auto& c : g_clips) {
        bool ok = c.pinned || (c.time >= old && n < MAX_ITEMS && media + uint64_t(c.size) <= MAX_MEDIA);
        if (ok && !c.pinned) { n++; media += uint64_t(c.size); }
        if (ok) keep.push_back(std::move(c)); else drop_files(c);
    }
    g_clips = std::move(keep);
}

int64_t new_id() {
    static int64_t last = 0;
    last = std::max(now_ms(), last + 1);
    return last;
}

// 一番上に足す。同じもの (sig) があれば、それを一番上へ動かす (固定・見本はそのまま)
Clip* put(Clip c) {
    auto it = std::find_if(g_clips.begin(), g_clips.end(), [&](const Clip& x) { return x.sig == c.sig && x.kind == c.kind; });
    if (it != g_clips.end()) {
        Clip old = std::move(*it);
        g_clips.erase(it);
        drop_file(c.file);   // 新しく書いた png は要らない
        old.time = now_ms();
        g_clips.insert(g_clips.begin(), std::move(old));
    } else {
        c.id = new_id();
        c.time = now_ms();
        g_clips.insert(g_clips.begin(), std::move(c));
    }
    const int64_t id = g_clips.front().id;
    prune();
    save_index();
    for (auto& x : g_clips) if (x.id == id) return &x;
    return nullptr;
}

// ---------------------------------------------------------------- 絵
CLSID png_clsid() {
    static CLSID id{};
    static bool done = false;
    if (done) return id;
    done = true;
    UINT n = 0, sz = 0;
    gp::GetImageEncodersSize(&n, &sz);
    std::vector<BYTE> buf(sz);
    auto* enc = reinterpret_cast<gp::ImageCodecInfo*>(buf.data());
    gp::GetImageEncoders(n, sz, enc);
    for (UINT i = 0; i < n; i++)
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) id = enc[i].Clsid;
    return id;
}

std::unique_ptr<gp::Bitmap> bitmap_from_bytes(const void* p, size_t n) {
    IStream* s = SHCreateMemStream(static_cast<const BYTE*>(p), UINT(n));
    if (!s) return nullptr;
    std::unique_ptr<gp::Bitmap> b(gp::Bitmap::FromStream(s));
    s->Release();
    if (!b || b->GetLastStatus() != gp::Ok) return nullptr;
    // 流れを手放しても使えるように写しを取る
    std::unique_ptr<gp::Bitmap> c(b->Clone(0, 0, int(b->GetWidth()), int(b->GetHeight()), PixelFormat32bppARGB));
    return c && c->GetLastStatus() == gp::Ok ? std::move(c) : nullptr;
}

// 縮小した見本を png で保存し、名前を返す
std::wstring save_thumb(gp::Bitmap& src, int64_t stamp) {
    const UINT w = src.GetWidth(), h = src.GetHeight();
    if (!w || !h) return {};
    const double r = std::min(1.0, double(THUMB_PX) / double(std::max(w, h)));
    const int tw = std::max(1, int(w * r)), th = std::max(1, int(h * r));
    gp::Bitmap t(tw, th, PixelFormat32bppARGB);
    {
        gp::Graphics g(&t);
        g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.DrawImage(&src, 0, 0, tw, th);
    }
    const std::wstring name = L"t" + std::to_wstring(stamp) + L".png";
    const CLSID png = png_clsid();
    return t.Save((g_dir / name).c_str(), &png, nullptr) == gp::Ok ? name : std::wstring();
}

gp::Bitmap* image_of(const std::wstring& name) {
    if (name.empty()) return nullptr;
    auto it = g_imgs.find(name);
    if (it != g_imgs.end()) return it->second.get();
    std::ifstream f(g_dir / name, std::ios::binary);
    std::unique_ptr<gp::Bitmap> b;
    if (f) {
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        b = bitmap_from_bytes(data.data(), data.size());
    }
    if (g_imgs.size() > 60) g_imgs.clear();
    gp::Bitmap* r = b.get();
    g_imgs[name] = std::move(b);
    return r;
}

// ファイルの見本 (動画・画像は中身の 1 コマ、そのほかはアイコン)。Explorer と同じもの。時間がかかるので別のスレッドで
void make_file_thumb(int64_t id, std::wstring path) {
    std::thread([id, path = std::move(path)] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring name;
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            HBITMAP hb = nullptr;
            if (SUCCEEDED(f->GetImage(SIZE{THUMB_PX, THUMB_PX}, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                BITMAP bm{};
                GetObjectW(hb, sizeof bm, &bm);
                const int w = bm.bmWidth, h = bm.bmHeight;
                std::vector<BYTE> px(size_t(w) * size_t(h) * 4);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;   // 上から
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                HDC dc = GetDC(nullptr);
                const bool ok = w > 0 && h > 0 && GetDIBits(dc, hb, 0, UINT(h), px.data(), &bi, DIB_RGB_COLORS) == h;
                ReleaseDC(nullptr, dc);
                if (ok) {
                    gp::Bitmap b(w, h, w * 4, PixelFormat32bppPARGB, px.data());
                    name = save_thumb(b, new_id());
                }
                DeleteObject(hb);
            }
            f->Release();
        }
        CoUninitialize();
        if (!name.empty()) PostMessageW(g_wnd, WM_THUMB, 0, LPARAM(new ThumbDone{id, name}));
    }).detach();
}

// ---------------------------------------------------------------- 記録
bool has_format(UINT f) { return f && IsClipboardFormatAvailable(f); }

// コピーした側が「履歴に入れない」印を付けたか (Windows の履歴と同じ印。パスワード管理ソフトなど)
bool excluded() {
    if (has_format(CF_EXCLUDE) || has_format(CF_IGNORE)) return true;
    if (has_format(CF_CANINCLUDE)) {
        if (HANDLE h = GetClipboardData(CF_CANINCLUDE)) {
            const DWORD* v = static_cast<const DWORD*>(GlobalLock(h));
            const bool no = v && GlobalSize(h) >= sizeof(DWORD) && *v == 0;
            GlobalUnlock(h);
            if (no) return true;
        }
    }
    return false;
}

bool is_video(const std::wstring& p) {
    static const wchar_t* ex[] = {L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".mkv", L".webm", L".3gp", L".mpg", L".mpeg", L".ts"};
    const std::wstring e = fs::path(p).extension().wstring();
    for (auto* x : ex) if (_wcsicmp(e.c_str(), x) == 0) return true;
    return false;
}
bool is_image_file(const std::wstring& p) {
    static const wchar_t* ex[] = {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".heic", L".tif", L".tiff"};
    const std::wstring e = fs::path(p).extension().wstring();
    for (auto* x : ex) if (_wcsicmp(e.c_str(), x) == 0) return true;
    return false;
}

void record() {
    const DWORD seq = GetClipboardSequenceNumber();
    if (seq == g_self_seq || !recording_on()) return;
    bool opened = false;
    for (int i = 0; i < 10 && !(opened = OpenClipboard(g_wnd)); i++) Sleep(20);   // ほかのアプリが開いている間は少し待つ
    if (!opened) return;
    if (excluded()) { CloseClipboard(); return; }

    Clip c;
    std::string png;   // 画像 (png のバイト)
    std::string dib;   // 画像 (CF_DIB。png が無いとき)
    if (has_format(CF_HDROP)) {
        if (HANDLE h = GetClipboardData(CF_HDROP)) {
            HDROP d = static_cast<HDROP>(GlobalLock(h));
            if (d) {
                const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 1000; i++) {
                    const UINT len = DragQueryFileW(d, i, nullptr, 0);
                    std::wstring p(len, L'\0');
                    DragQueryFileW(d, i, p.data(), len + 1);
                    c.paths.push_back(p);
                }
                GlobalUnlock(h);
            }
        }
        if (!c.paths.empty()) c.kind = c.paths.size() == 1 && is_video(c.paths[0]) ? VIDEO : FILES;
    } else if (has_format(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                c.text.assign(t, wcsnlen(t, std::min<size_t>(GlobalSize(h) / sizeof(wchar_t), MAX_TEXT)));
                GlobalUnlock(h);
            }
        }
        c.kind = TEXT;
    } else if (has_format(CF_PNG) || has_format(CF_DIB)) {
        const UINT f = has_format(CF_PNG) ? CF_PNG : CF_DIB;
        if (HANDLE h = GetClipboardData(f)) {
            const size_t n = GlobalSize(h);
            if (const void* p = GlobalLock(h)) {
                if (n > 0 && n <= MAX_IMAGE) (f == CF_PNG ? png : dib).assign(static_cast<const char*>(p), n);
                GlobalUnlock(h);
            }
        }
        c.kind = IMAGE;
    }
    CloseClipboard();

    if (c.kind == TEXT) {
        if (c.text.empty()) return;
        c.sig = fnv(c.text.data(), c.text.size() * sizeof(wchar_t));
        put(std::move(c));
    } else if (c.kind == IMAGE) {
        std::unique_ptr<gp::Bitmap> b;
        if (!png.empty()) {
            b = bitmap_from_bytes(png.data(), png.size());
        } else if (dib.size() > sizeof(BITMAPINFOHEADER)) {
            // CF_DIB の前に BMP の頭を付ければ、そのまま絵として読める
            const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
            DWORD colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? (1u << bih->biBitCount) : 0);
            DWORD masks = (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
            BITMAPFILEHEADER fh{};
            fh.bfType = 0x4D42;
            fh.bfSize = DWORD(sizeof fh + dib.size());
            fh.bfOffBits = DWORD(sizeof fh + bih->biSize + masks + colors * 4);
            std::string bmp(reinterpret_cast<const char*>(&fh), sizeof fh);
            bmp += dib;
            b = bitmap_from_bytes(bmp.data(), bmp.size());
        }
        if (!b) return;
        const int64_t stamp = new_id();
        c.file = L"i" + std::to_wstring(stamp) + L".png";
        if (!png.empty()) {
            std::ofstream f(g_dir / c.file, std::ios::binary);
            f.write(png.data(), std::streamsize(png.size()));
            c.sig = fnv(png.data(), png.size());
        } else {
            const CLSID enc = png_clsid();
            if (b->Save((g_dir / c.file).c_str(), &enc, nullptr) != gp::Ok) return;
            c.sig = fnv(dib.data(), dib.size());
        }
        std::error_code ec;
        c.size = int64_t(fs::file_size(g_dir / c.file, ec));
        c.thumb = save_thumb(*b, stamp);
        put(std::move(c));
    } else if (!c.paths.empty()) {
        std::wstring all;
        for (auto& p : c.paths) all += p + L"|";
        c.sig = fnv(all.data(), all.size() * sizeof(wchar_t));
        c.name = fs::path(c.paths[0]).filename().wstring();
        if (c.paths.size() > 1) c.name += T(214) + std::to_wstring(c.paths.size() - 1) + T(215);
        const bool need_thumb = std::none_of(g_clips.begin(), g_clips.end(), [&](const Clip& x) { return x.sig == c.sig && !x.thumb.empty(); });
        const std::wstring first = c.paths[0];
        if (Clip* added = put(std::move(c)); added && need_thumb) make_file_thumb(added->id, first);
    }
    if (IsWindowVisible(g_wnd)) InvalidateRect(g_wnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- 貼り付け
HGLOBAL global_copy(const void* p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return nullptr;
    if (void* d = GlobalLock(g)) { memcpy(d, p, n); GlobalUnlock(g); }
    return g;
}

// 画像を 24bit の CF_DIB に (透明な所は白に。32bit の DIB は透明を黒にしてしまうアプリがある)
HGLOBAL make_dib(gp::Bitmap& src) {
    const int w = int(src.GetWidth()), h = int(src.GetHeight());
    gp::Bitmap flat(w, h, PixelFormat24bppRGB);
    {
        gp::Graphics g(&flat);
        g.Clear(gp::Color(255, 255, 255, 255));
        g.DrawImage(&src, 0, 0, w, h);
    }
    gp::BitmapData bd{};
    gp::Rect r(0, 0, w, h);
    if (flat.LockBits(&r, gp::ImageLockModeRead, PixelFormat24bppRGB, &bd) != gp::Ok) return nullptr;
    const int stride = (w * 3 + 3) & ~3;
    const size_t n = sizeof(BITMAPINFOHEADER) + size_t(stride) * size_t(h);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) {
        auto* p = static_cast<BYTE*>(GlobalLock(g));
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(p);
        *bih = {};
        bih->biSize = sizeof *bih;
        bih->biWidth = w;
        bih->biHeight = h;   // 下から
        bih->biPlanes = 1;
        bih->biBitCount = 24;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = DWORD(stride * h);
        BYTE* dst = p + sizeof *bih;
        for (int y = 0; y < h; y++)
            memcpy(dst + size_t(stride) * size_t(h - 1 - y), static_cast<BYTE*>(bd.Scan0) + ptrdiff_t(bd.Stride) * y, size_t(w) * 3);
        GlobalUnlock(g);
    }
    flat.UnlockBits(&bd);
    return g;
}

// 履歴の 1 件をクリップボードに入れ直す。できたら true
bool to_clipboard(const Clip& c) {
    std::vector<std::pair<UINT, HGLOBAL>> data;
    if (c.kind == TEXT) {
        data.push_back({CF_UNICODETEXT, global_copy(c.text.c_str(), (c.text.size() + 1) * sizeof(wchar_t))});
    } else if (c.kind == IMAGE) {
        std::ifstream f(g_dir / c.file, std::ios::binary);
        if (!f) return false;
        std::string png((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto b = bitmap_from_bytes(png.data(), png.size());
        if (!b) return false;
        data.push_back({CF_DIB, make_dib(*b)});
        if (CF_PNG) data.push_back({CF_PNG, global_copy(png.data(), png.size())});
    } else {
        std::wstring list;
        for (auto& p : c.paths) {
            std::error_code ec;
            if (fs::exists(p, ec)) list += p + L'\0';
        }
        if (list.empty()) return false;
        list += L'\0';
        std::vector<BYTE> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        auto* df = reinterpret_cast<DROPFILES*>(buf.data());
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        data.push_back({CF_HDROP, global_copy(buf.data(), buf.size())});
        const DWORD effect = DROPEFFECT_COPY;
        if (CF_DROPEFFECT) data.push_back({CF_DROPEFFECT, global_copy(&effect, sizeof effect)});
    }
    bool opened = false;
    for (int i = 0; i < 10 && !(opened = OpenClipboard(g_wnd)); i++) Sleep(20);
    if (!opened) { for (auto& d : data) if (d.second) GlobalFree(d.second); return false; }
    EmptyClipboard();
    bool ok = false;
    for (auto& d : data) {
        if (d.second && SetClipboardData(d.first, d.second)) ok = true;
        else if (d.second) GlobalFree(d.second);
    }
    CloseClipboard();
    g_self_seq = GetClipboardSequenceNumber();
    return ok;
}

void hide() {
    g_pop = -1;
    ShowWindow(g_wnd, SW_HIDE);
    g_imgs.clear();   // 大きな見本の絵を手放す
}

// 押した 1 件を貼る: クリップボードに入れ直し、窓を閉じて元のアプリに戻し、少し待ってから Ctrl+V
void paste(size_t i);

void send_ctrl_v() {
    INPUT in[4] = {};
    for (auto& x : in) x.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'V';
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// ---------------------------------------------------------------- 画面
struct Colors { COLORREF bg, hot, text, sub, accent, onAccent, panel, border, btn; };
Colors colors() {
    if (dark()) return {RGB(32, 32, 32), RGB(55, 55, 58), RGB(242, 242, 247), RGB(154, 154, 160), RGB(91, 155, 255), RGB(255, 255, 255), RGB(44, 44, 46), RGB(70, 70, 72), RGB(58, 58, 60)};
    return {RGB(250, 250, 250), RGB(232, 238, 248), RGB(28, 28, 30), RGB(122, 127, 135), RGB(30, 84, 199), RGB(255, 255, 255), RGB(255, 255, 255), RGB(215, 216, 219), RGB(237, 238, 240)};
}

// 画面の並び: 固定したものを上に、あとは新しい順
std::vector<size_t> order() {
    std::vector<size_t> o;
    for (size_t i = 0; i < g_clips.size(); i++) if (g_clips[i].pinned) o.push_back(i);
    for (size_t i = 0; i < g_clips.size(); i++) if (!g_clips[i].pinned) o.push_back(i);
    return o;
}

int header_h() { return px(40); }
int footer_h() { return px(24); }
int row_h() { return px(52); }

RECT client() { RECT r; GetClientRect(g_wnd, &r); return r; }
RECT list_rect() { RECT r = client(); r.top += header_h(); r.bottom -= footer_h(); return r; }
RECT clear_btn() { RECT r = client(); return {r.right - px(124), px(8), r.right - px(10), px(32)}; }
int max_scroll() { const RECT l = list_rect(); return std::max(0, int(g_clips.size()) * row_h() - int(l.bottom - l.top)); }

std::wstring label(const Clip& c) {
    if (c.kind == TEXT) {
        std::wstring t = c.text.substr(0, 300);
        for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        return t;
    }
    if (c.kind == IMAGE) return T(211);
    if (c.kind == VIDEO) return std::wstring(T(212)) + L"  " + c.name;
    return c.name.empty() ? std::wstring(T(213)) : c.name;
}

int row_at(int y) {
    const RECT l = list_rect();
    if (y < l.top || y >= l.bottom) return -1;
    const int i = (y - l.top + g_scroll) / row_h();
    return i < int(g_clips.size()) ? i : -1;
}

struct Popup { RECT box, preview, paste, pin, del; };
Popup popup_geom() {
    const RECT rc = client();
    Popup p{};
    const int w = rc.right - px(24), ph = px(200), bh = px(32);
    const int h = ph + bh + px(30);
    const int row_top = list_rect().top + g_pop * row_h() - g_scroll;
    int top = row_top + row_h() + px(4);
    if (top + h > rc.bottom - px(4)) top = std::max(int(rc.top) + px(4), row_top - h - px(4));
    p.box = {px(12), top, px(12) + w, top + h};
    p.preview = {p.box.left + px(10), p.box.top + px(10), p.box.right - px(10), p.box.top + px(10) + ph};
    const int by = p.preview.bottom + px(10), bw = px(84);
    p.paste = {p.box.left + px(10), by, p.box.left + px(10) + bw, by + bh};
    p.pin = {p.paste.right + px(8), by, p.paste.right + px(8) + bw, by + bh};
    p.del = {p.pin.right + px(8), by, p.pin.right + px(8) + bw, by + bh};
    return p;
}

bool in(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void round_rect(gp::Graphics& g, const RECT& r, int rad, COLORREF fillc, COLORREF line, bool stroke) {
    gp::GraphicsPath path;
    const int d = rad * 2;
    path.AddArc(int(r.left), int(r.top), d, d, 180, 90);
    path.AddArc(int(r.right) - d - 1, int(r.top), d, d, 270, 90);
    path.AddArc(int(r.right) - d - 1, int(r.bottom) - d - 1, d, d, 0, 90);
    path.AddArc(int(r.left), int(r.bottom) - d - 1, d, d, 90, 90);
    path.CloseFigure();
    gp::SolidBrush br(gp::Color(255, GetRValue(fillc), GetGValue(fillc), GetBValue(fillc)));
    g.FillPath(&br, &path);
    if (stroke) {
        gp::Pen pen(gp::Color(255, GetRValue(line), GetGValue(line), GetBValue(line)), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void text_in(HDC dc, const RECT& r, const std::wstring& t, HFONT f, COLORREF c, UINT flags) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT x = r;
    DrawTextW(dc, t.c_str(), int(t.size()), &x, flags | DT_NOPREFIX);
}

// 絵を枠に収める (cover = 枠を埋めて切る、でなければ全体が見えるように)
void draw_image(gp::Graphics& g, gp::Bitmap* b, const RECT& r, bool cover) {
    if (!b) return;
    const double iw = b->GetWidth(), ih = b->GetHeight(), rw = r.right - r.left, rh = r.bottom - r.top;
    if (iw <= 0 || ih <= 0) return;
    g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
    if (cover) {
        const double s = std::max(rw / iw, rh / ih);
        const double sw = rw / s, sh = rh / s;
        g.DrawImage(b, gp::RectF(gp::REAL(r.left), gp::REAL(r.top), gp::REAL(rw), gp::REAL(rh)), gp::REAL((iw - sw) / 2), gp::REAL((ih - sh) / 2),
                    gp::REAL(sw), gp::REAL(sh), gp::UnitPixel);
    } else {
        const double s = std::min(1.0 * rw / iw, 1.0 * rh / ih);
        const double w = iw * s, h = ih * s;
        g.DrawImage(b, gp::RectF(gp::REAL(r.left + (rw - w) / 2), gp::REAL(r.top + (rh - h) / 2), gp::REAL(w), gp::REAL(h)));
    }
}

// 大きな見本の絵: 画像はもとの png、画像のファイルはそのファイル、ほかは見本
gp::Bitmap* preview_of(const Clip& c) {
    if (c.kind == IMAGE) return image_of(c.file);
    if (c.kind == FILES && c.paths.size() == 1 && is_image_file(c.paths[0])) {
        const std::wstring key = L"@" + c.paths[0];
        auto it = g_imgs.find(key);
        if (it != g_imgs.end()) return it->second.get();
        std::unique_ptr<gp::Bitmap> b;
        std::error_code ec;
        if (fs::file_size(c.paths[0], ec) <= MAX_IMAGE && !ec) {
            std::ifstream f(c.paths[0], std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            b = bitmap_from_bytes(data.data(), data.size());
        }
        gp::Bitmap* r = b.get();
        g_imgs[key] = std::move(b);
        if (r) return r;
    }
    return image_of(c.thumb);
}

void button(HDC dc, gp::Graphics& g, const RECT& r, const wchar_t* t, bool on, const Colors& k) {
    round_rect(g, r, px(8), on ? k.accent : k.btn, k.border, false);
    text_in(dc, r, t, g_font, on ? k.onAccent : k.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paint(HDC dc0) {
    const RECT rc = client();
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    const Colors k = colors();
    fill(dc, rc, k.bg);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        // 上: 見出しと「固定以外を消す」
        text_in(dc, {px(14), 0, rc.right - px(130), header_h()}, T(204), g_font, k.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        button(dc, g, clear_btn(), T(205), false, k);
        const RECT l = list_rect();
        HRGN clip = CreateRectRgn(l.left, l.top, l.right, l.bottom);
        SelectClipRgn(dc, clip);
        const auto o = order();
        if (o.empty()) text_in(dc, l, T(207), g_font, k.sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (size_t n = 0; n < o.size(); n++) {
            const int top = l.top + int(n) * row_h() - g_scroll;
            if (top + row_h() < l.top || top > l.bottom) continue;
            const Clip& c = g_clips[o[n]];
            RECT row{0, top, rc.right, top + row_h()};
            if (int(n) == g_hot) fill(dc, row, k.hot);
            int x = px(12);
            if (c.kind != TEXT) {
                RECT th{x, top + px(4), x + px(44), top + px(48)};
                gp::Bitmap* b = image_of(c.thumb);
                if (b) {
                    gp::GraphicsPath p;
                    p.AddRectangle(gp::Rect(th.left, th.top, th.right - th.left, th.bottom - th.top));
                    draw_image(g, b, th, c.kind != FILES);
                } else {
                    round_rect(g, th, px(6), k.btn, k.border, false);
                }
                if (c.kind == VIDEO) {   // 動画の印
                    const float cx = float(th.left + th.right) / 2, cy = float(th.top + th.bottom) / 2, s = float(px(8));
                    gp::SolidBrush wb(gp::Color(200, 0, 0, 0));
                    g.FillEllipse(&wb, cx - s * 1.4f, cy - s * 1.4f, s * 2.8f, s * 2.8f);
                    gp::PointF tri[3] = {{cx - s * 0.5f, cy - s * 0.8f}, {cx - s * 0.5f, cy + s * 0.8f}, {cx + s * 0.9f, cy}};
                    gp::SolidBrush white(gp::Color(255, 255, 255, 255));
                    g.FillPolygon(&white, tri, 3);
                }
                x = th.right + px(10);
            }
            const int tag = c.pinned ? px(40) : 0;
            text_in(dc, {x, top, rc.right - px(12) - tag, top + row_h()}, label(c), g_font, k.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (c.pinned) text_in(dc, {rc.right - px(12) - tag, top, rc.right - px(12), top + row_h()}, T(209), g_small, k.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            HPEN pen = CreatePen(PS_SOLID, 1, k.border);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, px(12), top + row_h() - 1, nullptr);
            LineTo(dc, rc.right - px(12), top + row_h() - 1);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        text_in(dc, {px(12), rc.bottom - footer_h(), rc.right - px(12), rc.bottom}, T(206), g_small, k.sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 見本のポップアップ (画面は替えない): 一覧を暗くして、その行の近くに
        if (g_pop >= 0 && g_pop < int(o.size())) {
            const Clip& c = g_clips[o[size_t(g_pop)]];
            gp::SolidBrush dim(gp::Color(dark() ? 110 : 60, 0, 0, 0));
            g.FillRectangle(&dim, 0, 0, int(rc.right), int(rc.bottom));
            const Popup p = popup_geom();
            round_rect(g, p.box, px(12), k.panel, k.border, true);
            if (c.kind == TEXT) {
                std::wstring t = c.text.substr(0, 4000);
                text_in(dc, p.preview, t, g_font, k.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
            } else if (gp::Bitmap* b = preview_of(c)) {
                draw_image(g, b, {p.preview.left, p.preview.top, p.preview.right, p.preview.bottom - px(20)}, false);
                text_in(dc, {p.preview.left, p.preview.bottom - px(18), p.preview.right, p.preview.bottom}, label(c), g_small, k.sub,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                text_in(dc, p.preview, label(c), g_font, k.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            button(dc, g, p.paste, T(208), true, k);
            button(dc, g, p.pin, T(209), c.pinned, k);
            button(dc, g, p.del, T(210), false, k);
        }
    }
    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void make_fonts() {
    if (g_font) DeleteObject(g_font);
    if (g_small) DeleteObject(g_small);
    g_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_small = CreateFontW(-px(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
}

// 貼り付け先の入力位置の近く (分からなければマウスの所) に出す
void show_list(HWND target) {
    g_target = target && IsWindow(target) ? GetAncestor(target, GA_ROOT) : GetForegroundWindow();
    g_scroll = 0;
    g_hot = g_clips.empty() ? -1 : 0;
    g_pop = -1;
    POINT at{};
    GUITHREADINFO gi{sizeof gi};
    const DWORD tid = g_target ? GetWindowThreadProcessId(g_target, nullptr) : 0;
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndCaret && !IsRectEmpty(&gi.rcCaret)) {
        at = {gi.rcCaret.left, gi.rcCaret.bottom};
        ClientToScreen(gi.hwndCaret, &at);
    } else {
        GetCursorPos(&at);
    }
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        if (auto f = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(sh, "GetDpiForMonitor"))) f(mon, 0, &dx, &dy);
        FreeLibrary(sh);
    }
    if (dx != g_dpi) { g_dpi = dx; make_fonts(); }
    const int w = px(380), h = px(470);
    int x = at.x, y = at.y + px(6);
    const RECT& wa = mi.rcWork;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = std::max(int(wa.top), int(at.y) - h - px(30));
    x = std::max(x, int(wa.left));
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void paste(size_t i) {
    if (i >= g_clips.size()) return;
    if (!to_clipboard(g_clips[i])) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const int64_t id = g_clips[i].id;
    // 使ったものを一番上へ (固定はそのまま)
    Clip c = std::move(g_clips[i]);
    g_clips.erase(g_clips.begin() + ptrdiff_t(i));
    c.time = now_ms();
    g_clips.insert(g_clips.begin(), std::move(c));
    save_index();
    (void)id;
    g_pasting = true;
    hide();
    if (g_target && IsWindow(g_target)) SetForegroundWindow(g_target);
    SetTimer(g_wnd, TIMER_PASTE, 80, nullptr);
}

size_t index_of_row(int n) {
    const auto o = order();
    return n >= 0 && n < int(o.size()) ? o[size_t(n)] : size_t(-1);
}

void ensure_visible(int n) {
    const RECT l = list_rect();
    const int top = n * row_h(), vis = l.bottom - l.top;
    if (top < g_scroll) g_scroll = top;
    if (top + row_h() > g_scroll + vis) g_scroll = top + row_h() - vis;
    g_scroll = std::clamp(g_scroll, 0, max_scroll());
}

void remove_row(int n) {
    const size_t i = index_of_row(n);
    if (i == size_t(-1)) return;
    drop_files(g_clips[i]);
    g_clips.erase(g_clips.begin() + ptrdiff_t(i));
    save_index();
    g_pop = -1;
    g_hot = std::min(g_hot, int(g_clips.size()) - 1);
    g_scroll = std::clamp(g_scroll, 0, max_scroll());
}

void toggle_pin(int n) {
    const size_t i = index_of_row(n);
    if (i == size_t(-1)) return;
    g_clips[i].pinned = !g_clips[i].pinned;
    save_index();
    g_pop = -1;
}

void clear_unpinned() {
    std::vector<Clip> keep;
    for (auto& c : g_clips) { if (c.pinned) keep.push_back(std::move(c)); else drop_files(c); }
    g_clips = std::move(keep);
    save_index();
    g_hot = g_clips.empty() ? -1 : 0;
    g_scroll = 0;
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            if (!recording_on() && !IsWindowVisible(h)) { DestroyWindow(h); return 0; }   // 設定で切った: 常駐をやめる
            SetTimer(h, TIMER_READ, 150, nullptr);   // 続けて変わったら最後の 1 回だけ読む
            return 0;
        case WM_SHOWLIST: show_list(reinterpret_cast<HWND>(wp)); return 0;
        case WM_THUMB: {
            std::unique_ptr<ThumbDone> d(reinterpret_cast<ThumbDone*>(lp));
            bool used = false;
            for (auto& c : g_clips)
                if (c.id == d->id && c.thumb.empty()) { c.thumb = d->thumb; used = true; }
            if (used) save_index(); else drop_file(d->thumb);
            if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_READ) {
                KillTimer(h, TIMER_READ);
                record();
                return 0;
            }
            if (wp == TIMER_PASTE) {
                KillTimer(h, TIMER_PASTE);
                send_ctrl_v();
                g_pasting = false;
                if (!recording_on()) DestroyWindow(h);   // 記録しない設定なら、使い終わったら閉じる
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(h) && !g_pasting) {
                hide();
                if (!recording_on()) DestroyWindow(h);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (g_pop >= 0) return 0;
            const int r = row_at(GET_Y_LPARAM(lp));
            if (r != g_hot && r >= 0) { g_hot = r; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (g_pop >= 0) {
                const Popup p = popup_geom();
                const int n = g_pop;
                if (in(p.paste, x, y)) paste(index_of_row(n));
                else if (in(p.pin, x, y)) toggle_pin(n);
                else if (in(p.del, x, y)) remove_row(n);
                else if (!in(p.box, x, y)) g_pop = -1;   // 外を押したら閉じる
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (in(clear_btn(), x, y)) { clear_unpinned(); InvalidateRect(h, nullptr, FALSE); return 0; }
            const int r = row_at(y);
            if (r >= 0) paste(index_of_row(r));
            return 0;
        }
        case WM_RBUTTONDOWN: {
            const int r = row_at(GET_Y_LPARAM(lp));
            g_pop = g_pop >= 0 ? -1 : r;
            if (r >= 0) g_hot = r;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (g_pop < 0) {
                g_scroll = std::clamp(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * row_h() / WHEEL_DELTA, 0, max_scroll());
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN: {
            const int n = int(g_clips.size());
            if (wp == VK_ESCAPE) { if (g_pop >= 0) g_pop = -1; else hide(); }
            else if (wp == VK_DOWN && n && g_pop < 0) { g_hot = std::min(n - 1, g_hot + 1); ensure_visible(g_hot); }
            else if (wp == VK_UP && n && g_pop < 0) { g_hot = std::max(0, g_hot - 1); ensure_visible(g_hot); }
            else if (wp == VK_RETURN && g_hot >= 0) paste(index_of_row(g_pop >= 0 ? g_pop : g_hot));
            else if (wp == VK_DELETE && g_hot >= 0) remove_row(g_pop >= 0 ? g_pop : g_hot);
            else if (wp == VK_APPS || (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0)) { g_pop = g_pop >= 0 ? -1 : g_hot; if (g_pop >= 0) ensure_visible(g_pop); }
            else if (wp >= '1' && wp <= '9' && g_pop < 0) { const int r = int(wp - '1'); if (r < n) paste(index_of_row(r)); return 0; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SYSKEYDOWN:
            if (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0) { g_pop = g_pop >= 0 ? -1 : g_hot; InvalidateRect(h, nullptr, FALSE); return 0; }
            break;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            make_fonts();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CLOSE: hide(); return 0;
        case WM_DESTROY:
            RemoveClipboardFormatListener(h);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool show = false;
    HWND target = nullptr;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--show") == 0) {
            show = true;
            if (i + 1 < argc) target = reinterpret_cast<HWND>(static_cast<INT_PTR>(_wcstoui64(argv[i + 1], nullptr, 10)));
        }
    }
    if (argv) LocalFree(argv);
    if (!target) target = GetForegroundWindow();

    // 1 つだけ常駐する。2 つ目は、常駐しているものに「窓を出して」と頼むだけ
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\shunti_clip");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (show) {
            if (HWND w = FindWindowW(CLASS, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, WM_SHOWLIST, WPARAM(target), 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    PWSTR app = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) {
        g_settings = fs::path(app) / L"shunti IME" / L"settings.json";
        g_dir = fs::path(app) / L"shunti IME" / L"clips";
    }
    CoTaskMemFree(app);
    if (g_dir.empty()) return 1;
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    if (!show && !recording_on()) return 0;   // 記録しない設定: 常駐しない

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gp::GdiplusStartupInput gsi;
    ULONG_PTR gtoken = 0;
    gp::GdiplusStartup(&gtoken, &gsi, nullptr);
    CF_PNG = RegisterClipboardFormatW(L"PNG");
    CF_EXCLUDE = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    CF_CANINCLUDE = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    CF_IGNORE = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    CF_DROPEFFECT = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    load_index();
    prune();

    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.lpszClassName = CLASS;
    wc.style = CS_DROPSHADOW;
    RegisterClassW(&wc);
    g_dpi = GetDpiForSystem();
    make_fonts();
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CLASS, T(204), WS_POPUP | WS_BORDER, 0, 0, px(380), px(470), nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    AddClipboardFormatListener(g_wnd);
    if (show) show_list(target);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_imgs.clear();
    gp::GdiplusShutdown(gtoken);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}

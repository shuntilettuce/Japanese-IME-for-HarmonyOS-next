// shunti IME の手書き入力 (shunti_hand.exe)。タスクバーの Zori-chan を右クリック →「手書き入力」で開く。
// 認識は Windows の日本語の手書き認識 (Windows.UI.Input.Inking の InkRecognizerContainer) を呼ぶだけで、モデルは持たない。
// 窓は WS_EX_NOACTIVATE なので、書いても入力欄からフォーカスが外れない。候補を押すと、その字を SendInput (KEYEVENTF_UNICODE)
// で今の入力欄に送る。IME は VK_PACKET を素通しする (入力中なら先に確定する。text_service.cpp の classify)
#include <windows.h>
#include <windowsx.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.Inking.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "strings.h"

using namespace shunti;
namespace ink = winrt::Windows::UI::Input::Inking;
namespace wf = winrt::Windows::Foundation;

namespace {

constexpr UINT WM_RESULT = WM_APP + 1;   // 認識の結果 (lParam = new std::vector<std::wstring>*、wParam = 何回目の認識か)
constexpr UINT_PTR TIMER_RECOG = 1;      // 書き終えてから少し待って認識する
constexpr UINT RECOG_DELAY_MS = 350;
constexpr int N_CAND = 10;               // 並べる候補の数

struct Stroke { std::vector<POINT> pts; };

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_big = nullptr;
UINT g_dpi = 96;
std::vector<Stroke> g_strokes;
bool g_drawing = false;
std::vector<std::wstring> g_cands;
std::atomic<int> g_gen{0};               // 書き足すたびに増やす。古い認識の結果は捨てる
std::wstring g_error;                    // 認識できないとき (日本語の手書きが入っていない など) の案内

int px(int v) { return MulDiv(v, int(g_dpi), 96); }

// 窓の中の区画 (DPI に合わせて毎回計算する)
struct Layout { RECT canvas, cands[N_CAND], clear, undo; };
Layout layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    Layout l{};
    const int m = px(8), btn = px(34), side = px(64);
    const int w = rc.right - m * 3 - side;
    l.canvas = {m, m, m + w, rc.bottom - m * 2 - btn};
    const int cw = (rc.right - m * 2) / N_CAND;
    for (int i = 0; i < N_CAND; i++)
        l.cands[i] = {m + cw * i, rc.bottom - m - btn, m + cw * (i + 1) - px(2), rc.bottom - m};
    const int x = l.canvas.right + m, bh = px(40);
    l.undo = {x, m, x + side, m + bh};
    l.clear = {x, m + bh + px(6), x + side, m + bh * 2 + px(6)};
    return l;
}

bool in(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// 認識は別のスレッドで (C++/WinRT の .get() は UI のスレッド (STA) では待てない)
void recognize() {
    const int gen = ++g_gen;
    std::vector<Stroke> strokes = g_strokes;
    if (strokes.empty()) {
        g_cands.clear();
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    HWND wnd = g_wnd;
    std::thread([gen, strokes = std::move(strokes), wnd] {
        auto* out = new std::vector<std::wstring>();
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            ink::InkRecognizerContainer rec;
            // 日本語の認識器を選ぶ (名前に「日本語」か「Japanese」が入っているもの)
            bool found = false;
            for (const auto& r : rec.GetRecognizers()) {
                const std::wstring name(r.Name());
                if (name.find(L"日本語") != std::wstring::npos || name.find(L"Japanese") != std::wstring::npos) {
                    rec.SetDefaultRecognizer(r);
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("no ja recognizer");
            ink::InkStrokeBuilder builder;
            ink::InkStrokeContainer box;
            for (const auto& s : strokes) {
                std::vector<wf::Point> pts;
                for (const auto& p : s.pts) pts.push_back({float(p.x), float(p.y)});
                if (pts.size() == 1) pts.push_back({pts[0].X + 1, pts[0].Y + 1});   // 点 1 つでは線にならない
                box.AddStroke(builder.CreateStroke(winrt::single_threaded_vector(std::move(pts))));
            }
            auto results = rec.RecognizeAsync(box, ink::InkRecognitionTarget::All).get();
            // 1 字ずつ書く前提: 最初の結果の候補を並べる (2 つ以上に分かれたら、それぞれの 1 位をつないだものを先頭に)
            if (results.Size() > 1) {
                std::wstring joined;
                for (const auto& r : results) {
                    auto c = r.GetTextCandidates();
                    if (c.Size()) joined += std::wstring(c.GetAt(0));
                }
                if (!joined.empty()) out->push_back(joined);
            }
            if (results.Size()) {
                for (const auto& c : results.GetAt(0).GetTextCandidates()) {
                    std::wstring t(c);
                    if (!t.empty() && std::find(out->begin(), out->end(), t) == out->end()) out->push_back(t);
                    if (out->size() >= size_t(N_CAND)) break;
                }
            }
        } catch (...) {
            out->clear();
            out->push_back(L"\x1");   // しるし: 認識できない
        }
        PostMessageW(wnd, WM_RESULT, WPARAM(gen), LPARAM(out));
    }).detach();
}

// 今フォーカスのある入力欄へ字を送る (この窓はフォーカスを取らないので、送り先は書く前の入力欄のまま)
void send_text(const std::wstring& t) {
    std::vector<INPUT> in;
    for (wchar_t c : t) {
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wScan = c;
        d.ki.dwFlags = KEYEVENTF_UNICODE;
        in.push_back(d);
        d.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(d);
    }
    if (!in.empty()) SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

void clear_all() {
    g_strokes.clear();
    g_cands.clear();
    ++g_gen;
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void draw_button(HDC dc, const RECT& r, const wchar_t* text, HFONT f, bool on) {
    HBRUSH b = CreateSolidBrush(on ? RGB(232, 240, 254) : RGB(245, 245, 245));
    FillRect(dc, &r, b);
    DeleteObject(b);
    FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    RECT t = r;
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void paint(HDC dc0) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    // ちらつかないよう、裏で描いてから写す
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const Layout l = layout();
    // 書く所: 薄い十字の補助線
    HBRUSH bg = CreateSolidBrush(RGB(252, 252, 250));
    FillRect(dc, &l.canvas, bg);
    DeleteObject(bg);
    FrameRect(dc, &l.canvas, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    HPEN guide = CreatePen(PS_DOT, 1, RGB(210, 210, 210));
    SelectObject(dc, guide);
    const int cx = (l.canvas.left + l.canvas.right) / 2, cy = (l.canvas.top + l.canvas.bottom) / 2;
    MoveToEx(dc, cx, l.canvas.top, nullptr); LineTo(dc, cx, l.canvas.bottom);
    MoveToEx(dc, l.canvas.left, cy, nullptr); LineTo(dc, l.canvas.right, cy);
    HPEN pen = CreatePen(PS_SOLID, px(4), RGB(30, 30, 30));
    SelectObject(dc, pen);
    for (const auto& s : g_strokes) {
        if (s.pts.empty()) continue;
        MoveToEx(dc, s.pts[0].x, s.pts[0].y, nullptr);
        if (s.pts.size() == 1) LineTo(dc, s.pts[0].x + 1, s.pts[0].y);
        for (size_t i = 1; i < s.pts.size(); i++) LineTo(dc, s.pts[i].x, s.pts[i].y);
    }
    SelectObject(dc, GetStockObject(BLACK_PEN));
    DeleteObject(pen);
    DeleteObject(guide);
    if (g_strokes.empty()) {   // 何も書いていないときの案内
        SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(150, 150, 150));
        RECT t = l.canvas;
        DrawTextW(dc, g_error.empty() ? T(197) : g_error.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    draw_button(dc, l.undo, T(198), g_font, false);
    draw_button(dc, l.clear, T(199), g_font, false);
    for (int i = 0; i < N_CAND; i++)
        draw_button(dc, l.cands[i], i < int(g_cands.size()) ? g_cands[size_t(i)].c_str() : L"", g_big, i == 0 && !g_cands.empty());
    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void make_fonts() {
    if (g_font) DeleteObject(g_font);
    if (g_big) DeleteObject(g_big);
    g_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_big = CreateFontW(-px(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // 押してもフォーカスを取らない (送り先の入力欄のまま)
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const Layout l = layout();
            if (in(l.canvas, x, y)) {
                KillTimer(h, TIMER_RECOG);
                g_strokes.push_back({{{x, y}}});
                g_drawing = true;
                SetCapture(h);
                InvalidateRect(h, &l.canvas, FALSE);
                return 0;
            }
            if (in(l.undo, x, y)) {
                if (!g_strokes.empty()) g_strokes.pop_back();
                recognize();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (in(l.clear, x, y)) { clear_all(); return 0; }
            for (int i = 0; i < N_CAND && i < int(g_cands.size()); i++)
                if (in(l.cands[i], x, y)) {
                    send_text(g_cands[size_t(i)]);
                    clear_all();
                    return 0;
                }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_drawing && !g_strokes.empty()) {
                const Layout l = layout();
                POINT p{std::clamp(int(GET_X_LPARAM(lp)), int(l.canvas.left), int(l.canvas.right) - 1),
                        std::clamp(int(GET_Y_LPARAM(lp)), int(l.canvas.top), int(l.canvas.bottom) - 1)};
                auto& pts = g_strokes.back().pts;
                if (pts.back().x != p.x || pts.back().y != p.y) pts.push_back(p);
                InvalidateRect(h, &l.canvas, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_drawing) {
                g_drawing = false;
                ReleaseCapture();
                SetTimer(h, TIMER_RECOG, RECOG_DELAY_MS, nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_RECOG) {
                KillTimer(h, TIMER_RECOG);
                recognize();
            }
            return 0;
        case WM_RESULT: {
            std::unique_ptr<std::vector<std::wstring>> r(reinterpret_cast<std::vector<std::wstring>*>(lp));
            if (int(wp) != g_gen.load()) return 0;   // 書き足した後の古い結果
            if (r->size() == 1 && (*r)[0] == L"\x1") {
                g_cands.clear();
                g_error = T(201);
                g_strokes.clear();
            } else {
                g_cands = std::move(*r);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
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
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 2 つ目は開かない: 開いている窓を前に出すだけ
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"shunti_hand_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"shunti_hand", nullptr)) ShowWindow(w, SW_SHOWNOACTIVATE);
        return 0;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.lpszClassName = L"shunti_hand";
    RegisterClassW(&wc);
    g_dpi = GetDpiForSystem();
    make_fonts();
    // 画面の右下 (作業領域の中) に出す
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT r{0, 0, px(440), px(330)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_dpi);
    const int w = r.right - r.left, hgt = r.bottom - r.top;
    g_wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"shunti_hand", T(196),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, wa.right - w - px(16), wa.bottom - hgt - px(16), w, hgt,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}

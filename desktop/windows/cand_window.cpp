#include "cand_window.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "../core/composer.h"
#include "globals.h"
#include "strings.h"

namespace shunti {
namespace win {

namespace {
constexpr wchar_t CLASS_NAME[] = L"ShuntiImeCandidates";
constexpr wchar_t TREE_CLASS_NAME[] = L"ShuntiImeCandidateTree";

struct Colors { COLORREF bg, border, text, sub, sel_bg, sel_text, accent; };
// Android 版・HarmonyOS 版の色 (LIGHT_THEME・DARK_THEME) に合わせる
Colors colors(const std::string& theme) {
    if (theme == "dark" || (theme != "light" && apps_dark())) return {RGB(0x2A, 0x2A, 0x2A), RGB(0x41, 0x41, 0x41), RGB(0xF2, 0xF2, 0xF7), RGB(0x9A, 0x9A, 0xA0),
                             RGB(0x27, 0x40, 0x70), RGB(0xFF, 0xFF, 0xFF), RGB(0x5B, 0x9B, 0xFF)};
    return {RGB(0xFB, 0xFB, 0xFC), RGB(0xD7, 0xD8, 0xDB), RGB(0x1C, 0x1C, 0x1E), RGB(0x7A, 0x7F, 0x87),
            RGB(0xDC, 0xE7, 0xFD), RGB(0x1C, 0x1C, 0x1E), RGB(0x1E, 0x54, 0xC7)};
}

const wchar_t* W(const u16& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

bool register_class() {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_IME | CS_DROPSHADOW;
    wc.lpfnWndProc = DefWindowProcW;   // 作ったあとで CandWindow::proc に差し替える
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    done = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return done;
}
}  // namespace

CandWindow::~CandWindow() { destroy(); }

void CandWindow::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (thwnd_) DestroyWindow(thwnd_);
    thwnd_ = nullptr;
    if (font_) DeleteObject(font_);
    if (small_) DeleteObject(small_);
    font_ = small_ = nullptr;
    for (HFONT* f : {&tfont_, &tsmall_, &talt_}) { if (*f) DeleteObject(*f); *f = nullptr; }
    dpi_ = 0;
}

void CandWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    if (thwnd_) ShowWindow(thwnd_, SW_HIDE);
}

LRESULT CALLBACK CandWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<CandWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;   // 触っても入力欄からフォーカスを奪わない
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (self) self->paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONUP:
            if (self && self->on_sugg_click) {
                int i = self->hit_sugg(short(LOWORD(l)) - self->list_x_, short(HIWORD(l)) - self->tree_h_);
                if (i >= 0) { self->on_sugg_click(i); return 0; }
            }
            if (self && self->on_click && !self->tree_.empty()) {
                const POINT pt = {short(LOWORD(l)), short(HIWORD(l))};
                for (size_t k = 0; k < self->tcands_.size(); k++)
                    if (PtInRect(&self->tcands_[k].r, pt)) { self->on_click(self->tcand_idx_[k]); return 0; }
                return 0;
            }
            if (self && self->on_click) {
                int i = self->hit(short(LOWORD(l)) - self->list_x_, short(HIWORD(l)) - self->tree_h_);
                if (i >= 0) self->on_click(i);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

void CandWindow::show(const std::vector<u16>& cands, int sel, const RECT& anchor, HWND owner, const std::vector<u16>& sugg,
                      int sugg_sel, const std::vector<u16>& notes) {
    notes_ = notes;
    if (cands.empty() && sugg.empty() && tree_.empty()) { hide(); return; }
    sugg_sel_ = sugg_sel;
    sugg_ = sugg;
    if (!register_class()) return;
    if (!hwnd_ || owner_ != owner) {
        if (hwnd_) DestroyWindow(hwnd_);
        if (thwnd_) DestroyWindow(thwnd_);
        thwnd_ = nullptr;
        owner_ = owner;
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_NAME, L"", WS_POPUP, 0, 0, 1, 1,
                                owner, nullptr, g_inst, nullptr);
        if (!hwnd_) return;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, LONG_PTR(this));
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, LONG_PTR(&CandWindow::proc));
        DWORD corner = 3;   // DWMWCP_ROUNDSMALL (Windows 11 の角の丸み)
        DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    }
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (!dpi) dpi = GetDpiForSystem();
    if (dpi != dpi_) {
        if (font_) DeleteObject(font_);
        if (small_) DeleteObject(small_);
        dpi_ = dpi;
        font_ = CreateFontW(-MulDiv(16, int(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        small_ = CreateFontW(-MulDiv(12, int(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        for (HFONT* f : {&tfont_, &tsmall_, &talt_}) if (*f) DeleteObject(*f);
        tfont_ = CreateFontW(-MulDiv(14, int(dpi), 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        tsmall_ = CreateFontW(-MulDiv(9, int(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        talt_ = CreateFontW(-MulDiv(12, int(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    }
    cands_ = cands;
    sel_ = std::clamp(sel, -1, int(cands.size()) - 1);   // -1 = まだ選んでいない (打っている間の候補)
    page_ = std::max(sel_, 0) / Composer::PAGE;
    layout();

    // 文節の下に出す (候補の字の頭を文節の頭にそろえる)。画面の下に入らなければ上に
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int x = tree_.empty() ? anchor.left - pad_ - num_w_ : anchor.left - MulDiv(14, int(dpi_), 96);   // 木は最初の札を文字の頭に
    int y = anchor.bottom + MulDiv(2, int(dpi_), 96);
    if (y + height_ > wa.bottom) y = anchor.top - height_ - MulDiv(2, int(dpi_), 96);
    x = std::max(int(wa.left), std::min(x, int(wa.right) - width_));
    y = std::max(int(wa.top), y);
    if (!tree_.empty()) {   // 候補の木は背景の無い窓で
        ShowWindow(hwnd_, SW_HIDE);
        present_tree(x, y);
        return;
    }
    if (thwnd_) ShowWindow(thwnd_, SW_HIDE);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandWindow::layout() {
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    row_h_ = tm.tmHeight + MulDiv(8, int(dpi_), 96);
    pad_ = MulDiv(8, int(dpi_), 96);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"9", 1, &sz);
    num_w_ = sz.cx + MulDiv(12, int(dpi_), 96);
    int text_w = MulDiv(120, int(dpi_), 96);
    // いちばん下の行: 左に「提案 Shift+↓」+ 札 (小さい字)、右に「Tab で選ぶ n 件」。幅が足りなければ窓を広げる (640 まで)
    sugg_x_.clear();
    int foot_need = 0;
    SelectObject(dc, small_);
    TEXTMETRICW st;
    GetTextMetricsW(dc, &st);
    const int pill_h = st.tmHeight + MulDiv(10, int(dpi_), 96);
    SelectObject(dc, font_);
    if (!sugg_.empty()) {
        SelectObject(dc, small_);
        SIZE lz, fz;
        const std::wstring label = sugg_label();
        GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &lz);
        const std::wstring foot = foot_text(int(cands_.size()));
        GetTextExtentPoint32W(dc, foot.c_str(), int(foot.size()), &fz);
        int x = pad_ + lz.cx + MulDiv(8, int(dpi_), 96);
        for (auto& s : sugg_) {
            SIZE z;
            GetTextExtentPoint32W(dc, W(s), int(s.size()), &z);
            int w = z.cx + MulDiv(14, int(dpi_), 96);
            sugg_x_.push_back({x, x + w});
            x += w + MulDiv(6, int(dpi_), 96);
        }
        foot_need = x + MulDiv(10, int(dpi_), 96) + fz.cx + pad_;
        text_w = std::max(text_w, foot_need - pad_ * 3 - num_w_);
        SelectObject(dc, font_);
    }
    int start = page_ * Composer::PAGE;
    int end = std::min(int(cands_.size()), start + Composer::PAGE);
    for (int i = start; i < end; i++) {
        GetTextExtentPoint32W(dc, W(cands_[size_t(i)]), int(cands_[size_t(i)].size()), &sz);
        int nw = 0;   // 右に添える種類の幅
        if (size_t(i) < notes_.size() && !notes_[size_t(i)].empty()) {
            SIZE ns;
            const u16& n = notes_[size_t(i)];
            SelectObject(dc, small_);
            GetTextExtentPoint32W(dc, W(n), int(n.size()), &ns);
            SelectObject(dc, font_);
            nw = ns.cx + MulDiv(12, int(dpi_), 96);
        }
        text_w = std::max(text_w, int(sz.cx) + nw);
    }
    text_w = std::min(text_w, MulDiv(640, int(dpi_), 96));
    SelectObject(dc, small_);
    GetTextMetricsW(dc, &tm);
    footer_h_ = sugg_.empty() ? tm.tmHeight + MulDiv(6, int(dpi_), 96) : pill_h + MulDiv(2, int(dpi_), 96);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    width_ = pad_ + num_w_ + text_w + pad_ * 2;
    if (cands_.empty() && sugg_.empty()) footer_h_ = 0;
    height_ = cands_.empty() ? footer_h_ : pad_ / 2 + row_h_ * (end - start) + footer_h_;
    // 候補の木の帯を上に。候補の一覧は選んでいる文節の札の下にずらす
    tree_w_ = tree_h_ = list_x_ = 0;
    tseps_.clear();
    if (!tree_.empty()) {
        HDC tdc = GetDC(hwnd_);
        layout_tree(tdc);
        ReleaseDC(hwnd_, tdc);
    }
    lw_ = width_;
    lh_ = height_;
    if (!tree_.empty()) lw_ = lh_ = 0;   // 候補は木の帯の中 (選んでいる札の下) に出す
    if (lh_ == 0) list_x_ = 0;
    width_ = std::max(tree_w_, list_x_ + lw_);
    height_ = tree_h_ + lh_;
}

namespace {
constexpr int TB_R = 47, TB_G = 111, TB_B = 237;   // shunti IME の青
COLORREF blend(COLORREF bg, float a) {
    a = std::clamp(a, 0.f, 1.f);
    return RGB(int(GetRValue(bg) + (TB_R - GetRValue(bg)) * a), int(GetGValue(bg) + (TB_G - GetGValue(bg)) * a),
               int(GetBValue(bg) + (TB_B - GetBValue(bg)) * a));
}
bool ensure_gdiplus() {
    static ULONG_PTR token = 0;
    if (!token) {
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) token = 0;
    }
    return token != 0;
}
// 字の幅と高さ。描くのと同じ GDI+ の計り方で (GDI で計ると GDI+ で描いた字がはみ出して … で切れた)
SIZE text_size(HDC dc, HFONT f, const u16& t) {
    SIZE z = {};
    if (!ensure_gdiplus()) {
        HGDIOBJ o = SelectObject(dc, f);
        GetTextExtentPoint32W(dc, W(t), int(t.size()), &z);
        SelectObject(dc, o);
        return z;
    }
    Gdiplus::Graphics g(dc);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Font font(dc, f);
    Gdiplus::RectF out;
    g.MeasureString(W(t), INT(t.size()), &font, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &out);
    z.cx = LONG(std::ceil(out.Width));
    z.cy = LONG(std::ceil(font.GetHeight(&g)));
    return z;
}
// 強さの色: 弱い 赤 → オレンジ → 黄 → 黄緑 → 青 強い (青の濃さだけでは見分けにくい。ユーザー 2026-10-20)。
// 木に無い候補 (強さが分からない = ほぼ 0) は赤
COLORREF strength_color(float s) {
    struct Stop { float at; int r, g, b; };
    static const Stop st[] = {{0.f, 0xE5, 0x39, 0x35}, {0.15f, 0xFF, 0x8A, 0x1F}, {0.4f, 0xFF, 0xD2, 0x1A},
                              {0.7f, 0x8B, 0xD3, 0x2E}, {1.f, TB_R, TB_G, TB_B}};
    s = std::clamp(s, 0.f, 1.f);
    for (int i = 0; i < 4; i++) {
        if (s > st[i + 1].at) continue;
        const float t = (s - st[i].at) / (st[i + 1].at - st[i].at);
        return RGB(int(st[i].r + (st[i + 1].r - st[i].r) * t), int(st[i].g + (st[i + 1].g - st[i].g) * t), int(st[i].b + (st[i + 1].b - st[i].b) * t));
    }
    return RGB(TB_R, TB_G, TB_B);
}
COLORREF mix(COLORREF a, COLORREF b, float t) {
    return RGB(int(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), int(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               int(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
// 地の色に合う字の色 (明るい地なら濃い字)
COLORREF ink_on(COLORREF bg) {
    const float l = (0.299f * GetRValue(bg) + 0.587f * GetGValue(bg) + 0.114f * GetBValue(bg)) / 255.f;
    return l > 0.58f ? RGB(0x1D, 0x24, 0x33) : RGB(255, 255, 255);
}

u16 percent(float s) {
    const std::string n = std::to_string(int(std::lround(s * 100)));
    return u16(n.begin(), n.end());
}
}  // namespace

// 小さな札 (別の候補) の四角: 字の後ろに強さの数字を小さく (s < 0 なら数字なし)。cx = 中央、top = 字の上
RECT CandWindow::pill_rect(HDC dc, const u16& text, float s, int cx, int top) const {
    const SIZE z = text_size(dc, talt_, text);
    const int nw = s >= 0 ? int(text_size(dc, tsmall_, percent(s)).cx) + MulDiv(4, int(dpi_), 96) : 0;
    const int w = z.cx + nw + MulDiv(10, int(dpi_), 96);
    return {cx - w / 2, top - MulDiv(1, int(dpi_), 96), cx - w / 2 + w, top + z.cy + MulDiv(1, int(dpi_), 96)};
}

// 候補の木の帯: 文節の札をピンと張った線でつなぎ、札の色 = 語の強さ (strength_color)、線の色と太さ = 前の語との結びつき。
// 選んでいる札の下に、その文節の候補を強さの数字つきで縦に並べる (二股の頭なら別の区切りの文も強さの順に混ぜる)。
// 選んでいる候補からは、基板の配線のように本線へケーブルを引く (今の文がどの候補を通っているか)。
// 確信している語 (90 以上) は別の候補を出さない。選んでいない二股は、区間の下に別の区切りを薄いセルで出す
void CandWindow::layout_tree(HDC dc) {
    auto d = [&](int v) { return MulDiv(v, int(dpi_), 96); };
    tchips_.clear();
    ttexts_.clear();
    tlines_.clear();
    tdots_.clear();
    tpaths_.clear();
    tcands_.clear();
    tcand_idx_.clear();
    const int H = d(22), GAP = d(12), pad = d(8), FH = d(20);
    const int Y = pad + H / 2;
    int maxY = Y + H / 2 + d(16), maxX = 0;
    bool list_set = false, col_set = false;
    struct FCell { RECT r; const TreeWord* w; };
    std::vector<FCell> fcells;   // 選んでいない二股の別の区切り (y は後で、いちばん下に)
    // 選んでいる札が入っている二股の区間の右端 (別の区切りの候補の行をそこまで伸ばす)
    int fork_right = -1;
    auto chip = [&](const TreeWord& w, int x, int y) {
        const bool sel = w.rs < tree_re_ && w.re > tree_rs_;
        // 選んでいる文節の頭の札 = 候補の 1 つ目 (候補の列はこの札の下に続く。選んでいる候補には枠とケーブル)
        const bool head = sel && !cands_.empty() && !col_set;
        const u16& text = head ? cands_[0] : w.surface;
        const float st = head && !scores_.empty() && scores_[0] >= 0 ? scores_[0] : w.s;
        const SIZE z = text_size(dc, tfont_, text);
        const int nw = st >= 0 ? int(text_size(dc, tsmall_, percent(st)).cx) + d(4) : 0;   // 強さの数字 (字の後ろに小さく)
        const int wd = std::max(d(26), int(z.cx) + nw + d(14));
        tchips_.push_back({{x, y - H / 2, x + wd, y + H / 2}, text, st, head ? sel_ <= 0 : (sel && cands_.empty()), false});
        if (sel && !list_set) { list_x_ = std::max(0, x - pad_ - num_w_ + d(4)); list_set = true; }
        if (sel && !cands_.empty() && !col_set) {
            col_set = true;
            colx_ = x;
            colw_ = wd;
            return wd;   // 候補の列は後で (二股の区間の幅が分かってから)
        }
        if (w.s < 0) return wd;   // 強さが分からない札 (点数も別の候補も出さない)
        if (w.s < 0.9f) {   // 確信していない語だけ別の候補を
            int yy = y + H / 2 + d(6), k = 0;
            for (auto& a : w.alts) {
                if (a.second < 0.03f || k >= 4) continue;
                k++;
                ttexts_.push_back({x + wd / 2, yy, a.first, false, a.second});
                {   // 選んでいない別の候補も灰色の線で本線につなぐ (札の左から直角に降りる)
                    const RECT pr = pill_rect(dc, a.first, a.second, x + wd / 2, yy);
                    const float cy = (pr.top + pr.bottom) / 2.f, x0 = float(x - d(6)), c45 = float(d(4));
                    const float left = float(pr.left);
                    if (left > x0 + c45) tpaths_.push_back({{{x0, float(Y)}, {x0, cy - c45}, {x0 + c45, cy}, {left, cy}}, a.second, true, x + wd / 2});
                }
                yy += d(24);
                maxY = std::max(maxY, yy);
            }
        }
        return wd;
    };
    int x = pad + d(6);
    float px = float(pad), py = float(Y);
    tdots_.push_back({px, py});
    for (auto& n : tree_) {
        if (n.main.empty()) continue;
        bool focus_here = false;
        for (auto& w : n.main) focus_here |= w.rs < tree_re_ && w.re > tree_rs_;
        const int fx = x;
        int tx = fx;
        for (size_t j = 0; j < n.main.size(); j++) {
            tlines_.push_back({j ? float(tx - GAP) : px, j ? float(Y) : py, float(tx), float(Y), n.main[j].link});
            tx += chip(n.main[j], tx, Y) + GAP;
        }
        tx -= GAP;
        int end = tx;
        if (!n.fork.empty()) {
            if (focus_here) {
                fork_right = tx;
            } else {   // 区間の幅を読みの長さで分けたセル (字が入らなければ区間を広げる)
                const int gap = d(2);
                std::vector<int> need;
                int sum = 0, rl = 0;
                for (auto& w : n.fork) {
                    need.push_back(int(text_size(dc, talt_, w.surface).cx) + d(12));
                    sum += need.back();
                    rl += w.re - w.rs;
                }
                const int W = std::max(tx - fx, sum + gap * int(n.fork.size() - 1));
                const int extra = W - sum - gap * int(n.fork.size() - 1);
                int cx = fx;
                for (size_t j = 0; j < n.fork.size(); j++) {
                    const auto& w = n.fork[j];
                    int cw = need[j] + (rl > 0 ? extra * (w.re - w.rs) / rl : 0);
                    if (j + 1 == n.fork.size()) cw = fx + W - cx;
                    fcells.push_back({{cx, 0, cx + cw, FH}, &w});
                    cx += cw + gap;
                }
                end = std::max(tx, fx + W);
            }
        }
        px = float(tx);
        py = float(Y);
        x = end + GAP;
    }
    // 選んでいる文節の候補の列: 最初は木の候補 (と別の区切り) だけ。その下で ↓ を押すと残りを 5 個ずつ
    if (col_set) {
        const int n = int(cands_.size());
        const bool brief = short_ > 0 && short_ < n;
        std::vector<int> rows;
        if (brief) {
            for (int i = 0; i < short_; i++) rows.push_back(i);
            if (sel_ >= short_) {   // 木の候補の下の残り: 5 個ずつ (木の候補は上に残したまま。ユーザー 2026-10-20「5 個ぐらいずつ」)
                constexpr int MORE = 5;
                const int st = short_ + (sel_ - short_) / MORE * MORE;
                rows.push_back(-1);   // 区切りの線
                for (int i = st; i < std::min(n, st + MORE); i++) rows.push_back(i);
            }
        } else {
            const int st = page_ * Composer::PAGE;
            for (int i = st; i < std::min(n, st + Composer::PAGE); i++) rows.push_back(i);
        }
        // 候補はほかの札の別の候補と同じ小さな札で、札の下の中央に (目立たせない。隣の札の候補を隠さない)
        const int cx = colx_ + colw_ / 2;
        int yy = Y + H / 2 + d(6);
        for (int i : rows) {
            if (i == 0) continue;   // 1 つ目は上の札
            if (i < 0) { tseps_.push_back(yy - d(4)); yy += d(4); continue; }
            const float sc = size_t(i) < scores_.size() ? scores_[size_t(i)] : -1.f;
            RECT rc = pill_rect(dc, cands_[size_t(i)], sc, cx, yy);
            if (i == fork_ && fork_right > colx_) {
                // 別の区切りの文は区切りが変わる範囲の幅まで伸ばす (結合したセル)。隣の札の候補を 1 つだけ潰すなら潰してよい
                // (ユーザー 2026-10-20)。2 つ以上にかかるなら伸ばさない
                const RECT wide = {colx_, rc.top, std::max(int(rc.right), fork_right), rc.bottom};
                std::vector<size_t> hit;
                for (size_t k = 0; k < ttexts_.size(); k++) {
                    const TText& t = ttexts_[k];
                    if (t.tiny || t.x == cx) continue;
                    const RECT ar = pill_rect(dc, t.text, t.s, t.x, t.y);
                    RECT o;
                    if (IntersectRect(&o, &ar, &wide)) hit.push_back(k);
                }
                if (hit.size() <= 1) {
                    if (!hit.empty()) {   // 潰す候補と、その点数 (すぐ下の小さな字) と線を外す
                        const TText v = ttexts_[hit[0]];
                        const RECT vr = pill_rect(dc, v.text, v.s, v.x, v.y);
                        const float vcy = (vr.top + vr.bottom) / 2.f;
                        std::vector<TText> kt;
                        for (auto& t : ttexts_)
                            if (!(t.x == v.x && (t.y == v.y || (t.tiny && t.y == v.y + d(16))))) kt.push_back(t);
                        ttexts_ = std::move(kt);
                        std::vector<TPath> kp;
                        for (auto& t : tpaths_)
                            if (!(t.owner == v.x && std::fabs(t.pts.back().second - vcy) < 1.f)) kp.push_back(t);
                        tpaths_ = std::move(kp);
                    }
                    rc = wide;
                }
            }
            tcands_.push_back({rc, cands_[size_t(i)], sc, i == sel_, i == fork_});
            tcand_idx_.push_back(i);
            maxX = std::max(maxX, int(rc.right));
            yy += d(24);
        }
        if (brief && sel_ < short_) {
            const std::wstring h = T(158);
            ttexts_.push_back({cx, yy - d(2), u16(h.begin(), h.end()), true, 0});
            yy += d(12);
        } else if (!brief && n > Composer::PAGE) {
            const std::string s = std::to_string(std::max(sel_, 0) + 1) + " / " + std::to_string(n);
            ttexts_.push_back({cx, yy - d(4), u16(s.begin(), s.end()), true, 0});
            yy += d(10);
        }
        maxY = std::max(maxY, yy + d(2));
        // 本線から各候補へ直角に降りる線 (選んでいる候補だけ青いケーブル、ほかは灰色)
        for (auto& t : tcands_) {
            const float cy = (t.r.top + t.r.bottom) / 2.f, c45 = float(d(4));
            const float x0 = std::min(float(colx_ - d(6)), float(t.r.left) - c45 - d(2));
            tpaths_.push_back({{{x0, float(Y)}, {x0, cy - c45}, {x0 + c45, cy}, {float(t.r.left), cy}}, std::max(t.s, 0.f), !t.sel});
        }
    }
    if (!fcells.empty()) {   // 選んでいない二股の別の区切り: いちばん下に、区間の幅のセルで
        const int top = maxY + d(6);
        for (auto& f : fcells) {
            RECT r = f.r;
            OffsetRect(&r, 0, top);
            tchips_.push_back({r, f.w->surface, f.w->s, false, true});
        }
        maxY = top + FH + d(4);
    }
    tree_w_ = std::max(x, maxX) + pad;
    tree_h_ = maxY + pad / 2;
}

// 候補の木の窓: 背景の無い窓 (札と線だけが浮いて見える)。GDI+ で透明度つきの絵を描き、UpdateLayeredWindow で出す
void CandWindow::present_tree(int wx, int wy) {
    if (!ensure_gdiplus()) return;
    if (!thwnd_) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc = {sizeof wc};
            wc.style = CS_IME;   // 影は付けない (札の形でなく窓の四角に付くため)
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = TREE_CLASS_NAME;
            reg = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        thwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TREE_CLASS_NAME, L"", WS_POPUP, 0, 0, 1, 1,
                                 owner_, nullptr, g_inst, nullptr);
        if (!thwnd_) return;
        SetWindowLongPtrW(thwnd_, GWLP_USERDATA, LONG_PTR(this));
        SetWindowLongPtrW(thwnd_, GWLP_WNDPROC, LONG_PTR(&CandWindow::proc));
    }
    const int W = std::max(1, width_), Hh = std::max(1, height_);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -Hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(mdc); ReleaseDC(nullptr, sdc); return; }
    HGDIOBJ ob = SelectObject(mdc, dib);
    {
        Gdiplus::Bitmap canvas(W, Hh, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        paint_tree(g, mdc);
    }
    POINT src = {0, 0}, dst = {wx, wy};
    SIZE sz = {W, Hh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(thwnd_, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(thwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(mdc, ob);
    DeleteObject(dib);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}

void CandWindow::paint_tree(Gdiplus::Graphics& g, HDC dc) {
    auto d = [&](float v) { return v * float(dpi_) / 96.f; };
    const Colors c = colors(theme);
    const bool dark = theme == "dark" || (theme != "light" && apps_dark());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    auto col = [&](COLORREF r, int a = 255) { return Gdiplus::Color(BYTE(a), GetRValue(r), GetGValue(r), GetBValue(r)); };
    auto rounded = [&](Gdiplus::GraphicsPath& path, const RECT& rc) {
        const float x = float(rc.left), y = float(rc.top), w = float(rc.right - rc.left), h = float(rc.bottom - rc.top), r = d(8);
        path.AddArc(x, y, r, r, 180, 90);
        path.AddArc(x + w - r, y, r, r, 270, 90);
        path.AddArc(x + w - r, y + h - r, r, r, 0, 90);
        path.AddArc(x, y + h - r, r, r, 90, 90);
        path.CloseFigure();
    };
    const Gdiplus::Color outline = dark ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 11, 42, 107);
    Gdiplus::Font fbig(dc, tfont_), falt(dc, talt_), ftiny(dc, tsmall_);
    Gdiplus::FontFamily fam;
    fbig.GetFamily(&fam);
    // 浮いている字 (点数・別の候補・案内): 背景が無いので、テーマの背景色のふちどりを付けて読めるように
    auto halo_text = [&](const u16& t, Gdiplus::Font& f, float cx, float top, COLORREF fg) {
        Gdiplus::GraphicsPath p;
        Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::FontFamily ff;
        f.GetFamily(&ff);
        const float em = f.GetUnit() == Gdiplus::UnitPoint ? f.GetSize() * float(dpi_) / 72.f : f.GetSize();
        p.AddString(W(t), INT(t.size()), &ff, f.GetStyle(), em, Gdiplus::PointF(cx, top), &sf);
        Gdiplus::Pen halo(col(c.bg, 220), d(3.f));
        halo.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&halo, &p);
        Gdiplus::SolidBrush b(col(fg));
        g.FillPath(&b, &p);
    };
    auto box_text = [&](const u16& t, Gdiplus::Font& f, const RECT& rc, COLORREF fg, Gdiplus::StringAlignment al) {
        Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
        sf.SetAlignment(al);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
        Gdiplus::SolidBrush b(col(fg));
        g.DrawString(W(t), INT(t.size()), &f, Gdiplus::RectF(float(rc.left), float(rc.top), float(rc.right - rc.left), float(rc.bottom - rc.top)), &sf, &b);
    };
    // 小さな札の字: 字の後ろに強さの数字を小さく
    auto pill_text = [&](const u16& t, float s, const RECT& rc, COLORREF fg) {
        if (s < 0) { box_text(t, falt, rc, fg, Gdiplus::StringAlignmentCenter); return; }
        const u16 n = percent(s);
        const int nw = int(text_size(dc, tsmall_, n).cx);
        RECT tr = {rc.left + int(d(4)), rc.top, rc.right - nw - int(d(6)), rc.bottom};
        box_text(t, falt, tr, fg, Gdiplus::StringAlignmentCenter);
        RECT nr = {rc.right - nw - int(d(7)), rc.top + int(d(1)), rc.right - int(d(3)), rc.bottom};
        box_text(n, ftiny, nr, fg, Gdiplus::StringAlignmentCenter);
    };
    // 文節どうしはピンと張った線
    for (auto& l : tlines_) {
        Gdiplus::Pen pen(col(mix(c.bg, strength_color(l.s), 0.45f + 0.55f * l.s)), d(0.8f + 2.4f * l.s));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, l.x1, l.y1, l.x2, l.y2);
    }
    // 候補の列の配線: 選んでいない候補は灰色、選んでいる候補は青 (基板の配線のように直角、角は 45 度。本線側はビア)
    for (int pass = 0; pass < 2; pass++)
        for (auto& t : tpaths_) {
            if (t.gray != (pass == 0)) continue;
            Gdiplus::Pen pen(t.gray ? col(c.sub, 150) : col(strength_color(t.s)), t.gray ? d(1.f) : d(1.2f + 1.6f * t.s));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            std::vector<Gdiplus::PointF> pts;
            for (auto& q : t.pts) pts.push_back({q.first, q.second});
            g.DrawLines(&pen, pts.data(), INT(pts.size()));
            if (!t.gray) {
                Gdiplus::SolidBrush ring(col(blend(c.bg, 0.85f))), hole(col(c.bg));
                const float ro = d(4.f), ri = d(1.8f);
                g.FillEllipse(&ring, t.pts.front().first - ro, t.pts.front().second - ro, 2 * ro, 2 * ro);
                g.FillEllipse(&hole, t.pts.front().first - ri, t.pts.front().second - ri, 2 * ri, 2 * ri);
            }
        }
    for (size_t i = 0; i < tdots_.size(); i++) {
        const float r = d(2.8f);
        Gdiplus::SolidBrush b(col(blend(c.bg, 0.9f)));
        g.FillEllipse(&b, tdots_[i].first - r, tdots_[i].second - r, 2 * r, 2 * r);
    }
    // 選んでいる文節の候補: ほかの札の別の候補と同じ小さな札 (選んでいる候補に枠、別の区切りの文は点線の枠)
    // 小さな札の地: 強さの色 (上の札よりわずかに地に寄せる。強さが分からなければ赤)
    auto pill_fill = [&](float s) { return mix(c.bg, strength_color(std::max(s, 0.f)), 0.85f); };
    for (auto& t : tcands_) {
        const COLORREF fill = pill_fill(t.s);
        Gdiplus::GraphicsPath path;
        rounded(path, t.r);
        Gdiplus::SolidBrush b(col(fill));
        g.FillPath(&b, &path);
        if (t.ghost) {
            Gdiplus::Pen pen(col(blend(c.bg, 0.7f)), d(1.f));
            pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawPath(&pen, &path);
        }
        if (t.sel) {
            Gdiplus::Pen pen(outline, d(1.6f));
            g.DrawPath(&pen, &path);
        }
        pill_text(t.text, t.s, t.r, ink_on(fill));
    }
    for (int y : tseps_) {
        Gdiplus::Pen pen(col(c.sub, 120), d(1.f));
        g.DrawLine(&pen, float(colx_ + d(4)), float(y), float(colx_ + colw_ - d(4)), float(y));
    }
    for (auto& t : tchips_) {
        const COLORREF fill = t.s < 0 ? blend(c.bg, 0.7f) : t.ghost ? mix(c.bg, strength_color(t.s), 0.8f) : strength_color(t.s);
        const COLORREF fg = ink_on(fill);
        Gdiplus::GraphicsPath path;
        rounded(path, t.r);
        Gdiplus::SolidBrush b(col(fill));
        g.FillPath(&b, &path);
        if (t.sel) {
            Gdiplus::Pen pen(outline, d(1.6f));
            g.DrawPath(&pen, &path);
        }
        if (t.ghost || t.s < 0) {
            box_text(t.text, t.ghost ? falt : fbig, t.r, fg, Gdiplus::StringAlignmentCenter);
        } else {   // 字の後ろに強さの数字を小さく
            const u16 n = percent(t.s);
            const int nw = int(text_size(dc, tsmall_, n).cx) + int(d(4));
            const int tw = int(text_size(dc, tfont_, t.text).cx);
            const int left = (t.r.left + t.r.right - tw - nw) / 2;
            RECT tr = {left - int(d(2)), t.r.top, left + tw + int(d(2)), t.r.bottom};
            box_text(t.text, fbig, tr, fg, Gdiplus::StringAlignmentCenter);
            RECT nr = {left + tw + int(d(2)), t.r.top + int(d(2)), left + tw + nw + int(d(2)), t.r.bottom};
            box_text(n, ftiny, nr, fg, Gdiplus::StringAlignmentNear);
        }
    }
    // 別の候補は強さの色の小さな札に、案内はふちどりの字で
    for (auto& t : ttexts_) {
        if (t.tiny) {
            halo_text(t.text, ftiny, float(t.x), float(t.y), c.sub);
            continue;
        }
        const RECT rc = pill_rect(dc, t.text, t.s, t.x, t.y);
        Gdiplus::GraphicsPath path;
        rounded(path, rc);
        const COLORREF fill = pill_fill(t.s);
        Gdiplus::SolidBrush b(col(fill));
        g.FillPath(&b, &path);
        pill_text(t.text, t.s, rc, ink_on(fill));
    }
}

// 提案の頭の文字: 列に入る方法 (入ったら出し方)
std::wstring CandWindow::sugg_label() const {
    if (!label.empty()) return label;
    return sugg_sel_ < 0 ? T(136) : T(137);
}

// 下の行の右の文字
std::wstring CandWindow::foot_text(int n) const {
    if (sugg_sel_ >= 0) return T(138);
    if (n == 0) return L"";
    return sel_ < 0 ? T(139) + std::to_wstring(n) + T(140) : std::to_wstring(sel_ + 1) + L" / " + std::to_wstring(n);
}

int CandWindow::hit_sugg(int x, int y) const {
    if (y < lh_ - footer_h_ || y >= lh_) return -1;
    for (size_t i = 0; i < sugg_x_.size(); i++)
        if (x >= sugg_x_[i].first && x < sugg_x_[i].second) return int(i);
    return -1;
}

int CandWindow::hit(int x, int y) const {
    (void)x;
    int start = page_ * Composer::PAGE;
    int row = (y - pad_ / 2) / std::max(1, row_h_);
    int n = std::min(int(cands_.size()) - start, Composer::PAGE);
    if (y < pad_ / 2 || row < 0 || row >= n) return -1;
    return start + row;
}

void CandWindow::paint(HDC wdc) {
    Colors c = colors(theme);
    RECT rc = {0, 0, width_, height_};
    // ちらつかないように裏で描いてから写す
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, width_, height_);
    HGDIOBJ oldb = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(c.bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HBRUSH border = CreateSolidBrush(c.border);
    FrameRect(dc, &rc, border);
    DeleteObject(border);
    SetBkMode(dc, TRANSPARENT);

    // 候補の一覧は (list_x_, tree_h_) から。一覧の描き方は木が無いときと同じ (幅と高さを一覧のものにして描く)
    const int full_w = width_, full_h = height_;
    width_ = lw_;
    height_ = lh_;
    POINT org;
    SetViewportOrgEx(dc, list_x_, tree_h_, &org);

    int start = page_ * Composer::PAGE;
    int end = std::min(int(cands_.size()), start + Composer::PAGE);
    HGDIOBJ oldf = SelectObject(dc, font_);
    for (int i = start; i < end; i++) {
        int top = pad_ / 2 + (i - start) * row_h_;
        RECT row = {MulDiv(3, int(dpi_), 96), top, width_ - MulDiv(3, int(dpi_), 96), top + row_h_};
        bool selected = i == sel_;
        if (selected) {
            HBRUSH sb = CreateSolidBrush(c.sel_bg);
            HGDIOBJ ob = SelectObject(dc, sb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            int r = MulDiv(8, int(dpi_), 96);
            RoundRect(dc, row.left, row.top + 1, row.right, row.bottom - 1, r, r);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(sb);
        }
        wchar_t num[4] = {wchar_t(L'1' + (i - start)), 0};
        RECT nr = {pad_, top, pad_ + num_w_, top + row_h_};
        SetTextColor(dc, selected ? c.accent : c.sub);
        DrawTextW(dc, num, 1, &nr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        RECT tr = {pad_ + num_w_, top, width_ - pad_, top + row_h_};
        u16 s = cands_[size_t(i)];
        SetTextColor(dc, selected ? c.sel_text : c.text);
        DrawTextW(dc, W(s), int(s.size()), &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        // 提案の種類を右に薄い字で (もしかして・絵文字 …。Windows の候補の窓の見た目に合わせる、2026-10-14)
        if (size_t(i) < notes_.size() && !notes_[size_t(i)].empty()) {
            const u16& n = notes_[size_t(i)];   // 括弧は付けない (薄い字で注釈だと分かる)
            SelectObject(dc, small_);
            SetTextColor(dc, selected ? c.sel_text : c.sub);
            DrawTextW(dc, W(n), int(n.size()), &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            SelectObject(dc, font_);
        }
    }
    SelectObject(dc, small_);
    const int fy = height_ - footer_h_;
    if (!sugg_.empty()) {   // 提案 (青い字の札) を下の行の左に
        const std::wstring label = sugg_label();
        RECT lr = {pad_, fy, width_ - pad_, height_};
        SetTextColor(dc, c.accent);
        DrawTextW(dc, label.c_str(), int(label.size()), &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        HPEN pen = CreatePen(PS_SOLID, 1, c.accent);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int r = MulDiv(10, int(dpi_), 96), m = MulDiv(3, int(dpi_), 96);
        SIZE fz;
        const std::wstring ft = foot_text(int(cands_.size()));
        GetTextExtentPoint32W(dc, ft.c_str(), int(ft.size()), &fz);
        const int limit = width_ - pad_ - fz.cx - MulDiv(8, int(dpi_), 96);
        for (size_t i = 0; i < sugg_.size() && i < sugg_x_.size(); i++) {
            const auto& xs = sugg_x_[i];
            if (xs.second > limit) break;   // 右の文字に重なる札は出さない
            if (int(i) == sugg_sel_) {
                HBRUSH sb = CreateSolidBrush(c.sel_bg);
                HGDIOBJ obb = SelectObject(dc, sb);
                RoundRect(dc, xs.first, fy + m, xs.second, height_ - m, r, r);
                SelectObject(dc, obb);
                DeleteObject(sb);
            } else {
                RoundRect(dc, xs.first, fy + m, xs.second, height_ - m, r, r);
            }
            RECT tr2 = {xs.first, fy, xs.second, height_};
            DrawTextW(dc, W(sugg_[i]), int(sugg_[i].size()), &tr2, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    const std::wstring foot = foot_text(int(cands_.size()));
    RECT fr = {pad_, fy, width_ - pad_, height_ - MulDiv(3, int(dpi_), 96)};
    SetTextColor(dc, c.sub);
    DrawTextW(dc, foot.c_str(), int(foot.size()), &fr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    SelectObject(dc, oldf);
    SetViewportOrgEx(dc, org.x, org.y, nullptr);
    width_ = full_w;
    height_ = full_h;

    BitBlt(wdc, 0, 0, width_, height_, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldb);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------- 打った綴りの帯

namespace {
constexpr wchar_t STRIP_CLASS_NAME[] = L"ShuntiImeKeyStrip";
}

KeyStrip::~KeyStrip() { destroy(); }

void KeyStrip::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (font_) DeleteObject(font_);
    font_ = nullptr;
    dpi_ = 0;
}

void KeyStrip::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    text_.clear();
}

LRESULT CALLBACK KeyStrip::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<KeyStrip*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (self) self->paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void KeyStrip::show(const u16& typed, bool fixed, const std::vector<bool>& mark, const RECT& anchor, HWND owner) {
    if (typed.empty()) { hide(); return; }
    if (!hwnd_) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc = {sizeof wc};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_inst;
            wc.lpszClassName = STRIP_CLASS_NAME;
            reg = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, STRIP_CLASS_NAME, L"", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
        if (!hwnd_) return;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&KeyStrip::proc));
    }
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (!dpi) dpi = 96;
    if (dpi != dpi_ || !font_) {
        if (font_) DeleteObject(font_);
        font_ = CreateFontW(-MulDiv(12, int(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        dpi_ = dpi;
    }
    text_ = typed;
    fixed_ = fixed;
    mark_ = mark;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, W(text_), int(text_.size()), &sz);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    const int px = MulDiv(5, int(dpi_), 96), py = MulDiv(1, int(dpi_), 96);
    w_ = sz.cx + px * 2;
    h_ = tm.tmHeight + py * 2;
    const int x = anchor.left - px, y = anchor.top - h_ - MulDiv(1, int(dpi_), 96);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void KeyStrip::paint(HDC dc) {
    const Colors c = colors(theme);
    RECT r = {0, 0, w_, h_};
    HBRUSH bg = CreateSolidBrush(c.bg);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    HBRUSH br = CreateSolidBrush(c.border);
    FrameRect(dc, &r, br);
    DeleteObject(br);
    HGDIOBJ old = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    // 直した鍵だけ赤 (直した所が分からなければ、直したときは全部を直しの色に)。ほかは薄い字
    const bool any = std::find(mark_.begin(), mark_.end(), true) != mark_.end();
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    int x = MulDiv(5, int(dpi_), 96);
    const int y = (h_ - tm.tmHeight) / 2;
    for (size_t i = 0; i < text_.size();) {
        const bool m = i < mark_.size() && mark_[i];
        size_t j = i + 1;
        while (j < text_.size() && (j < mark_.size() && mark_[j]) == m) j++;
        SetTextColor(dc, m ? RGB(220, 40, 40) : (fixed_ && !any) ? RGB(230, 90, 30) : c.sub);
        TextOutW(dc, x, y, W(text_) + i, int(j - i));
        SIZE sz = {};
        GetTextExtentPoint32W(dc, W(text_) + i, int(j - i), &sz);
        x += sz.cx;
        i = j;
    }
    SelectObject(dc, old);
}

}  // namespace win
}  // namespace shunti

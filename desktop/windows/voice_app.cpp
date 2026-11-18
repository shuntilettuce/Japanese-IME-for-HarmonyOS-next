// shunti IME の声の入力 (shunti_voice.exe、2026-11-16 試作)。タスクバーの Zori-chan を右クリック →「声の入力」で開く。
// 録音も認識もこの PC の中だけ (voice_core.h: 音響モデル E1 → 語の網 → 変換モデル)。声はどこにも送らず、ファイルにも残さない。
// 窓は手書き入力と同じく WS_EX_NOACTIVATE で、押しても入力欄からフォーカスが外れない。候補を押すと SendInput (KEYEVENTF_UNICODE)
// で今の入力欄に送る。部品 (辞書・変換モデル・voice_trie.bin・voice_e1.bin) は窓を開いたときに読み、閉じると返す (IME 本体は重くならない)
// デバッグ版 (build.bat debug、SHUNTI_INPUT_LOG) だけ: 直前の 1 回 (声と結果) を次に話すまで持っておき、「今回の記録を残す」で
// 作者が正しい文とフィードバックを書いて %APPDATA%/shunti IME/voice_log/ に残す (声の wav + voice_log.jsonl の 1 行)。配布版には入らない
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "strings.h"
#include "voice_core.h"

using namespace shunti;

namespace {

constexpr UINT WM_LOADED = WM_APP + 1;   // 読み込みの結果 (wParam = 1 なら成功)
constexpr UINT WM_RESULT = WM_APP + 2;   // 認識の結果 (lParam = new voice::Result*)
constexpr UINT_PTR TIMER_LIMIT = 1;      // 録音の上限 (話し続けても 30 秒で止める)
constexpr UINT LIMIT_MS = 30000;
constexpr int N_CAND = 6;                // 並べる候補の数
constexpr int RATE = 16000, NBUF = 8, BUF_SAMPLES = RATE / 10;   // 0.1 秒ずつの録音の器

#ifdef SHUNTI_INPUT_LOG
constexpr bool VOICE_LOG = true;
#else
constexpr bool VOICE_LOG = false;
#endif

enum class State { Loading, Ready, Recording, Busy, Error };

// 認識の結果 (スレッドから窓へ)。声も一緒に返して、記録を残すときに使う
struct Done { voice::Result r; std::vector<float> pcm; };

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_big = nullptr, g_small = nullptr;
UINT g_dpi = 96;
State g_state = State::Loading;
std::wstring g_status;
std::vector<std::wstring> g_cands;

std::string g_lex, g_model;   // エンジンが写さずに読むので、窓を閉じるまで持つ
voice::Recognizer g_rec;

HWAVEIN g_wi = nullptr;
WAVEHDR g_hdr[NBUF];
std::vector<int16_t> g_bufs[NBUF];
int g_queued = 0;
bool g_capturing = false;
std::vector<float> g_pcm;

// 直前の 1 回 (デバッグ版の記録用)。次に話すと置き換わる
std::unique_ptr<Done> g_last;
int g_last_chosen = -1;      // 押した候補の番号 (押していなければ -1)
std::wstring g_last_time;    // 話し終えた時刻 (記録のファイル名)
HWND g_form = nullptr, g_ans = nullptr, g_fb = nullptr;
int g_saved = 0;

int px(int v) { return MulDiv(v, int(g_dpi), 96); }

std::filesystem::path exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

bool slurp(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !out.empty();
}

struct Layout { RECT button, status, cands[N_CAND], keep; };
Layout layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    Layout l{};
    const int m = px(8), bh = px(48), sh = px(22), ch = px(34);
    l.button = {m, m, rc.right - m, m + bh};
    l.status = {m, l.button.bottom + px(4), rc.right - m, l.button.bottom + px(4) + sh};
    int y = l.status.bottom + px(4);
    for (int i = 0; i < N_CAND; i++, y += ch + px(3)) l.cands[i] = {m, y, rc.right - m, y + ch};
    l.keep = {rc.right - m - px(180), y + px(2), rc.right - m, y + px(30)};
    return l;
}

bool in(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void set_status(State s, const std::wstring& msg) {
    g_state = s;
    g_status = msg;
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void load_async() {
    HWND wnd = g_wnd;
    std::thread([wnd] {
        const auto d = exe_dir();
        std::string trie, e1;
        bool ok = slurp(d / L"kkc_lex.bin", g_lex) && slurp(d / L"kkc_model.bin", g_model) && slurp(d / L"voice_trie.bin", trie) &&
                  slurp(d / L"voice_e1.bin", e1);
        if (ok)
            ok = g_rec.open((const uint8_t*)g_lex.data(), g_lex.size(), (const uint8_t*)g_model.data(), g_model.size(),
                            (const uint8_t*)trie.data(), trie.size(), (const uint8_t*)e1.data(), e1.size());
        PostMessageW(wnd, WM_LOADED, ok ? 1 : 0, 0);
    }).detach();
}

// ---- 録音 (waveIn、16 kHz・モノラル・16 ビット。器が埋まるたびに窓へ MM_WIM_DATA が来る)
bool start_capture() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = RATE * 2;
    if (waveInOpen(&g_wi, WAVE_MAPPER, &wf, DWORD_PTR(g_wnd), 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR) { g_wi = nullptr; return false; }
    g_pcm.clear();
    g_queued = 0;
    for (int i = 0; i < NBUF; i++) {
        g_bufs[i].assign(BUF_SAMPLES, 0);
        g_hdr[i] = WAVEHDR{};
        g_hdr[i].lpData = reinterpret_cast<LPSTR>(g_bufs[i].data());
        g_hdr[i].dwBufferLength = DWORD(BUF_SAMPLES * 2);
        waveInPrepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
        if (waveInAddBuffer(g_wi, &g_hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) g_queued++;
    }
    g_capturing = true;
    if (waveInStart(g_wi) != MMSYSERR_NOERROR) {
        g_capturing = false;
        waveInReset(g_wi);
        for (int i = 0; i < NBUF; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
        waveInClose(g_wi);
        g_wi = nullptr;
        return false;
    }
    return true;
}

void stop_capture() {
    if (!g_wi || !g_capturing) return;
    g_capturing = false;
    waveInReset(g_wi);   // 残りの器が MM_WIM_DATA で返ってくる。全部返ったら閉じて認識する (on_data)
}

void recognize_async() {
    std::vector<float> pcm = std::move(g_pcm);
    g_pcm.clear();
    if (pcm.size() < size_t(RATE / 4)) { set_status(State::Ready, T(242)); return; }
    set_status(State::Busy, T(239));
    HWND wnd = g_wnd;
    std::thread([wnd, pcm = std::move(pcm)]() mutable {
        auto* d = new Done{g_rec.recognize(pcm.data(), int(pcm.size())), {}};
        if (VOICE_LOG) d->pcm = std::move(pcm);
        PostMessageW(wnd, WM_RESULT, 0, LPARAM(d));
    }).detach();
}

void on_data(WAVEHDR* h) {
    const int16_t* s = reinterpret_cast<const int16_t*>(h->lpData);
    for (DWORD i = 0; i < h->dwBytesRecorded / 2; i++) g_pcm.push_back(s[i] / 32768.0f);
    g_queued--;
    if (g_capturing) {
        if (waveInAddBuffer(g_wi, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) g_queued++;
        return;
    }
    if (g_queued > 0) return;
    for (int i = 0; i < NBUF; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
    waveInClose(g_wi);
    g_wi = nullptr;
    recognize_async();
}

// 今フォーカスのある入力欄へ字を送る (この窓はフォーカスを取らないので、送り先は話す前の入力欄のまま)
void send_text(const std::wstring& t) {
    std::vector<INPUT> v;
    for (wchar_t c : t) {
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wScan = c;
        d.ki.dwFlags = KEYEVENTF_UNICODE;
        v.push_back(d);
        d.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        v.push_back(d);
    }
    if (!v.empty()) SendInput(UINT(v.size()), v.data(), sizeof(INPUT));
}

void toggle() {
    if (g_state == State::Ready) {
        g_cands.clear();
        if (g_form) DestroyWindow(g_form);   // 書きかけの記録は捨てる (次の声で置き換わる)
        g_last.reset();
        if (!start_capture()) { set_status(State::Ready, T(241)); return; }
        SetTimer(g_wnd, TIMER_LIMIT, LIMIT_MS, nullptr);
        set_status(State::Recording, L"");
    } else if (g_state == State::Recording) {
        KillTimer(g_wnd, TIMER_LIMIT);
        set_status(State::Busy, T(239));
        stop_capture();
    }
}

void draw_box(HDC dc, const RECT& r, const wchar_t* text, HFONT f, COLORREF bg, COLORREF fg, UINT align) {
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    RECT t = r;
    t.left += px(10);
    t.right -= px(10);
    DrawTextW(dc, text, -1, &t, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void paint(HDC dc0) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const Layout l = layout();
    const bool rec = g_state == State::Recording, can = g_state == State::Ready || rec;
    draw_box(dc, l.button, rec ? T(236) : T(235), g_big, rec ? RGB(253, 232, 228) : can ? RGB(232, 240, 254) : RGB(242, 242, 242),
             can ? RGB(20, 20, 20) : RGB(160, 160, 160), DT_CENTER);
    SelectObject(dc, g_small);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(110, 110, 110));
    RECT s = l.status;
    DrawTextW(dc, g_status.c_str(), -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    for (int i = 0; i < N_CAND && i < int(g_cands.size()); i++)
        draw_box(dc, l.cands[i], g_cands[size_t(i)].c_str(), i == 0 ? g_big : g_font, i == 0 ? RGB(232, 240, 254) : RGB(248, 248, 248),
                 RGB(20, 20, 20), DT_LEFT);
    if (VOICE_LOG && g_last && g_state == State::Ready && !g_form)
        draw_box(dc, l.keep, T(244), g_small, RGB(255, 247, 230), RGB(90, 60, 10), DT_CENTER);
    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void make_fonts() {
    for (HFONT* f : {&g_font, &g_big, &g_small}) if (*f) DeleteObject(*f);
    g_font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_big = CreateFontW(-px(18), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_small = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
}

// ---- デバッグ版の記録 (今回の声 + 結果 + 正しい文 + フィードバック)
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string o(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), o.data(), n, nullptr, nullptr);
    return o;
}
std::string utf8(const std::u16string& s) { return utf8(std::wstring(s.begin(), s.end())); }
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
std::wstring edit_text(HWND e) {
    std::wstring t(size_t(GetWindowTextLengthW(e)) + 1, L'\0');
    t.resize(size_t(GetWindowTextW(e, t.data(), int(t.size()))));
    return t;
}
std::filesystem::path log_dir() {
    wchar_t* p = nullptr;
    std::filesystem::path d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = std::filesystem::path(p) / L"shunti IME" / L"voice_log";
    CoTaskMemFree(p);
    return d;
}
bool write_wav(const std::filesystem::path& p, const std::vector<float>& pcm) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    const uint32_t data = uint32_t(pcm.size() * 2), riff = 36 + data, fmt = 16, rate = RATE, bps = RATE * 2;
    const uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
    f.write("RIFF", 4); f.write((const char*)&riff, 4); f.write("WAVEfmt ", 8); f.write((const char*)&fmt, 4);
    f.write((const char*)&pcmfmt, 2); f.write((const char*)&ch, 2); f.write((const char*)&rate, 4); f.write((const char*)&bps, 4);
    f.write((const char*)&align, 2); f.write((const char*)&bits, 2); f.write("data", 4); f.write((const char*)&data, 4);
    for (float v : pcm) { const int16_t s = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); f.write((const char*)&s, 2); }
    return bool(f);
}
// 1 件 = wav 1 つ + voice_log.jsonl の 1 行。中身: 時刻・wav の名前・長さ・土台の読み・かなの 1 位・語の網の上位の読み・変換の候補・
// 押した候補・正しい文・フィードバック・時間・部品の大きさ (どの版の音響モデルと変換モデルで出した結果か)
bool save_record(const std::wstring& answer, const std::wstring& feedback) {
    if (!g_last) return false;
    const auto d = log_dir();
    if (d.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    const std::wstring wav = g_last_time + L".wav";
    if (!write_wav(d / wav, g_last->pcm)) return false;
    const voice::Result& r = g_last->r;
    std::string j = "{\"t\":" + jstr(utf8(g_last_time)) + ",\"wav\":" + jstr(utf8(wav));
    char num[96];
    snprintf(num, sizeof(num), ",\"sec\":%.2f", double(g_last->pcm.size()) / RATE);
    j += num;
    j += ",\"base\":" + jstr(utf8(r.base)) + ",\"greedy\":" + jstr(utf8(r.greedy)) + ",\"alts\":[";
    for (size_t i = 0; i < r.alts.size(); i++) {
        snprintf(num, sizeof(num), ",\"ac\":%.3f,\"score\":%.3f,\"cost\":%d}", r.alts[i].ac, r.alts[i].score, int(r.alts[i].cost));
        j += (i ? ",{\"read\":" : "{\"read\":") + jstr(utf8(r.alts[i].read)) + num;
    }
    j += "],\"cands\":[";
    for (size_t i = 0; i < r.cands.size(); i++) j += (i ? "," : "") + jstr(utf8(r.cands[i]));
    snprintf(num, sizeof(num), "],\"chosen\":%d", g_last_chosen);
    j += num;
    j += ",\"answer\":" + jstr(utf8(answer)) + ",\"feedback\":" + jstr(utf8(feedback));
    snprintf(num, sizeof(num), ",\"ac_ms\":%.0f,\"conv_ms\":%.0f,\"lex\":%zu,\"model\":%zu", r.ac_ms, r.conv_ms, g_lex.size(), g_model.size());
    j += num;
    std::error_code e2;
    const auto e1sz = std::filesystem::file_size(exe_dir() / L"voice_e1.bin", e2), trsz = std::filesystem::file_size(exe_dir() / L"voice_trie.bin", e2);
    snprintf(num, sizeof(num), ",\"e1\":%llu,\"trie\":%llu}\n", (unsigned long long)e1sz, (unsigned long long)trsz);
    j += num;
    std::ofstream f(d / L"voice_log.jsonl", std::ios::binary | std::ios::app);
    if (!f) return false;
    f << j;
    return bool(f);
}

enum { ID_ANS = 10, ID_FB = 11, ID_SAVE = 12, ID_CANCEL = 13 };

LRESULT CALLBACK form_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == ID_SAVE) {
                const bool ok = save_record(edit_text(g_ans), edit_text(g_fb));
                if (ok) {
                    g_saved++;
                    g_last.reset();
                    wchar_t t[200];
                    swprintf_s(t, T(250), (g_last_time + L".wav").c_str());
                    g_status = t;
                } else {
                    g_status = T(251);
                }
                DestroyWindow(h);
                return 0;
            }
            // Esc (IDCANCEL) はやめる。Enter (IDOK) は何もしない (変換の確定の Enter で誤って保存しないよう、保存はボタンだけ)
            if (LOWORD(wp) == ID_CANCEL || LOWORD(wp) == IDCANCEL) { DestroyWindow(h); return 0; }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_form = g_ans = g_fb = nullptr;
            InvalidateRect(g_wnd, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 記録の窓。こちらは字を打つので普通の窓 (フォーカスを取る)。正しい文の欄には押した候補 (無ければ 1 位) を先に入れておく
void open_form() {
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = form_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.lpszClassName = L"shunti_voice_form";
        RegisterClassW(&wc);
        reg = true;
    }
    RECT me;
    GetWindowRect(g_wnd, &me);
    const int W = px(460), H = px(300);
    RECT r{0, 0, W, H};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST, g_dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g_form = CreateWindowExW(WS_EX_TOPMOST, L"shunti_voice_form", T(245), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             me.left, std::max(0, int(me.top) - wh - px(8)), ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_form) return;
    const int m = px(10), lw = W - m * 2;
    auto label = [&](const wchar_t* t, int y) {
        HWND s = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, m, y, lw, px(20), g_form, nullptr, inst, nullptr);
        SendMessageW(s, WM_SETFONT, WPARAM(g_small), TRUE);
    };
    label(T(246), m);
    g_ans = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, m, m + px(22), lw, px(30),
                            g_form, HMENU(INT_PTR(ID_ANS)), inst, nullptr);
    label(T(247), m + px(62));
    g_fb = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                           m, m + px(84), lw, px(130), g_form, HMENU(INT_PTR(ID_FB)), inst, nullptr);
    const int by = H - m - px(32);
    HWND ok = CreateWindowExW(0, L"BUTTON", T(248), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, W - m - px(200), by, px(96), px(32),
                              g_form, HMENU(INT_PTR(ID_SAVE)), inst, nullptr);
    HWND no = CreateWindowExW(0, L"BUTTON", T(249), WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - m - px(96), by, px(96), px(32),
                              g_form, HMENU(INT_PTR(ID_CANCEL)), inst, nullptr);
    for (HWND c : {g_ans, g_fb, ok, no}) SendMessageW(c, WM_SETFONT, WPARAM(g_font), TRUE);
    const voice::Result& rr = g_last->r;
    const int pick = g_last_chosen >= 0 ? g_last_chosen : 0;
    if (pick < int(rr.cands.size())) SetWindowTextW(g_ans, std::wstring(rr.cands[size_t(pick)].begin(), rr.cands[size_t(pick)].end()).c_str());
    ShowWindow(g_form, SW_SHOW);
    SetForegroundWindow(g_form);
    SetFocus(g_ans);
    SendMessageW(g_ans, EM_SETSEL, 0, -1);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // 押してもフォーカスを取らない (送り先の入力欄のまま)
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const Layout l = layout();
            if (in(l.button, x, y)) { toggle(); return 0; }
            if (VOICE_LOG && g_last && g_state == State::Ready && !g_form && in(l.keep, x, y)) { open_form(); return 0; }
            for (int i = 0; i < N_CAND && i < int(g_cands.size()); i++)
                if (in(l.cands[i], x, y)) {
                    send_text(g_cands[size_t(i)]);
                    g_last_chosen = i;
                    g_cands.clear();
                    set_status(State::Ready, T(238));
                    return 0;
                }
            return 0;
        }
        case MM_WIM_DATA: on_data(reinterpret_cast<WAVEHDR*>(lp)); return 0;
        case WM_TIMER:
            if (wp == TIMER_LIMIT) {
                KillTimer(h, TIMER_LIMIT);
                if (g_state == State::Recording) toggle();
            }
            return 0;
        case WM_LOADED:
            if (wp) set_status(State::Ready, T(238));
            else set_status(State::Error, T(240));
            return 0;
        case WM_RESULT: {
            std::unique_ptr<Done> d(reinterpret_cast<Done*>(lp));
            const voice::Result* r = &d->r;
            g_cands.clear();
            for (auto& c : r->cands) if (!c.empty()) g_cands.emplace_back(c.begin(), c.end());
            wchar_t t[160];
            if (g_cands.empty()) wcscpy_s(t, T(242));
            else swprintf_s(t, T(243), (r->ac_ms + r->conv_ms) / 1000.0, std::wstring(r->base.begin(), r->base.end()).c_str());
            set_status(State::Ready, t);
            if (VOICE_LOG) {
                wchar_t ts[32];
                const std::time_t now = std::time(nullptr);
                std::tm tm{};
                localtime_s(&tm, &now);
                wcsftime(ts, 32, L"%Y%m%d_%H%M%S", &tm);
                g_last_time = ts;
                g_last_chosen = -1;
                g_last = std::move(d);
            }
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
        case WM_DESTROY:
            if (g_wi) { g_capturing = false; waveInReset(g_wi); for (int i = 0; i < NBUF; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR)); waveInClose(g_wi); g_wi = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 2 つ目は開かない: 開いている窓を前に出すだけ
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"shunti_voice_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"shunti_voice", nullptr)) ShowWindow(w, SW_SHOWNOACTIVATE);
        return 0;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.lpszClassName = L"shunti_voice";
    RegisterClassW(&wc);
    g_dpi = GetDpiForSystem();
    make_fonts();
    // 画面の右下 (作業領域の中) に出す
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT r{0, 0, px(460), px(8 + 48 + 4 + 22 + 4 + N_CAND * 37 + 8 + (VOICE_LOG ? 34 : 0))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_dpi);
    const int w = r.right - r.left, hgt = r.bottom - r.top;
    g_wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"shunti_voice", T(233),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, wa.right - w - px(16), wa.bottom - hgt - px(16), w, hgt,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    g_status = T(237);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    load_async();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g_form && IsDialogMessageW(g_form, &m)) continue;   // 記録の窓の Tab・Enter
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    // 認識のスレッドが走っている途中で閉じても、プロセスごと終わるので待たない
    ExitProcess(0);
}

#include "text_service.h"

#include <inputscope.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "display_attr.h"
#include "globals.h"
#include "input_log.h"
#include "langbar.h"
#include "shortcut_key.h"
#include "strings.h"

// 試験用: 設定のフォルダに layout_trace.on があるときだけ、配列が受け取ったキーと時刻を layout_trace.txt に書く
static void layout_trace(const char* op, const std::string& k, uint64_t t) {
    static int on = -1;
    if (on < 0) on = std::filesystem::exists(shunti::win::user_dir() / L"layout_trace.on") ? 1 : 0;
    if (!on) return;
    FILE* f = _wfopen((shunti::win::user_dir() / L"layout_trace.txt").c_str(), L"ab");
    if (!f) return;
    fprintf(f, "%llu %s %s\n", (unsigned long long)t, op, k.c_str());
    fclose(f);
}

// 配列の同時押しの判定に使う時刻 (ms)。GetTickCount64 は約 16ms 刻みで重なりを測るには粗いので、性能カウンタで
static uint64_t layout_now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart * 1000 / freq.QuadPart);
}


using Microsoft::WRL::ComPtr;

namespace shunti {
namespace win {

namespace {
constexpr LONG CTX_CHARS = 40;   // 変換の文脈に読むカーソルの左の字数 (モデルの文脈の長さ)

const wchar_t* W(const u16& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

// 関数を 1 つ持つだけの編集の枠 (TSF では入力欄の文字を触るのはこの中だけ)
class EditSession : public ITfEditSession {
public:
    explicit EditSession(std::function<void(TfEditCookie)> f) : f_(std::move(f)) { dll_add_ref(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) *ppv = static_cast<ITfEditSession*>(this);
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return ULONG(n);
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        f_(ec);
        return S_OK;
    }

private:
    ~EditSession() { dll_release(); }
    LONG refs_ = 1;
    std::function<void(TfEditCookie)> f_;
};

// 日本語入力のオン・オフを切り替えるキー (半角/全角・漢字・Alt+`)
struct Preserved { const GUID guid; UINT vk; UINT mods; };
// {5A9F1B06-7C3E-4E0B-9E8D-2B6A1F4C7D11} など。この IME の中だけで使う印
const Preserved PRESERVED[] = {
    {{0x5a9f1b06, 0x7c3e, 0x4e0b, {0x9e, 0x8d, 0x2b, 0x6a, 0x1f, 0x4c, 0x7d, 0x11}}, VK_KANJI, TF_MOD_IGNORE_ALL_MODIFIER},
    {{0x5a9f1b07, 0x7c3e, 0x4e0b, {0x9e, 0x8d, 0x2b, 0x6a, 0x1f, 0x4c, 0x7d, 0x11}}, VK_OEM_AUTO, TF_MOD_IGNORE_ALL_MODIFIER},
    {{0x5a9f1b08, 0x7c3e, 0x4e0b, {0x9e, 0x8d, 0x2b, 0x6a, 0x1f, 0x4c, 0x7d, 0x11}}, VK_OEM_ENLW, TF_MOD_IGNORE_ALL_MODIFIER},
    {{0x5a9f1b09, 0x7c3e, 0x4e0b, {0x9e, 0x8d, 0x2b, 0x6a, 0x1f, 0x4c, 0x7d, 0x11}}, VK_OEM_3, TF_MOD_ALT},
};

// ショートカットキーの「入力中だけ効く」機能 -> 押したときと同じ KeyEvent (composer に渡す)。知らない id は false
bool shortcut_compose_event(const std::string& id, KeyEvent& ev) {
    ev = KeyEvent();
    struct { const char* id; Key key; bool shift; bool ctrl; } funcs[] = {
        {"confirm", Key::Enter, false, false},
        {"cancel", Key::Escape, false, false},
        {"convert", Key::Space, false, false},
        {"prev_cand", Key::Up, false, false},
        {"hiragana", Key::F6, false, false},
        {"katakana", Key::F7, false, false},
        {"halfkana", Key::F8, false, false},
        {"zen_alpha", Key::F9, false, false},
        {"han_alpha", Key::F10, false, false},
        {"seg_shrink", Key::Left, true, false},
        {"seg_grow", Key::Right, true, false},
        {"seg_prev", Key::Left, false, false},
        {"seg_next", Key::Right, false, false},
        {"cand_delete", Key::Delete, false, true},
        {"backspace", Key::Backspace, false, false},
        {"delete_char", Key::Delete, false, false},
        {"seg_first", Key::Home, false, false},
        {"seg_last", Key::End, false, false},
        {"page_next", Key::PageDown, false, false},
        {"page_prev", Key::PageUp, false, false},
        {"sugg_next", Key::Tab, false, false},
        {"sugg_prev", Key::Tab, true, false},
        {"sugg_commit", Key::Enter, true, false},
        {"kana_cycle", Key::Muhenkan, false, false},
        {"next_cand_down", Key::Down, false, false},
    };
    for (auto& f : funcs) {
        if (id == f.id) { ev.key = f.key; ev.shift = f.shift; ev.ctrl = f.ctrl; return true; }
    }
    return false;
}
}  // namespace

TextService::TextService() { dll_add_ref(); }
TextService::~TextService() { dll_release(); }

// ---------------------------------------------------------------- IUnknown

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor) || IsEqualIID(riid, IID_ITfTextInputProcessorEx))
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
        *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
        *ppv = static_cast<ITfKeyEventSink*>(this);
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
        *ppv = static_cast<ITfCompositionSink*>(this);
    else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
        *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    else if (IsEqualIID(riid, IID_ITfCompartmentEventSink))
        *ppv = static_cast<ITfCompartmentEventSink*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) TextService::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }
STDMETHODIMP_(ULONG) TextService::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return ULONG(n);
}

// ---------------------------------------------------------------- 始まりと終わり

STDMETHODIMP TextService::Activate(ITfThreadMgr* tm, TfClientId cid) { return ActivateEx(tm, cid, 0); }

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* tm, TfClientId cid, DWORD) {
    tm_ = tm;
    tm_->AddRef();
    cid_ = cid;

    // 辞書・モデル・学習の読み込み (Core) と Composer は、最初にキーを打ったときに作る (ensure_composer)。
    // IME はアプリが起動して入力の仕組みに加わるたびに読み込まれるので、ここで作ると入力しないアプリまで起動が遅くなった (ブログの報告 2026-10-18)
    cand_.on_click = [this](int i) { on_candidate_clicked(i); };
    cand_.on_sugg_click = [this](int i) { on_suggestion_clicked(i); };

    ComPtr<ITfSource> src;
    if (SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&src))))
        src->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &tm_sink_cookie_);
    init_key_sink();
    init_preserved_keys();

    ComPtr<ITfCategoryMgr> cat;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cat)))) {
        cat->RegisterGUID(GUID_AttrInput, &atom_input_);
        cat->RegisterGUID(GUID_AttrConverted, &atom_converted_);
        cat->RegisterGUID(GUID_AttrFocused, &atom_focused_);
        cat->RegisterGUID(GUID_AttrCorrected, &atom_corrected_);
    }

    advise_compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, open_cookie_);
    advise_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, conv_cookie_);

    langbar_ = new LangBarButton(this);
    ComPtr<ITfLangBarItemMgr> lbm;
    if (SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&lbm)))) lbm->AddItem(langbar_);

    // このソフトで前に使ったときの状態 (終了時・切り替え時に覚える)。初めてのソフトは設定 (初めて使うソフト: オン/オフ)
    Settings st;   // Core はまだ作らない (起動を軽く) ので、設定は直接読む
    load_settings(user_dir() / L"settings.json", st);
    set_open(app_open_state(st.app_default_open));
    return S_OK;
}

// アプリ (実行ファイルの名前) ごとの日本語入力のオン・オフ。shunti IME の設定のフォルダの app_open.tsv に覚える
// 実行ファイルの名前の大文字小文字をそろえる (app_open.tsv の古い行は Claude.exe のように大文字まじり)
static std::string lower_ascii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

std::wstring TextService::app_name() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t k = s.find_last_of(L"\\/");
    std::wstring name = k == std::wstring::npos ? s : s.substr(k + 1);
    for (auto& c : name) c = wchar_t(towlower(c));   // Claude.exe と claude.exe を同じソフトとして覚える
    return name;
}

// app_open.tsv の 1 行: 名前 	 オン(1)/オフ(0) [	 窓の名前 [	 印]]。印 "noshortcut" = このソフトではショートカットを使わない
// (ゲームの Ctrl+Space など。ユーザー 2026-10-22: Minecraft で Ctrl+Space が日本語入力の切り替えとぶつかる)
struct AppRow { std::string name, title, flags; bool open = false; bool found = false; bool has_flags = false; };   // has_flags = 印の欄がある (0.7.1 から)
static std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('\t', a);
        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}

static AppRow read_app_row(const std::string& t, const std::string& me) {
    AppRow r;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size();
        std::string line = t.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto f = split_tab(line);
        if (f.size() < 2 || lower_ascii(f[0]) != me) continue;
        r.found = true;
        r.name = me;
        r.open = f[1].substr(0, 1) == "1";
        r.title = f.size() > 2 ? f[2] : "";
        r.flags = f.size() > 3 ? f[3] : "";
        r.has_flags = f.size() > 3;
    }
    return r;
}

// このソフトの今の窓の名前 (一覧で javaw.exe が Minecraft だと分かるように)。前に出ている窓がこのソフトのものなら
static std::string own_window_title() {
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (!h || !GetWindowThreadProcessId(h, &pid) || pid != GetCurrentProcessId()) return "";
    wchar_t buf[200] = L"";
    GetWindowTextW(h, buf, 200);
    std::string t = to_utf8(u16(reinterpret_cast<const char16_t*>(buf)));
    for (auto& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return t;
}

// Java で動くソフト (Minecraft など) は、初めて使うときからショートカットを使わない (ゲームのキーとぶつかるので)
static bool default_no_shortcut(const std::wstring& name) { return name == L"javaw.exe" || name == L"java.exe"; }

bool TextService::app_open_state(bool def) {
    std::string t;
    read_file(user_dir() / L"app_open.tsv", t);
    const std::string me = to_utf8(u16(reinterpret_cast<const char16_t*>(app_name().c_str())));
    const AppRow r = read_app_row(t, me);
    // 印の欄の無い古い行 (0.7.0 まで) のソフトも、Java のソフトなら使わないにする (前から使っていた Minecraft も直るように)
    no_shortcuts_ = r.has_flags ? r.flags.find("noshortcut") != std::string::npos : default_no_shortcut(app_name());
    if (no_shortcuts_) return false;   // 常にオフ: いつもオフで始める
    return r.found ? r.open : def;
}

void TextService::remember_app_open(bool open) {
    std::string t, out;
    read_file(user_dir() / L"app_open.tsv", t);
    const std::string me = to_utf8(u16(reinterpret_cast<const char16_t*>(app_name().c_str())));
    const AppRow old = read_app_row(t, me);
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size();
        std::string line = t.substr(i, j - i);
        i = j + 1;
        if (line.empty() || lower_ascii(line.substr(0, line.find('\t'))) == me) continue;   // 大文字小文字の違う古い行も置き換える
        out += line + "\n";
    }
    std::string title = own_window_title();
    if (title.empty()) title = old.title;
    std::string flags = old.has_flags ? old.flags : (no_shortcuts_ ? "noshortcut" : "");
    out += me + "\t" + (open ? "1" : "0") + "\t" + title + "\t" + flags + "\n";
    write_file_atomic(user_dir() / L"app_open.tsv", out);
}

STDMETHODIMP TextService::Deactivate() {
    if (tm_) remember_app_open(is_open());   // ソフトの終了・IME の切り替え: このソフトでの状態を覚える (次に起動したとき戻す)
    finish_composition();
    cand_.destroy();
    strip_.destroy();
    if (langbar_) {
        ComPtr<ITfLangBarItemMgr> lbm;
        if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&lbm)))) lbm->RemoveItem(langbar_);
        langbar_->detach();
        langbar_->Release();
        langbar_ = nullptr;
    }
    unadvise_compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, open_cookie_);
    unadvise_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, conv_cookie_);
    uninit_preserved_keys();
    uninit_key_sink();
    if (tm_ && tm_sink_cookie_ != TF_INVALID_COOKIE) {
        ComPtr<ITfSource> src;
        if (SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&src)))) src->UnadviseSink(tm_sink_cookie_);
        tm_sink_cookie_ = TF_INVALID_COOKIE;
    }
    if (comp_) { comp_->Release(); comp_ = nullptr; }
    if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; }
    composer_.reset();
    if (tm_) { tm_->Release(); tm_ = nullptr; }
    cid_ = TF_CLIENTID_NULL;
    return S_OK;
}

bool TextService::init_key_sink() {
    ComPtr<ITfKeystrokeMgr> km;
    if (FAILED(tm_->QueryInterface(IID_PPV_ARGS(&km)))) return false;
    return SUCCEEDED(km->AdviseKeyEventSink(cid_, static_cast<ITfKeyEventSink*>(this), TRUE));
}

void TextService::uninit_key_sink() {
    ComPtr<ITfKeystrokeMgr> km;
    if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&km)))) km->UnadviseKeyEventSink(cid_);
}

bool TextService::init_preserved_keys() {
    ComPtr<ITfKeystrokeMgr> km;
    if (FAILED(tm_->QueryInterface(IID_PPV_ARGS(&km)))) return false;
    for (auto& p : PRESERVED) {
        TF_PRESERVEDKEY k = {p.vk, p.mods};
        km->PreserveKey(cid_, p.guid, &k, L"", 0);
    }
    return true;
}

void TextService::uninit_preserved_keys() {
    ComPtr<ITfKeystrokeMgr> km;
    if (!tm_ || FAILED(tm_->QueryInterface(IID_PPV_ARGS(&km)))) return;
    for (auto& p : PRESERVED) {
        TF_PRESERVEDKEY k = {p.vk, p.mods};
        km->UnpreserveKey(p.guid, &k);
    }
}

// ---------------------------------------------------------------- オン・オフ (コンパートメント)

bool TextService::advise_compartment(const GUID& g, DWORD& cookie) {
    ComPtr<ITfCompartmentMgr> cm;
    ComPtr<ITfCompartment> c;
    ComPtr<ITfSource> src;
    if (FAILED(tm_->QueryInterface(IID_PPV_ARGS(&cm))) || FAILED(cm->GetCompartment(g, &c)) || FAILED(c.As(&src))) return false;
    return SUCCEEDED(src->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(this), &cookie));
}

void TextService::unadvise_compartment(const GUID& g, DWORD& cookie) {
    if (!tm_ || cookie == TF_INVALID_COOKIE) return;
    ComPtr<ITfCompartmentMgr> cm;
    ComPtr<ITfCompartment> c;
    ComPtr<ITfSource> src;
    if (SUCCEEDED(tm_->QueryInterface(IID_PPV_ARGS(&cm))) && SUCCEEDED(cm->GetCompartment(g, &c)) && SUCCEEDED(c.As(&src)))
        src->UnadviseSink(cookie);
    cookie = TF_INVALID_COOKIE;
}

void TextService::set_compartment(const GUID& g, DWORD v) {
    ComPtr<ITfCompartmentMgr> cm;
    ComPtr<ITfCompartment> c;
    if (!tm_ || FAILED(tm_->QueryInterface(IID_PPV_ARGS(&cm))) || FAILED(cm->GetCompartment(g, &c))) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = LONG(v);
    c->SetValue(cid_, &var);
}

DWORD TextService::get_compartment(const GUID& g, bool* ok) {
    if (ok) *ok = false;
    ComPtr<ITfCompartmentMgr> cm;
    ComPtr<ITfCompartment> c;
    if (!tm_ || FAILED(tm_->QueryInterface(IID_PPV_ARGS(&cm))) || FAILED(cm->GetCompartment(g, &c))) return 0;
    VARIANT var;
    VariantInit(&var);
    if (c->GetValue(&var) != S_OK || var.vt != VT_I4) return 0;
    if (ok) *ok = true;
    return DWORD(var.lVal);
}

bool TextService::is_open() { return get_compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) != 0; }

void TextService::set_open(bool open) {
    if (!open) finish_composition();
    set_compartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, open ? 1 : 0);
    update_mode_ui();
}

void TextService::update_mode_ui() {
    if (is_open()) {
        // タスクバーの表示は変換モードも見る: ひらがな (全角・ローマ字)
        const DWORD want = TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_FULLSHAPE | TF_CONVERSIONMODE_ROMAN;
        bool ok = false;
        if (get_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &ok) != want || !ok)
            set_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, want);
    }
    if (langbar_) langbar_->update();
}

STDMETHODIMP TextService::OnChange(REFGUID guid) {
    if (IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
        if (!is_open()) finish_composition();
        update_mode_ui();
    } else if (IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
        // タスクバーの入力モードのメニューで「半角英数」などを選んだ: 日本語入力を切る
        bool ok = false;
        DWORD m = get_compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &ok);
        if (ok && !(m & TF_CONVERSIONMODE_NATIVE) && is_open()) set_open(false);
        else if (ok && (m & TF_CONVERSIONMODE_NATIVE) && !is_open()) set_open(true);
    }
    return S_OK;
}

// ---------------------------------------------------------------- キー

void TextService::apply_settings() {
    Core& c = core();
    c.settings.refresh();
    const Settings& s = c.settings.get();
    if (s.layout != layout_name_) layout_load(s.layout);
    layout_.window_ms = uint64_t(s.layout_window);
    layout_.overlap_ratio = s.layout_overlap / 100.0;
    layout_.thumbs = s.layout_thumbs;
    layout_.continuous = s.layout_continuous;
    layout_.thumb_tap = s.layout_thumb_tap;
    layout_.repeat = s.layout_repeat;
    c.use_model(s.model);   // 変換モデルを替えた (同じなら何もしない)
    c.conv.set_suggest(true);   // 提案は候補に混ぜて種類を添える (Composer)。分けて出す設定は廃止 (ユーザー 2026-10-23「統合の方がわかりやすい」)
    c.conv.set_latin(s.latin_auto);
    c.conv.set_typo(s.typo_fix);
    if (composer_) {
        composer_->options.live = s.live;
        composer_->options.live_commit = s.live_commit;
        composer_->options.live_display = s.live_display;
        composer_->options.cand_tree = s.cand_tree;
        composer_->options.space_literal = s.space_literal;
        composer_->options.personal = s.personal;
        composer_->options.caps_alpha = s.caps_alpha;
        composer_->options.space_fullwidth = s.space_fullwidth;
        composer_->options.punct = s.punct;
        composer_->options.digits_fullwidth = s.digits_fullwidth;
        composer_->options.half_symbols = from_utf8(s.half_symbols);
        composer_->options.numpad = s.numpad;
        composer_->options.private_mode = s.private_mode;
        Composer::PAGE = s.page_size;
        // ローマ字の表に足す綴り (shunti IME の設定のフォルダの romaji.tsv: 綴り<TAB>かな、# で始まる行は説明)
        std::string rt;
        std::error_code tec;
        const auto rstamp = std::filesystem::last_write_time(user_dir() / L"romaji.tsv", tec);
        const bool rchanged = tec ? romaji_stamp_ != std::filesystem::file_time_type{} : rstamp != romaji_stamp_;
        if (rchanged && tec) {   // ファイルを消した: 元の表に戻す
            composer_->reset_romaji();
            romaji_stamp_ = {};
        }
        if (rchanged && !tec && read_file(user_dir() / L"romaji.tsv", rt)) {
            romaji_stamp_ = rstamp;   // 読めたときだけ (保存の途中で読めなかったら次の打鍵でもう一度)
            composer_->reset_romaji();
            size_t i = 0;
            while (i < rt.size()) {
                size_t j = rt.find('\n', i);
                if (j == std::string::npos) j = rt.size();
                std::string line = rt.substr(i, j - i);
                i = j + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t tab = line.find('\t');
                if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
                composer_->add_romaji(from_utf8(line.substr(0, tab)), from_utf8(line.substr(tab + 1)));
            }
        }
    }
    cand_.theme = s.theme;
    strip_.theme = s.theme;
}

bool TextService::to_key_event(WPARAM vk, LPARAM lp, KeyEvent& ev) {
    ev = KeyEvent();
    ev.shift = GetKeyState(VK_SHIFT) < 0;
    ev.ctrl = GetKeyState(VK_CONTROL) < 0;
    ev.alt = GetKeyState(VK_MENU) < 0;
    switch (vk) {
        case VK_SPACE: ev.key = Key::Space; return true;
        case VK_RETURN: ev.key = Key::Enter; return true;
        case VK_BACK: ev.key = Key::Backspace; return true;
        case VK_DELETE: ev.key = Key::Delete; return true;
        case VK_ESCAPE: ev.key = Key::Escape; return true;
        case VK_LEFT: case VK_RIGHT:
            ev.key = vk == VK_LEFT ? Key::Left : Key::Right;
            // ATOK の型: 変換中は ←→ だけで文節を伸び縮み、Shift+←→ で文節を移る (Shift の意味を入れ替える)
            if (composer_ && composer_->converting() && !ev.ctrl && !ev.alt && core().settings.get().keymap == "atok") ev.shift = !ev.shift;
            return true;
        case VK_UP: ev.key = Key::Up; return true;
        case VK_DOWN: ev.key = Key::Down; return true;
        case VK_HOME: ev.key = Key::Home; return true;
        case VK_END: ev.key = Key::End; return true;
        case VK_TAB: ev.key = Key::Tab; return true;
        case VK_PRIOR: ev.key = Key::PageUp; return true;
        case VK_NEXT: ev.key = Key::PageDown; return true;
        case VK_F6: ev.key = Key::F6; return true;
        case VK_F7: ev.key = Key::F7; return true;
        case VK_F8: ev.key = Key::F8; return true;
        case VK_F9: ev.key = Key::F9; return true;
        case VK_F10: ev.key = Key::F10; return true;
        case VK_CONVERT: ev.key = Key::Henkan; return true;
        case VK_NONCONVERT: ev.key = Key::Muhenkan; return true;
        case VK_MULTIPLY: ev.ch = u'*'; break;
        case VK_ADD: ev.ch = u'+'; break;
        case VK_SUBTRACT: ev.ch = u'-'; break;
        case VK_DECIMAL: ev.ch = u'.'; break;
        case VK_DIVIDE: ev.ch = u'/'; break;
        default:
            if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ev.ch = char16_t(u'0' + (vk - VK_NUMPAD0));
            break;
    }
    ev.key = Key::Char;
    if (ev.ch) {   // テンキー: 打ったまま (かなや全角にしない)
        ev.raw = true;
        return true;
    }
    if (ev.ctrl || ev.alt) return false;
    BYTE ks[256];
    if (!GetKeyboardState(ks)) return false;
    WCHAR buf[4];
    int n = ToUnicodeEx(UINT(vk), UINT((lp >> 16) & 0xFF), ks, buf, 4, 0x4 /* キーボードの状態を変えない */, GetKeyboardLayout(0));
    if (n != 1 || buf[0] < 0x20 || buf[0] == 0x7F) return false;
    ev.ch = char16_t(buf[0]);
    return true;
}

namespace {
// 入力の記録用のキーの名前: 字はそのまま、それ以外は {bs} {sp} のように (Shift つきは {S-left})
std::string key_name(const KeyEvent& ev) {
    if (ev.key == Key::Char) return to_utf8(u16(1, ev.ch));
    static const char* names[] = {"", "sp", "enter", "bs", "del", "esc", "left", "right", "up", "down", "home", "end", "tab",
                                  "pgup", "pgdn", "f6", "f7", "f8", "f9", "f10", "henkan", "muhenkan"};
    return std::string("{") + (ev.shift ? "S-" : "") + names[int(ev.key)] + "}";
}
}  // namespace

// 選んでいる文字の読み (再変換できるときだけ。できなければ空)
u16 TextService::reconvert_reading(ITfContext* ctx) {
    u16 out;
    edit(ctx, TF_ES_SYNC | TF_ES_READ, [this, ctx, &out](TfEditCookie ec) {
        TF_SELECTION sel = {};
        ULONG n = 0;
        if (!composer_ || FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || n == 0 || !sel.range) return;
        ComPtr<ITfRange> r;
        r.Attach(sel.range);
        WCHAR buf[257];
        ULONG got = 0;
        // 256 字まで。それより長い選択は読みに戻さない (途中で切れた読みで置き換えると後ろが消える)
        if (FAILED(r->GetText(ec, 0, buf, 257, &got)) || got == 0 || got > 256) return;
        out = composer_->reading_of(u16(reinterpret_cast<const char16_t*>(buf), got));
    });
    return out;
}

// 入力欄ではない所 (ブラウザでページを読んでいる・読み取り専用の欄) か。そこでは Space などをアプリに渡す
// (IME がオンだと Space でページが送れなかった。ユーザー 2026-10-15)
static bool read_only_context(ITfContext* ctx) {
    if (!ctx) return false;
    TF_STATUS st = {};
    if (FAILED(ctx->GetStatus(&st))) return false;
    return (st.dwDynamicFlags & TF_SD_READONLY) != 0;
}

// Composer (と辞書・モデル・学習の Core) を、要るときに初めて作る
bool TextService::ensure_composer() {
    if (composer_) return true;
    if (!tm_) return false;
    Core& c = core();
    composer_ = std::make_unique<Composer>(c.conv.ok() ? &c.conv : nullptr, &c.learning, &c.dict);
    composer_->set_style_log(&c.style);
    composer_->set_app(to_utf8(u16(reinterpret_cast<const char16_t*>(app_name().c_str()))));
    composer_->flow = &c.flow;
    composer_->set_context_provider([this] { return left_ctx_; });
    if (INPUT_LOG_ENABLED) {   // デバッグ用のビルドだけ (リリース用では INPUT_LOG_ENABLED が false で、書き出す関数も空)
        composer_->on_log = [this](const LogEvent& e) {
            if (!secure_) input_log(e);
        };
        input_log_simple("diag", "activate");
    }
    apply_settings();
    return true;
}

TextService::Action TextService::classify(WPARAM vk, LPARAM lp, KeyEvent& ev, ITfContext* ctx) {
    if (!ensure_composer()) return Action::None;
    bool composing = composer_->composing();
    if (!composing) apply_settings();   // 設定画面で変えた分 (ファイルの更新を見るだけなので軽い)
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    const bool open = is_open();
    // ショートカットキー (既定も自分で追加も同じ一覧)。ほかの判定より先に、押したキーと修飾が合うかを見る。
    // 使えない状態 (入力中だけの機能を打っていないとき など) なら一致しなかったことにして、今までどおりに進む
    for (const auto& sc : core().settings.get().shortcuts) {
        // このソフトではショートカットを使わない (ゲーム): 入力中の機能 (文節の伸び縮みなど) だけ残す
        if (no_shortcuts_ && !composing) break;
        const ShortcutKey k = parse_shortcut_key(sc.first);
        if (!k.ok || k.vk != vk || k.ctrl != ctrl || k.shift != shift || k.alt != alt) continue;
        KeyEvent cev;
        if (shortcut_compose_event(sc.second, cev)) {   // 入力中だけ効く機能: KeyEvent にして composer に渡す
            if (!composing || !composer_->will_handle(cev)) continue;
            // 文節の伸び縮みは変換中だけ (入力中の Ctrl+K/L は元のキーのまま。今までと同じ)
            if ((sc.second == "seg_shrink" || sc.second == "seg_grow") && !composer_->converting()) continue;
            ev = cev;
            return Action::Compose;
        }
        if (sc.second == "ime_toggle") return Action::Toggle;
        if (sc.second == "ime_on") { if (!open) return Action::Open; continue; }
        if (sc.second == "ime_off") { if (open && !composing) return Action::Close; continue; }
        if (sc.second == "private_toggle") return Action::Private;
        if (sc.second == "undo_commit") {   // 確定した直後だけ (Ctrl+Backspace と同じ)
            if (!composing && open && !composer_->undoable().first.empty() && !read_only_context(ctx)) return Action::Undo;
            continue;
        }
        if (sc.second == "reconvert") {   // 選んだ文字を読みに戻せるときだけ
            if (!composing && open && ctx && !read_only_context(ctx) && !reconvert_reading(ctx).empty()) return Action::Reconvert;
            continue;
        }
        if (sc.second == "open_settings") return Action::OpenSettings;
        if (sc.second == "clear_all") {   // 入力中のものを全部消す (読みが空になるまで Escape)
            if (!composing) continue;
            return Action::ClearAll;
        }
        if (sc.second == "space_full" || sc.second == "space_half") {   // 何も打っていないとき・日本語入力オンで
            if (composing || !open) continue;
            return sc.second == "space_full" ? Action::SpaceFull : Action::SpaceHalf;
        }
        if (sc.second == "open_dict") return Action::OpenDict;
        if (sc.second == "model_toggle") return Action::ModelToggle;
    }
    if (open && layout_.loaded() && !ctrl && !alt && !read_only_context(ctx)) {
        const std::string k = layout_key(lp);
        if (!k.empty() && layout_.handles(k)) return Action::Layout;
    }
    if (!open) return Action::None;
    if (!composing && read_only_context(ctx)) return Action::None;   // ページを読んでいる所: Space などをアプリに渡す
    if (!to_key_event(vk, lp, ev)) return Action::None;
    return composer_->will_handle(ev) ? Action::Compose : Action::None;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) {
    KeyEvent ev;
    *eaten = classify(wp, lp, ev, ctx) != Action::None;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* ctx, WPARAM wp, LPARAM lp, BOOL* eaten) {
    KeyEvent ev;
    Action a = classify(wp, lp, ev, ctx);
    // 確定したあとの Backspace (直したしるし。消した中身は記録しない)
    if (INPUT_LOG_ENABLED && a == Action::None && wp == VK_BACK && composer_ && !composer_->composing() && !secure_ && is_open())
        input_log_simple("delchar", "IDLE");
    *eaten = a != Action::None;
    // 何も打っていないときにアプリへ渡したキー (Backspace・Enter・矢印): 文の流れに知らせ、続きの窓を消す
    if (a == Action::None && composer_ && !composer_->composing() && is_open() && to_key_event(wp, lp, ev) && ev.key != Key::Char) {
        composer_->passthrough(ev);
        if (composer_->view().post.empty()) cand_.hide();
    }
    switch (a) {
        case Action::Open: set_open(true); remember_app_open(true); break;
        case Action::Close: set_open(false); remember_app_open(false); break;
        case Action::Toggle: set_open(!is_open()); remember_app_open(is_open()); break;
        case Action::Compose: {
            ComPtr<ITfContext> keep(ctx);
            edit(ctx, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, ev](TfEditCookie ec) {
                if (!composer_) return;
                if (!composer_->composing()) {
                    if (INPUT_LOG_ENABLED) secure_ = is_secure_field(ec, keep.Get());
                    left_ctx_ = read_left_context(ec, keep.Get());
                }
                composer_->press(ev);
                apply(ec, keep.Get());
                // 入力中の 1 打鍵 (デバッグ用のビルドだけ): 押したキーと、押した後の入力中の文字列
                if (INPUT_LOG_ENABLED && !secure_) input_log_key(key_name(ev), composer_->view().text);
            });
            break;
        }
        case Action::Private: {
            Settings s = core().settings.get();
            s.private_mode = !s.private_mode;
            save_settings(user_dir() / L"settings.json", s);
            apply_settings();
            if (s.private_mode && composer_) composer_->clear_recent();
            MessageBeep(s.private_mode ? MB_ICONASTERISK : MB_OK);
            break;
        }
        case Action::Undo: {
            // カーソルの直前を確定した表記の長さだけ選び、同じ文字なら読みに戻す (違えば何もしない: カーソルを動かした等)
            ComPtr<ITfContext> keep(ctx);
            edit(ctx, TF_ES_SYNC | TF_ES_READWRITE, [this, keep](TfEditCookie ec) {
                if (!composer_) return;
                const std::pair<u16, u16> uc = composer_->undoable();
                if (uc.first.empty()) return;
                TF_SELECTION sel = {};
                ULONG n = 0;
                if (FAILED(keep->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || n == 0 || !sel.range) return;
                ComPtr<ITfRange> r;
                r.Attach(sel.range);
                BOOL empty = FALSE;
                if (FAILED(r->IsEmpty(ec, &empty)) || !empty) return;   // 文字を選んでいるときはしない
                LONG moved = 0;
                if (FAILED(r->ShiftStart(ec, -LONG(uc.first.size()), &moved, nullptr)) || moved != -LONG(uc.first.size())) return;
                std::wstring got(uc.first.size() + 1, L'\0');
                ULONG gn = 0;
                if (FAILED(r->GetText(ec, 0, got.data(), ULONG(got.size()), &gn)) || gn != uc.first.size() ||
                    got.compare(0, gn, reinterpret_cast<const wchar_t*>(uc.first.data()), gn) != 0) return;
                TF_SELECTION ns = {};
                ns.range = r.Get();
                ns.style.ase = TF_AE_NONE;
                ns.style.fInterimChar = FALSE;
                if (FAILED(keep->SetSelection(ec, 1, &ns))) return;
                left_ctx_ = read_left_context(ec, keep.Get());
                if (composer_->undo_commit()) apply(ec, keep.Get());   // 選んだ範囲を変換中の文字で置き換える
            });
            break;
        }
        case Action::Layout: {
            const std::string k = layout_key(lp);
            layout_down_.insert(k);
            layout_ctx_ = ctx;
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            const uint64_t now = layout_now_ms();
            layout_trace("d", k, now);
            layout_apply(ctx, layout_.down(k, shift, now));
            if (layout_.waiting()) layout_arm_timer();
            break;
        }
        case Action::Reconvert: {
            // 選んでいる文字を読みに戻して変換し直す。読みが分からない (ほかで入れた漢字) ときはアプリに渡す
            ComPtr<ITfContext> keep(ctx);
            const u16 reading = reconvert_reading(ctx);
            edit(ctx, TF_ES_SYNC | TF_ES_READWRITE, [this, keep, reading](TfEditCookie ec) {
                if (!composer_ || reading.empty()) return;
                left_ctx_ = read_left_context(ec, keep.Get());
                if (composer_->start_reconvert(reading)) apply(ec, keep.Get());
            });
            break;
        }
        case Action::OpenSettings: {
            // ショートカットキー「設定を開く」: 同じフォルダの shunti_settings.exe を起動
            const std::wstring exe = (module_dir() / L"shunti_settings.exe").wstring();
            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case Action::ClearAll: {
            // 入力中のものを全部消す (読みが空になるまで Escape。Composer の cancel と同じ)
            ComPtr<ITfContext> keep(ctx);
            edit(ctx, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep](TfEditCookie ec) {
                if (!composer_) return;
                while (composer_->composing()) {
                    KeyEvent esc;
                    esc.key = Key::Escape;
                    composer_->press(esc);
                }
                apply(ec, keep.Get());
            });
            break;
        }
        case Action::SpaceFull:
        case Action::SpaceHalf: {
            const bool full = a == Action::SpaceFull;
            ComPtr<ITfContext> keep(ctx);
            edit(ctx, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, full](TfEditCookie ec) {
                if (!composer_) return;
                const u16 t(1, full ? u'　' : u' ');
                ComPtr<ITfInsertAtSelection> ins;
                ComPtr<ITfRange> r;
                if (SUCCEEDED(keep->QueryInterface(IID_PPV_ARGS(&ins))) &&
                    SUCCEEDED(ins->InsertTextAtSelection(ec, 0, W(t), LONG(t.size()), &r)) && r) {
                    r->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel = {};
                    sel.range = r.Get();
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    keep->SetSelection(ec, 1, &sel);
                }
            });
            break;
        }
        case Action::OpenDict: {
            // ショートカットキー「ユーザー辞書を開く」: 設定を --dict で起動し、辞書の欄を開く
            const std::wstring exe = (module_dir() / L"shunti_settings.exe").wstring();
            ShellExecuteW(nullptr, L"open", exe.c_str(), L"--dict", nullptr, SW_SHOWNORMAL);
            break;
        }
        case Action::ModelToggle: {
            // 変換モデルを XS3 ⇔ S7 で入れ替えて保存・反映 (切り替えたら音で知らせる)
            Settings s = core().settings.get();
            s.model = s.model == "light" ? "standard" : "light";
            save_settings(user_dir() / L"settings.json", s);
            apply_settings();
            MessageBeep(MB_OK);
            break;
        }
        case Action::None: break;
    }
    return S_OK;
}

// ---------------------------------------------------------------- キー配列 (core/layout.h)

std::string TextService::layout_key(LPARAM lp) {
    // 親指キーの割り当て (無変換・空白など) に合わせて読み替えた名前
    return layout_.map_key(shunti::key_name_from_scancode(unsigned((lp >> 16) & 0xFF), ((lp >> 24) & 1) != 0));
}

void TextService::layout_load(const std::string& name) {
    layout_name_ = name;
    layout_ = shunti::Layout();
    layout_down_.clear();
    if (name.empty()) return;
    const std::wstring file = L"layout_" + std::wstring(name.begin(), name.end()) + L".tsv";
    for (const auto& dir : {user_dir(), module_dir()}) {   // 自分で足した配列 (設定のフォルダ) が先
        std::string t;
        if (read_file(dir / file, t) && layout_.load(t)) return;
    }
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM lp, BOOL* eaten) {
    *eaten = layout_down_.count(layout_key(lp)) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* ctx, WPARAM, LPARAM lp, BOOL* eaten) {
    const std::string k = layout_key(lp);
    *eaten = FALSE;
    if (!layout_down_.count(k)) return S_OK;
    layout_down_.erase(k);
    *eaten = TRUE;
    const uint64_t now = layout_now_ms();
    layout_trace("u", k, now);
    layout_apply(ctx, layout_.up(k, now));
    if (layout_.waiting()) layout_arm_timer();
    return S_OK;
}

static TextService* g_layout_owner = nullptr;   // 時間切れのタイマーの持ち主 (タイマーはスレッドに 1 つ)

void TextService::layout_arm_timer() {
    g_layout_owner = this;
    layout_timer_ = SetTimer(nullptr, layout_timer_, UINT(layout_.window_ms + 5), layout_timer_proc);
}

void CALLBACK TextService::layout_timer_proc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    TextService* self = g_layout_owner;
    if (!self) return;
    self->layout_timer_ = 0;
    auto outs = self->layout_.tick(layout_now_ms());
    if (!outs.empty() && self->layout_ctx_) self->layout_apply(self->layout_ctx_.Get(), outs);
    if (self->layout_.waiting()) self->layout_arm_timer();
}

// 配列で決まった字・打鍵を入れる。英字の配列は英字としてローマ字へ、かなの配列はかなのまま。
// BS・Enter・矢印は、打っている間はこちらで、何も打っていないときはアプリへ (キーを送り直す)
void TextService::layout_apply(ITfContext* ctx, const std::vector<shunti::Layout::Out>& outs) {
    if (outs.empty() || !ctx) return;
    ComPtr<ITfContext> keep(ctx);
    const bool letters = layout_.kind() == shunti::Layout::Kind::Letters;
    edit(ctx, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, outs, letters](TfEditCookie ec) {
        std::vector<WORD> to_app;   // 何も打っていないときの BS・Enter・矢印 (アプリへ送り直す)
        if (!composer_) return;
        for (const auto& o : outs) {
            if (!composer_->composing()) left_ctx_ = read_left_context(ec, keep.Get());
            if (!o.text.empty()) {
                if (letters) {
                    for (char16_t c : o.text) {
                        KeyEvent ev;
                        ev.key = Key::Char;
                        ev.ch = c;
                        composer_->press(ev);
                    }
                } else {
                    composer_->type_kana(o.text);
                }
                continue;
            }
            KeyEvent ev;
            WORD vk = 0;
            if (o.key == "bs") { ev.key = Key::Backspace; vk = VK_BACK; }
            else if (o.key == "enter") { ev.key = Key::Enter; vk = VK_RETURN; }
            else if (o.key == "left") { ev.key = Key::Left; vk = VK_LEFT; }
            else if (o.key == "right") { ev.key = Key::Right; vk = VK_RIGHT; }
            else if (o.key == "space") { ev.key = Key::Space; vk = VK_SPACE; }
            else if (o.key == "henkan") { ev.key = Key::Henkan; }
            else if (o.key == "muhenkan") {
                ev.key = Key::Muhenkan;
                if (!composer_->composing()) { vk = 0; to_app.push_back(0xFFFF); continue; }
            } else continue;
            if (composer_->will_handle(ev)) composer_->press(ev);
            else if (vk) to_app.push_back(vk);
        }
        apply(ec, keep.Get());
        for (WORD vk : to_app) {
        if (vk == 0xFFFF) { set_open(false); remember_app_open(false); continue; }   // 親指の無変換を単独で: 日本語入力をオフ
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = vk;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        }
    });
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
    set_open(!is_open());
    remember_app_open(is_open());
    *eaten = TRUE;
    return S_OK;
}

// ---------------------------------------------------------------- 入力欄への反映

HRESULT TextService::edit(ITfContext* ctx, DWORD flags, std::function<void(TfEditCookie)> fn) {
    if (!ctx) return E_INVALIDARG;
    auto* es = new EditSession(std::move(fn));
    HRESULT hr = E_FAIL;
    HRESULT r = ctx->RequestEditSession(cid_, es, flags, &hr);
    es->Release();
    return FAILED(r) ? r : hr;
}

u16 TextService::read_left_context(TfEditCookie ec, ITfContext* ctx) {
    TF_SELECTION sel = {};
    ULONG n = 0;
    if (FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || n == 0 || !sel.range) return u16();
    ComPtr<ITfRange> r;
    r.Attach(sel.range);
    r->Collapse(ec, TF_ANCHOR_START);
    LONG moved = 0;
    r->ShiftStart(ec, -CTX_CHARS, &moved, nullptr);
    WCHAR buf[CTX_CHARS + 8];
    ULONG got = 0;
    if (FAILED(r->GetText(ec, 0, buf, CTX_CHARS + 8, &got))) return u16();
    return u16(reinterpret_cast<const char16_t*>(buf), got);
}

void TextService::set_attributes(TfEditCookie ec, ITfContext* ctx, ITfRange* comp_range, const View& v) {
    ComPtr<ITfProperty> prop;
    if (FAILED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) return;
    prop->Clear(ec, comp_range);
    for (auto& sp : v.spans) {
        TfGuidAtom atom = sp.kind == SpanKind::Input ? atom_input_ : sp.kind == SpanKind::Converted ? atom_converted_
                        : sp.kind == SpanKind::Corrected ? atom_corrected_ : atom_focused_;
        if (atom == TF_INVALID_GUIDATOM) continue;
        ComPtr<ITfRange> r;
        if (FAILED(comp_range->Clone(&r))) continue;
        LONG moved = 0;
        r->Collapse(ec, TF_ANCHOR_START);
        r->ShiftEnd(ec, sp.start + sp.len, &moved, nullptr);
        r->ShiftStart(ec, sp.start, &moved, nullptr);
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_I4;
        var.lVal = LONG(atom);
        prop->SetValue(ec, r.Get(), &var);
    }
}

void TextService::end_composition(TfEditCookie ec, ITfContext* ctx) {
    if (!comp_) return;
    ComPtr<ITfRange> r;
    if (SUCCEEDED(comp_->GetRange(&r))) {
        ComPtr<ITfProperty> prop;
        if (SUCCEEDED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) prop->Clear(ec, r.Get());
    }
    comp_->EndComposition(ec);
    comp_->Release();
    comp_ = nullptr;
    if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; }
}

void TextService::apply(TfEditCookie ec, ITfContext* ctx) {
    u16 commit = composer_->take_commit();
    const View& v = composer_->view();
    auto set_caret = [&](ITfRange* at) {
        TF_SELECTION sel = {};
        sel.range = at;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        ctx->SetSelection(ec, 1, &sel);
    };

    // 確定する文: 入力中の文字があればそれを置き換えて終わらせる。無ければカーソルの所に入れる
    if (!commit.empty()) {
        if (comp_) {
            ComPtr<ITfRange> r;
            if (SUCCEEDED(comp_->GetRange(&r))) {
                r->SetText(ec, 0, W(commit), LONG(commit.size()));
                ComPtr<ITfRange> end;
                if (SUCCEEDED(r->Clone(&end))) {
                    end->Collapse(ec, TF_ANCHOR_END);
                    set_caret(end.Get());
                }
            }
            end_composition(ec, ctx);
        } else {
            ComPtr<ITfInsertAtSelection> ins;
            ComPtr<ITfRange> r;
            if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ins))) &&
                SUCCEEDED(ins->InsertTextAtSelection(ec, 0, W(commit), LONG(commit.size()), &r)) && r) {
                r->Collapse(ec, TF_ANCHOR_END);
                set_caret(r.Get());
            }
        }
    }

    // 入力中の文字
    if (!v.text.empty()) {
        if (!comp_) {
            ComPtr<ITfInsertAtSelection> ins;
            ComPtr<ITfRange> r;
            ComPtr<ITfContextComposition> cc;
            if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ins))) ||
                FAILED(ins->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &r)) || !r ||
                FAILED(ctx->QueryInterface(IID_PPV_ARGS(&cc))) ||
                FAILED(cc->StartComposition(ec, r.Get(), static_cast<ITfCompositionSink*>(this), &comp_)) || !comp_) {
                comp_ = nullptr;
                cand_.hide();
                strip_.hide();
                return;
            }
            comp_ctx_ = ctx;
            comp_ctx_->AddRef();
        }
        ComPtr<ITfRange> r;
        if (FAILED(comp_->GetRange(&r))) return;
        r->SetText(ec, 0, W(v.text), LONG(v.text.size()));
        set_attributes(ec, ctx, r.Get(), v);
        ComPtr<ITfRange> c;
        if (SUCCEEDED(r->Clone(&c))) {
            LONG moved = 0;
            c->Collapse(ec, TF_ANCHOR_START);
            c->ShiftStart(ec, v.caret, &moved, nullptr);
            c->Collapse(ec, TF_ANCHOR_START);
            set_caret(c.Get());
        }
    } else if (comp_) {
        ComPtr<ITfRange> r;
        if (SUCCEEDED(comp_->GetRange(&r))) r->SetText(ec, 0, L"", 0);
        end_composition(ec, ctx);
    }
    update_candidates(ec, ctx, v);
}

// 候補の窓の置き場所が取れなかったとき (Firefox は配置が決まるまで位置を教えず、キャレットも知らせない: 窓が左上の隅に出た。
// ブログの報告 2026-10-18): キャレット → 同じ窓で最後に取れた位置 → 入力している窓の下の辺り、の順に。取れた位置は覚えておく
void TextService::place_fallback(bool ok, RECT& rc, HWND hwnd) {
    if (ok && (rc.left != 0 || rc.top != 0)) {
        last_rc_ = rc;
        last_rc_root_ = GetAncestor(hwnd, GA_ROOT);
        return;
    }
    GUITHREADINFO gi = {sizeof gi};
    if (GetGUIThreadInfo(0, &gi) && gi.hwndCaret && (gi.rcCaret.right > gi.rcCaret.left || gi.rcCaret.bottom > gi.rcCaret.top)) {
        rc = gi.rcCaret;
        MapWindowPoints(gi.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        return;
    }
    HWND root = GetAncestor(hwnd ? hwnd : GetForegroundWindow(), GA_ROOT);
    if (root && root == last_rc_root_ && (last_rc_.left != 0 || last_rc_.top != 0)) { rc = last_rc_; return; }
    RECT w = {};
    if (root && GetWindowRect(root, &w)) {   // 窓の下の方の左寄り
        const int x = w.left + (w.right - w.left) / 4, y = w.bottom - (w.bottom - w.top) / 4;
        rc = {x, y - 20, x + 1, y};
    }
}

// 打った綴りの帯を入力中の字の頭の上に (入力中はいつも出す)
void TextService::update_strip(TfEditCookie ec, ITfContext* ctx, const View& v) {
    if (!comp_ || v.typed.empty()) { strip_.hide(); return; }
    ComPtr<ITfContextView> view;
    ComPtr<ITfRange> r, a;
    RECT rc = {};
    BOOL clipped = FALSE;
    HWND hwnd = nullptr;
    bool ok = SUCCEEDED(ctx->GetActiveView(&view)) && SUCCEEDED(comp_->GetRange(&r)) && SUCCEEDED(r->Clone(&a));
    if (ok) {
        LONG moved = 0;
        a->Collapse(ec, TF_ANCHOR_START);
        a->ShiftEnd(ec, 1, &moved, nullptr);
        ok = view->GetTextExt(ec, a.Get(), &rc, &clipped) == S_OK && (rc.right > rc.left || rc.bottom > rc.top);
        view->GetWnd(&hwnd);
    }
    if (!hwnd) hwnd = GetFocus();
    place_fallback(ok, rc, hwnd);
    strip_.show(v.typed, v.typed_fixed, v.typed_mark, rc, hwnd);
}

void TextService::update_candidates(TfEditCookie ec, ITfContext* ctx, const View& v) {
    // 確定した後の文の流れの続き (お疲れ様です -> 。 -> 本日の進捗報告です。): カーソルの所に小さく。Tab かクリックで入れる
    if (!comp_) strip_.hide();
    if (!comp_ && !v.post.empty()) {
        ComPtr<ITfContextView> view;
        RECT rc = {};
        BOOL clipped = FALSE;
        HWND hwnd = nullptr;
        TF_SELECTION sel = {};
        ULONG n = 0;
        bool ok = SUCCEEDED(ctx->GetActiveView(&view)) && SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) && n == 1;
        if (ok) {
            ok = view->GetTextExt(ec, sel.range, &rc, &clipped) == S_OK && (rc.right > rc.left || rc.bottom > rc.top);
            view->GetWnd(&hwnd);
            sel.range->Release();
        }
        if (!hwnd) hwnd = GetFocus();
        place_fallback(ok, rc, hwnd);
        post_ctx_ = ctx;
        cand_.label = T(130);
        cand_.set_tree({}, -1, -1);
        cand_.show({}, -1, rc, hwnd, v.post, -1);
        return;
    }
    cand_.label.clear();
    update_strip(ec, ctx, v);
    if (!comp_ || ((!v.cand_open || v.cands.empty()) && v.sugg.empty() && v.tree.empty())) { cand_.hide(); return; }
    // 候補の木があるときは、木の頭を入力中の文字の頭にそろえる (窓を文字の頭の下に出す)
    const int at = v.tree.empty() ? v.anchor : 0;
    ComPtr<ITfContextView> view;
    ComPtr<ITfRange> r, a;
    RECT rc = {};
    BOOL clipped = FALSE;
    HWND hwnd = nullptr;
    bool ok = SUCCEEDED(ctx->GetActiveView(&view)) && SUCCEEDED(comp_->GetRange(&r)) && SUCCEEDED(r->Clone(&a));
    if (ok) {
        LONG moved = 0;
        a->Collapse(ec, TF_ANCHOR_START);
        a->ShiftEnd(ec, at + 1, &moved, nullptr);
        a->ShiftStart(ec, at, &moved, nullptr);
        ok = view->GetTextExt(ec, a.Get(), &rc, &clipped) == S_OK && (rc.right > rc.left || rc.bottom > rc.top);
        view->GetWnd(&hwnd);
    }
    if (!hwnd) hwnd = GetFocus();
    place_fallback(ok, rc, hwnd);
    cand_.set_tree(v.tree, v.tree_rs, v.tree_re, v.cand_short, v.cand_scores, v.cand_fork);
    cand_.show(v.cand_open ? v.cands : std::vector<u16>{}, v.cand_sel, rc, hwnd, v.sugg, v.sugg_sel, v.cand_notes);
}

void TextService::finish_composition() {
    if (!composer_ || !composer_->composing() || !comp_ctx_) {
        cand_.hide();
        strip_.hide();
        return;
    }
    ComPtr<ITfContext> keep(comp_ctx_);
    edit(keep.Get(), TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep](TfEditCookie ec) {
        if (!composer_) return;
        composer_->finish();
        apply(ec, keep.Get());
    });
}

void TextService::on_suggestion_clicked(int index) {
    if (composer_ && !composer_->composing() && post_ctx_) {   // 文の流れの続きを押した
        ComPtr<ITfContext> keep(post_ctx_);
        edit(keep.Get(), TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, index](TfEditCookie ec) {
            if (!composer_) return;
            composer_->select_post(index);
            apply(ec, keep.Get());
        });
        return;
    }
    if (!composer_ || !comp_ctx_) return;
    ComPtr<ITfContext> keep(comp_ctx_);
    edit(keep.Get(), TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, index](TfEditCookie ec) {
        if (!composer_) return;
        composer_->select_suggestion(index);
        apply(ec, keep.Get());
    });
}

void TextService::on_candidate_clicked(int index) {
    if (!composer_ || !comp_ctx_) return;
    ComPtr<ITfContext> keep(comp_ctx_);
    edit(keep.Get(), TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, [this, keep, index](TfEditCookie ec) {
        if (!composer_) return;
        composer_->select_candidate(index);
        apply(ec, keep.Get());
    });
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* comp) {
    // アプリが入力中の文字を終わらせた (クリックで別の所に移ったなど)。文字はそのまま残るので、こちらは忘れるだけ
    if (comp_ && comp == comp_) {
        comp_->Release();
        comp_ = nullptr;
        if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; }
    }
    if (composer_) composer_->cancel();
    cand_.hide();
    strip_.hide();
    return S_OK;
}

bool TextService::is_secure_field(TfEditCookie ec, ITfContext* ctx) {
    ComPtr<ITfReadOnlyProperty> prop;
    TF_SELECTION sel = {};
    ULONG n = 0;
    // GUID_PROP_INPUTSCOPE (uuid.lib に入っていないので値を書く)
    static const GUID PROP_INPUTSCOPE = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};
    if (FAILED(ctx->GetAppProperty(PROP_INPUTSCOPE, &prop)) || !prop) return false;
    if (FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || !n || !sel.range) return false;
    ComPtr<ITfRange> r;
    r.Attach(sel.range);
    VARIANT v;
    VariantInit(&v);
    bool secure = false;
    if (SUCCEEDED(prop->GetValue(ec, r.Get(), &v)) && v.vt == VT_UNKNOWN && v.punkVal) {
        ComPtr<ITfInputScope> is;
        InputScope* scopes = nullptr;
        UINT count = 0;
        if (SUCCEEDED(v.punkVal->QueryInterface(IID_PPV_ARGS(&is))) && SUCCEEDED(is->GetInputScopes(&scopes, &count))) {
            for (UINT i = 0; i < count; i++)
                if (scopes[i] == IS_PASSWORD || scopes[i] == IS_NUMERIC_PASSWORD || scopes[i] == IS_NUMERIC_PIN ||
                    scopes[i] == IS_ALPHANUMERIC_PIN || scopes[i] == IS_ALPHANUMERIC_PIN_SET)
                    secure = true;
            CoTaskMemFree(scopes);
        }
    }
    VariantClear(&v);
    return secure;
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) {
    if (INPUT_LOG_ENABLED) input_log_simple("field");
    // 別の入力欄に移った: 入力中のものは確定し、文脈の代わりに覚えていた文を忘れる
    finish_composition();
    cand_.hide();
    strip_.hide();
    if (composer_) composer_->reset_history();
    return S_OK;
}

// ---------------------------------------------------------------- 見せ方の提供

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** out) {
    if (!out) return E_INVALIDARG;
    *out = new shunti::win::EnumDisplayAttributeInfo();
    return S_OK;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** out) {
    if (!out) return E_INVALIDARG;
    *out = make_display_attribute(guid);
    return *out ? S_OK : E_INVALIDARG;
}

}  // namespace win
}  // namespace shunti

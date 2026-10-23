#include "shunti.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace shunti {
namespace fx {

namespace {

// 学習とユーザー辞書を置くフォルダ ($XDG_DATA_HOME/shunti-ime、無ければ ~/.local/share/shunti-ime)
fs::path user_dir() {
    const char* x = getenv("XDG_DATA_HOME");
    fs::path base = x && *x ? fs::path(x) : fs::path(getenv("HOME") ? getenv("HOME") : ".") / ".local" / "share";
    fs::path d = base / "shunti-ime";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// 辞書とモデルのあるフォルダ ($SHUNTI_DATA_DIR → ~/.local/share/shunti-ime → /usr/local/share → /usr/share の順に探す)
fs::path data_dir() {
    std::vector<fs::path> cands;
    if (const char* d = getenv("SHUNTI_DATA_DIR")) cands.emplace_back(d);
    cands.push_back(user_dir());
    cands.emplace_back("/usr/local/share/shunti-ime");
    cands.emplace_back("/usr/share/shunti-ime");
    for (auto& d : cands) {
        std::error_code ec;
        if (fs::exists(d / "kkc_lex.bin", ec) && fs::exists(d / "kkc_model.bin", ec)) return d;
    }
    return {};
}

// ファイルを読み取り専用でメモリに写す (閉じない。プロセスが終わるまで使う)
const void* map_file(const fs::path& p, size_t& size) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    const void* view = nullptr;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        void* m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            view = m;
            size = size_t(st.st_size);
        }
    }
    close(fd);
    return view;
}

// Fcitx5 のキー → Composer のキー。扱わないキーなら false
bool to_key(const fcitx::Key& k, KeyEvent& ev) {
    ev = KeyEvent();
    auto st = k.states();
    ev.shift = st.test(fcitx::KeyState::Shift);
    ev.ctrl = st.test(fcitx::KeyState::Ctrl);
    ev.alt = st.test(fcitx::KeyState::Alt);
    if (st.test(fcitx::KeyState::Super)) return false;
    switch (k.sym()) {
        case FcitxKey_space: ev.key = Key::Space; return true;
        case FcitxKey_Return:
        case FcitxKey_KP_Enter: ev.key = Key::Enter; return true;
        case FcitxKey_BackSpace: ev.key = Key::Backspace; return true;
        case FcitxKey_Delete:
        case FcitxKey_KP_Delete: ev.key = Key::Delete; return true;
        case FcitxKey_Escape: ev.key = Key::Escape; return true;
        case FcitxKey_Left:
        case FcitxKey_KP_Left: ev.key = Key::Left; return true;
        case FcitxKey_Right:
        case FcitxKey_KP_Right: ev.key = Key::Right; return true;
        case FcitxKey_Up:
        case FcitxKey_KP_Up: ev.key = Key::Up; return true;
        case FcitxKey_Down:
        case FcitxKey_KP_Down: ev.key = Key::Down; return true;
        case FcitxKey_Home: ev.key = Key::Home; return true;
        case FcitxKey_End: ev.key = Key::End; return true;
        case FcitxKey_Tab:
        case FcitxKey_ISO_Left_Tab: ev.key = Key::Tab; return true;
        case FcitxKey_Page_Up: ev.key = Key::PageUp; return true;
        case FcitxKey_Page_Down: ev.key = Key::PageDown; return true;
        case FcitxKey_F6: ev.key = Key::F6; return true;
        case FcitxKey_F7: ev.key = Key::F7; return true;
        case FcitxKey_F8: ev.key = Key::F8; return true;
        case FcitxKey_F9: ev.key = Key::F9; return true;
        case FcitxKey_F10: ev.key = Key::F10; return true;
        case FcitxKey_Henkan_Mode: ev.key = Key::Henkan; return true;
        case FcitxKey_Muhenkan: ev.key = Key::Muhenkan; return true;
        case FcitxKey_KP_Multiply: ev.ch = u'*'; break;
        case FcitxKey_KP_Add: ev.ch = u'+'; break;
        case FcitxKey_KP_Subtract: ev.ch = u'-'; break;
        case FcitxKey_KP_Decimal: ev.ch = u'.'; break;
        case FcitxKey_KP_Divide: ev.ch = u'/'; break;
        default:
            if (k.sym() >= FcitxKey_KP_0 && k.sym() <= FcitxKey_KP_9) ev.ch = char16_t(u'0' + (k.sym() - FcitxKey_KP_0));
            break;
    }
    ev.key = Key::Char;
    if (ev.ch) {   // テンキー: 打ったまま (かなや全角にしない)
        ev.raw = true;
        return true;
    }
    if (ev.ctrl || ev.alt) return false;
    uint32_t u = fcitx::Key::keySymToUnicode(k.sym());
    if (u < 0x20 || u == 0x7F || u > 0xFFFF) return false;
    ev.ch = char16_t(u);
    return true;
}

// 候補の窓の 1 つ (マウスで選んだとき)
class Candidate : public fcitx::CandidateWord {
public:
    Candidate(ShuntiEngine* engine, int index, const std::string& text)
        : fcitx::CandidateWord(fcitx::Text(text)), engine_(engine), index_(index) {}
    void select(fcitx::InputContext* ic) const override { engine_->state(ic)->selectCandidate(index_); }

private:
    ShuntiEngine* engine_;
    int index_;
};

}  // namespace

Core::Core() : learning(user_dir() / "learned.json"), dict(user_dir() / "userdict.json") {
    flow.load(user_dir() / "flow.tsv");
    dir_ = data_dir();
    if (dir_.empty()) return;
    lex_ = map_file(dir_ / "kkc_lex.bin", lex_size_);
}

void Core::use_model(const std::string& name) {
    if (!lex_) return;
    std::string file = model_file_name(name);
    std::error_code ec;
    if (!fs::exists(dir_ / file, ec)) file = model_file_name("standard");
    if (file == model_file_) return;
    size_t ms = 0;
    const void* model = map_file(dir_ / file, ms);
    if (!model) return;
    if (!conv.open(lex_, lex_size_, model, ms, 2)) {
        munmap(const_cast<void*>(model), ms);
        return;
    }
    if (model_) munmap(const_cast<void*>(model_), model_size_);   // 前のモデル (エンジンは開き直したので、もう使っていない)
    model_ = model;
    model_size_ = ms;
    model_file_ = file;
}

// ---------------------------------------------------------------- 入力欄ごとの状態

ShuntiState::ShuntiState(ShuntiEngine* engine, fcitx::InputContext* ic)
    : engine_(engine), ic_(ic),
      composer_(engine->core().conv.ok() ? &engine->core().conv : nullptr, &engine->core().learning, &engine->core().dict) {
    composer_.flow = &engine->core().flow;
    // 変換の文脈 = 入力欄のカーソルの左 (アプリが教えてくれるときだけ。教えてくれなければこれまでに確定した文を使う)
    composer_.set_context_provider([this]() -> u16 {
        if (!ic_->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) return {};
        const auto& st = ic_->surroundingText();
        if (!st.isValid()) return {};
        const std::string& t = st.text();
        size_t bytes = fcitx::utf8::ncharByteLength(t.begin(), std::min<size_t>(st.cursor(), fcitx::utf8::length(t)));
        u16 left = from_utf8(t.substr(0, bytes));
        return left.size() > 40 ? left.substr(left.size() - 40) : left;
    });
}

void ShuntiState::keyEvent(fcitx::KeyEvent& event) {
    if (event.isRelease()) return;
    KeyEvent ev;
    if (!to_key(event.key(), ev)) return;
    if (!composer_.composing()) {   // 設定の画面で変えた分
        engine_->applyConfig(composer_);
        engine_->useModel();
    }
    if (!composer_.will_handle(ev)) {
        // 何も打っていないときにアプリへ渡すキー (Backspace・Enter・矢印): 文の流れに知らせる
        if (!composer_.composing() && ev.key != Key::Char) {
            const bool had = !composer_.view().post.empty();
            composer_.passthrough(ev);
            if (had) apply();
        }
        return;
    }
    composer_.press(ev);
    apply();
    event.filterAndAccept();
}

void ShuntiState::apply() {
    u16 commit = composer_.take_commit();
    if (!commit.empty()) ic_->commitString(to_utf8(commit));
    const View& v = composer_.view();
    auto& panel = ic_->inputPanel();
    panel.reset();
    if (!v.text.empty()) {
        // 入力中の文字: 入力中・変換済みは下線、選んでいる文節は強調
        fcitx::Text t;
        int pos = 0;
        auto put = [&](int start, int len, fcitx::TextFormatFlags flags) {
            if (len > 0) t.append(to_utf8(v.text.substr(size_t(start), size_t(len))), flags);
        };
        for (const Span& sp : v.spans) {
            put(pos, sp.start - pos, fcitx::TextFormatFlag::Underline);
            put(sp.start, sp.len, sp.kind == SpanKind::Focused ? fcitx::TextFormatFlags{fcitx::TextFormatFlag::HighLight}
                                                                  : fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
            pos = sp.start + sp.len;
        }
        put(pos, int(v.text.size()) - pos, fcitx::TextFormatFlag::Underline);
        t.setCursor(int(to_utf8(v.text.substr(0, size_t(std::max(0, v.caret)))).size()));
        if (ic_->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) panel.setClientPreedit(t);
        else panel.setPreedit(t);
    }
    // 文の流れの続き (確定した後。Tab で 1 つ目を入れる)
    if (v.text.empty() && !v.post.empty()) {
        std::string a = "続き (Tab):";
        for (size_t i = 0; i < v.post.size(); i++) a += "  " + to_utf8(v.post[i]);
        panel.setAuxUp(fcitx::Text(a));
    }
    // 提案 (候補の上の補助の行に。Shift+Enter で 1 つ目を確定)
    if (!v.sugg.empty()) {
        // 選んでいる提案は [ ] で囲む (Shift+↓ で列に入り ←→ で選ぶ)
        std::string a = "提案 (Shift+↓):";
        for (size_t i = 0; i < v.sugg.size() && i < 6; i++)
            a += int(i) == v.sugg_sel ? "  [" + to_utf8(v.sugg[i]) + "]" : "  " + to_utf8(v.sugg[i]);
        panel.setAuxUp(fcitx::Text(a));
    }
    if (v.cand_open && !v.cands.empty()) {
        auto list = std::make_unique<fcitx::CommonCandidateList>();
        list->setPageSize(Composer::PAGE);
        list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
        std::vector<std::string> labels;
        for (int i = 1; i <= Composer::PAGE; i++) labels.push_back(std::to_string(i) + ". ");
        list->setLabels(labels);
        // 提案の種類は候補の後ろに添える (もしかして・絵文字 …。2026-10-14)
        for (size_t i = 0; i < v.cands.size(); i++) {
            std::string t = to_utf8(v.cands[i]);
            if (i < v.cand_notes.size() && !v.cand_notes[i].empty()) t += "  " + to_utf8(v.cand_notes[i]);
            list->append<Candidate>(engine_, int(i), t);
        }
        if (v.cand_sel >= 0 && v.cand_sel < int(v.cands.size())) {
            list->setPage(v.cand_sel / Composer::PAGE);   // 選んでいる候補のページを見せる (1〜9 の数字と同じページ)
            list->setGlobalCursorIndex(v.cand_sel);
        }
        panel.setCandidateList(std::move(list));
    }
    ic_->updatePreedit();
    ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ShuntiState::finish() {
    if (composer_.composing()) composer_.finish();
    apply();
}

void ShuntiState::cancel() {
    if (composer_.composing()) composer_.cancel();
    apply();
}

void ShuntiState::selectCandidate(int index) {
    composer_.select_candidate(index);
    apply();
}

// ---------------------------------------------------------------- エンジン

ShuntiEngine::ShuntiEngine(fcitx::Instance* instance)
    : instance_(instance), factory_([this](fcitx::InputContext& ic) { return new ShuntiState(this, &ic); }) {
    instance_->inputContextManager().registerProperty("shuntiState", &factory_);
    reloadConfig();
}

Core& ShuntiEngine::core() {
    if (!core_) {
        core_ = std::make_unique<Core>();   // 辞書とモデルは最初に使うときに開く
        useModel();
    }
    return *core_;
}

void ShuntiEngine::useModel() {
    static const char* const NAMES[] = {"light", "standard", "standard"};   // 2 (高精度 M4) は廃止、標準 (S7) に (2026-10-14)
    if (core_) core_->use_model(NAMES[std::clamp(*config_.model, 0, 2)]);
    if (core_) core_->conv.set_suggest(true);   // 提案は候補に混ぜて種類を添える (分けて出す設定は廃止、2026-10-23)
}

void ShuntiEngine::applyConfig(Composer& c) const {
    c.options.live = *config_.live;
    c.options.live_commit = *config_.liveCommit;
    c.options.live_display = *config_.liveDisplay;
    c.options.space_fullwidth = *config_.spaceFullwidth;
    c.options.digits_fullwidth = *config_.digitsFullwidth;
    c.options.punct = *config_.punct;
}

void ShuntiEngine::setConfig(const fcitx::RawConfig& raw) {
    config_.load(raw, true);
    fcitx::safeSaveAsIni(config_, "conf/shunti.conf");
}

void ShuntiEngine::reloadConfig() { fcitx::readAsIni(config_, "conf/shunti.conf"); }

void ShuntiEngine::keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) { state(event.inputContext())->keyEvent(event); }

void ShuntiEngine::activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    state(event.inputContext())->focusIn();
}

void ShuntiEngine::deactivate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    state(event.inputContext())->finish();   // 別の入力方法に切り替えた: 見えているまま確定
}

void ShuntiEngine::reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) {
    // 入力欄から離れた: 見えているまま確定 (Windows 版と同じ)。アプリが入力欄を作り直した: 捨てる
    if (event.type() == fcitx::EventType::InputContextFocusOut) state(event.inputContext())->finish();
    else state(event.inputContext())->cancel();
}

}  // namespace fx
}  // namespace shunti

FCITX_ADDON_FACTORY(shunti::fx::ShuntiEngineFactory);

#include "composer.h"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "datetime.h"
#include "tables.h"

namespace shunti {

// 変換で受け取る候補の数。10 では短い読み (き・かん) の単漢字が足りない (期・季・記が 11 位より後ろにある)
constexpr int kCandidates = 30;

namespace {
constexpr size_t CTX_MAX = 40;   // モデルの文脈の長さ

bool is_digit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool is_upper(char16_t c) { return c >= u'A' && c <= u'Z'; }
bool is_lower(char16_t c) { return c >= u'a' && c <= u'z'; }

u16 tail(const u16& s, size_t n) { return s.size() > n ? s.substr(s.size() - n) : s; }

bool contains(const std::vector<u16>& v, const u16& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
void push_unique(std::vector<u16>& v, const u16& s) {
    if (!s.empty() && !contains(v, s)) v.push_back(s);
}

u16 lower(u16 s) { for (auto& c : s) if (is_upper(c)) c = char16_t(c + 32); return s; }
u16 upper(u16 s) { for (auto& c : s) if (is_lower(c)) c = char16_t(c - 32); return s; }
u16 capital(u16 s) {
    s = lower(s);
    if (!s.empty() && is_lower(s[0])) s[0] = char16_t(s[0] - 32);
    return s;
}

u16 replace_all(u16 s, const u16& from, const u16& to) {
    for (size_t i = s.find(from); i != u16::npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
    return s;
}

// 挨拶はかな書きを 1 位に (スマホ版 preferGreetings と同じ決まり)
void prefer_greetings(const u16& target, std::vector<u16>& cands) {
    if (cands.empty()) return;
    const u16& top = cands[0];
    u16 trimmed = target;
    while (!trimmed.empty() && u16(u"、。！？!?ー〜").find(trimmed.back()) != u16::npos) trimmed.pop_back();
    struct Rule { const char16_t* kanji; const char16_t* kana; bool ok; };
    const Rule rules[] = {
        {u"今日は", u"こんにちは", target.find(u"こんにちは") != u16::npos && target.find(u"きょうは") == u16::npos},
        {u"今晩は", u"こんばんは", trimmed == u"こんばんは"},
    };
    for (auto& r : rules) {
        if (!r.ok || top.find(r.kanji) == u16::npos) continue;
        u16 fixed = replace_all(top, r.kanji, r.kana);
        cands.erase(std::remove(cands.begin(), cands.end(), fixed), cands.end());
        cands.insert(cands.begin(), fixed);
        return;
    }
}

// 括弧の全種類 (開き, 閉じ)。どの括弧も必ず打てるように、変換の候補はここから出す。
// スマホ版 ime/Brackets.ets・Android 版 Brackets.kt も同じ並び (変えるときは 3 つとも)
const char16_t* const BRACKETS[][2] = {
    {u"（", u"）"}, {u"(", u")"}, {u"「", u"」"}, {u"『", u"』"}, {u"【", u"】"}, {u"［", u"］"}, {u"[", u"]"},
    {u"｛", u"｝"}, {u"{", u"}"}, {u"〔", u"〕"}, {u"〈", u"〉"}, {u"《", u"》"}, {u"〖", u"〗"}, {u"〘", u"〙"},
    {u"〚", u"〛"}, {u"｢", u"｣"}, {u"＜", u"＞"}, {u"<", u">"}, {u"«", u"»"}, {u"‹", u"›"}, {u"“", u"”"},
    {u"‘", u"’"}, {u"〝", u"〟"}, {u"｟", u"｠"},
};

// 読みに合う括弧の候補 (スマホ版 bracketVariants と同じ決まり)。括弧 1 字 → 同じ側 (開き・閉じ) の全種類 (打った字が先頭)、
// かっこ → 全種類の組、かっこひらき・かっことじ → 開き・閉じの全種類。括弧でない読みは空
std::vector<u16> bracket_variants(const u16& reading) {
    std::vector<u16> opens, closes, out;
    for (auto& b : BRACKETS) {
        opens.push_back(b[0]);
        closes.push_back(b[1]);
    }
    if (reading == u"かっこ") {
        for (auto& b : BRACKETS) out.push_back(u16(b[0]) + b[1]);
        return out;
    }
    if (reading == u"かっこひらき") return opens;
    if (reading == u"かっことじ") return closes;
    for (const std::vector<u16>* side : {&opens, &closes}) {
        if (!contains(*side, reading)) continue;
        out.push_back(reading);
        for (auto& c : *side) if (c != reading) out.push_back(c);
        break;
    }
    return out;
}

// 括弧の候補を入れる: 括弧 1 字なら同じ側の全種類を先頭から (打った字が 1 位)、かっこ などは 1 位の後ろに (すでにあれば引き上げて) 並べる
void add_brackets(const u16& reading, std::vector<u16>& cands) {
    std::vector<u16> br = bracket_variants(reading);
    if (br.empty()) return;
    if (br[0] == reading) {
        for (auto& c : cands) if (!contains(br, c)) br.push_back(c);
        cands = std::move(br);
        return;
    }
    // 辞書に下の方で入っている組 (（）「」 など) も 1 位の後ろへ引き上げる
    size_t at = std::min<size_t>(1, cands.size());
    for (auto& b : br) {
        auto it = std::find(cands.begin(), cands.end(), b);
        if (it != cands.end() && size_t(it - cands.begin()) < at) continue;
        if (it != cands.end()) cands.erase(it);
        cands.insert(cands.begin() + long(at++), b);
    }
}
}  // namespace

u16 greeting_fix(const u16& reading, const u16& surf) {
    if (reading.find(u"こんにちは") != u16::npos && reading.find(u"きょうは") == u16::npos) return replace_all(surf, u"今日は", u"こんにちは");
    return surf;
}

Composer::Composer(Converter* conv, Learning* learning, UserDict* dict) : conv_(conv), learning_(learning), dict_(dict) {}

// ---------------------------------------------------------------- キー

bool Composer::will_handle(const KeyEvent& ev) const {
    if (ev.ctrl && !ev.alt && state_ == State::Convert && ev.key == Key::Delete) return true;   // 候補を消す
    if (ev.ctrl || ev.alt) return false;
    if (state_ == State::Idle) return ev.key == Key::Char || ev.key == Key::Space || (ev.key == Key::Tab && !ev.shift && !post_.empty());
    return true;
}

bool Composer::press(const KeyEvent& ev) {
    if (!will_handle(ev)) return false;
    last_commit_ = {};   // 取り消せるのは確定の直後だけ (このキーで確定したらまた覚える)
    bool handled = false;
    switch (state_) {
        case State::Idle:
            if (ev.key == Key::Tab) {   // 文の流れの続きを入れる (お疲れ様です -> 。 -> 本日の進捗報告です。)
                const u16 t = post_[0];
                commit_text(t);
                handled = true;
            } else if (ev.key == Key::Space) {   // 何も打っていないときの空白: 全角 (Shift で半角)
                commit_text(ev.shift != options.space_fullwidth ? u"　" : u" ");
                handled = true;
            } else if (ev.key == Key::Char) {
                start_input();
                type_char(ev.ch, ev.raw);
                handled = true;
            }
            break;
        case State::Input: handled = press_input(ev); break;
        case State::Convert: handled = press_convert(ev); break;
    }
    if (ev.key != Key::F9 && ev.key != Key::F10 && ev.key != Key::Muhenkan) last_f_ = Key::Char;
    if (state_ == State::Input) update_live();
    if (state_ == State::Convert && options.always_cands && !segs_.empty()) {
        if (!segs_[focus_].loaded) load_cands(focus_);
        cand_open_ = true;
    }
    rebuild_view();
    return handled;
}

void Composer::start_input() {
    post_.clear();
    u16 c = context_ ? context_() : u16();
    ctx_ = tail(c.empty() ? history_ : c, CTX_MAX);
    state_ = State::Input;
    kana_.clear();
    caret_ = 0;
    romaji_.reset();
    raw_.clear();
    raw_valid_ = true;
    alpha_run_ = false;
    live_key_.clear();
    live_cands_.clear();
    prev_segs_.clear();
}

void Composer::insert(const u16& t) {
    kana_.insert(caret_, t);
    caret_ += t.size();
}

void Composer::type_kana(const u16& s) {
    if (s.empty()) return;
    last_commit_ = {};
    if (state_ == State::Convert) {   // 変換中に次の字: 確定してから (文脈は確定した文まで)
        u16 carry = ctx_;
        for (auto& x : segs_) carry += x.surface;
        commit_all();
        start_input();
        ctx_ = tail(carry, CTX_MAX);
    } else if (state_ == State::Idle) {
        start_input();
    }
    flush_romaji();
    raw_valid_ = false;
    static const char16_t* const DAKU_FROM = u"かきくけこさしすせそたちつてとはひふへほうカキクケコサシスセソタチツテトハヒフヘホウ";
    static const char16_t* const DAKU_TO = u"がぎぐげござじずぜぞだぢづでどばびぶべぼゔガギグゲゴザジズゼゾダヂヅデドバビブベボヴ";
    static const char16_t* const HAN_FROM = u"はひふへほハヒフヘホ";
    static const char16_t* const HAN_TO = u"ぱぴぷぺぽパピプペポ";
    static const u16 SMALL_FROM = u"あいうえおやゆよつわかけアイウエオヤユヨツワカケ";
    static const u16 SMALL_TO = u"ぁぃぅぇぉゃゅょっゎゕゖァィゥェォャュョッヮヵヶ";
    for (char16_t c : s) {
        if ((c == u'\uE000' || c == u'\uE001') && caret_ > 0) {   // 拗 (小さく) / 修 (濁音 → 半濁音 → 元、または 小さく → 元)
            char16_t& prev = kana_[caret_ - 1];
            const u16 dfrom = DAKU_FROM, dto = DAKU_TO, hfrom = HAN_FROM, hto = HAN_TO;
            size_t at;
            if (c == u'\uE000') {
                if ((at = SMALL_FROM.find(prev)) != u16::npos) prev = SMALL_TO[at];
                else if ((at = SMALL_TO.find(prev)) != u16::npos) prev = SMALL_FROM[at];
            } else if ((at = dfrom.find(prev)) != u16::npos) prev = dto[at];                 // か -> が
            else if ((at = dto.find(prev)) != u16::npos) {                                   // が -> か、ば -> ぱ
                const char16_t plain = dfrom[at];
                const size_t h = hfrom.find(plain);
                prev = h != u16::npos ? hto[h] : plain;
            } else if ((at = hto.find(prev)) != u16::npos) prev = hfrom[at];                 // ぱ -> は
            else if ((at = SMALL_FROM.find(prev)) != u16::npos) prev = SMALL_TO[at];          // や -> ゃ
            else if ((at = SMALL_TO.find(prev)) != u16::npos) prev = SMALL_FROM[at];
            continue;
        }
        if ((c == u'゛' || c == u'゜') && caret_ > 0) {
            const char16_t prev = kana_[caret_ - 1];
            const u16 from = c == u'゛' ? DAKU_FROM : HAN_FROM, to = c == u'゛' ? DAKU_TO : HAN_TO;
            const size_t at = from.find(prev);
            if (at != u16::npos) {
                kana_[caret_ - 1] = to[at];
                continue;
            }
        }
        insert(u16(1, c));
    }
    update_live();
    rebuild_view();
}

void Composer::flush_romaji() {
    if (!romaji_.pending().empty()) insert(romaji_.flush());
}

void Composer::type_char(char16_t c, bool raw) {
    if (raw_valid_ && caret_ == kana_.size()) raw_.push_back(c);
    else raw_valid_ = false;
    if (raw) {   // テンキー: 詳細設定で半角・数字の設定に合わせる・全角
        flush_romaji();
        const bool full = options.numpad == 2 || (options.numpad == 1 && options.digits_fullwidth);
        insert(u16(1, full && c > 0x20 && c < 0x7F ? char16_t(c + 0xFEE0) : c));
        return;
    }
    char16_t prev = caret_ > 0 ? kana_[caret_ - 1] : 0;
    if (is_upper(c)) {                                     // 大文字: ここから英字のまま
        flush_romaji();
        alpha_run_ = true;
        insert(u16(1, c));
        return;
    }
    if (alpha_run_ && c != u' ' && c < 0x7F) {            // 英字の続き (記号も打ったまま)
        flush_romaji();
        insert(u16(1, c));
        return;
    }
    if (romaji_.pending().empty() && is_digit(prev) && (c == u'-' || c == u',' || c == u'.' || c == u':' || c == u'/')) {
        insert(u16(1, c));                                  // 数の続きの記号 (03-1234・1,000・3.14・12:30・9/27)
        return;
    }
    // - を半角にしていれば、ローマ字の長音 (ー) にせずそのまま (詳細設定の記号の全角/半角)
    if (c == u'-' && romaji_.pending().empty() && options.half_symbols.find(u'-') != u16::npos) {
        insert(u"-");
        return;
    }
    if (is_lower(c) || romaji_.accepts(c) || c == u'-') {
        // 打ち間違いを消して、ローマ字にならなかった英字 (子音) だけが残っているとき (「ps」→ s を消す → 「p」):
        // その英字を溜めている分に戻して、続けて打ったキーとつなぐ (p + a → ぱ)。つながる綴りになるときだけ
        if (romaji_.pending().empty() && is_lower(c)) {
            size_t n = 0;
            while (n < 3 && caret_ > n && is_lower(kana_[caret_ - 1 - n])) n++;
            for (; n > 0; n--) {
                u16 back = kana_.substr(caret_ - n, n);
                bool sokuon = n == 1 && back[0] == c && c != u'n' && u16(u"aiueo").find(c) == u16::npos;   // t + t → っt
                if (!romaji_.is_prefix(back + c) && !sokuon) continue;
                kana_.erase(caret_ - n, n);
                caret_ -= n;
                romaji_.unread(back);
                break;
            }
        }
        Romaji::Result r = romaji_.process(c);
        insert(r.committed);
        return;
    }
    flush_romaji();
    if ((c == 0x309B || c == 0x309C) && caret_ > 0) {   // かな入力の濁点・半濁点: 前のかなにつける (か + ゛ → が)
        char16_t p = kana_[caret_ - 1];
        const char16_t* voiced = u"かきくけこさしすせそたちつてとはひふへほ";
        const char16_t* semi = u"はひふへほ";
        if (c == 0x309B && u16(voiced).find(p) != u16::npos) { kana_[caret_ - 1] = char16_t(p + 1); return; }
        if (c == 0x309B && p == u'う') { kana_[caret_ - 1] = u'ゔ'; return; }
        if (c == 0x309C && u16(semi).find(p) != u16::npos) { kana_[caret_ - 1] = char16_t(p + 2); return; }
    }
    if (c > 0x20 && c < 0x7F && !is_digit(c) && options.half_symbols.find(c) != u16::npos) insert(u16(1, c));   // 半角にした記号
    else if (is_digit(c)) insert(u16(1, options.digits_fullwidth ? char16_t(c + 0xFEE0) : c));
    else if (c == u',') insert(options.punct == 1 || options.punct == 3 ? u"，" : u"、");
    else if (c == u'.') insert(options.punct == 1 || options.punct == 2 ? u"．" : u"。");
    else insert(u16(1, japanese_symbol(c)));
}

bool Composer::press_input(const KeyEvent& ev) {
    // 提案の列: Shift+↓ / Shift+Tab で入り、←→ / Tab / Shift+Tab で選び、Enter・空白で確定、Esc・↑ で戻る。ほかのキーは列を出てふつうに
    if (sugg_focus_ >= 0) {
        const int n = int(live_sugg_.size());
        switch (ev.key) {
            case Key::Right: case Key::Down:
                sugg_focus_ = (sugg_focus_ + 1) % std::max(1, n);
                return true;
            case Key::Left:
                sugg_focus_ = (sugg_focus_ + n - 1) % std::max(1, n);
                return true;
            case Key::Tab:
                sugg_focus_ = ev.shift ? (sugg_focus_ + n - 1) % std::max(1, n) : (sugg_focus_ + 1) % std::max(1, n);
                return true;
            case Key::Enter: case Key::Space:
                select_suggestion(sugg_focus_);
                return true;
            case Key::Escape: case Key::Up:
                sugg_focus_ = -1;
                return true;
            default:
                sugg_focus_ = -1;
                break;
        }
    }
    switch (ev.key) {
        case Key::Char:
            type_char(ev.ch, ev.raw);
            return true;
        case Key::Space:
        case Key::Henkan:
            // 打ち間違いの直しがあるときは、文ごとの候補 (直しを 1 位に上げた・交互に混ぜた) のまま変換に入る。
            // 文節に分けると直しが候補に入らなかった (ユーザー 2026-10-14「win 版も訂正を普通の変換内に」)
            flush_romaji();
            update_live();
            if ((!typo_top_.empty() || !sugg_typo_.empty()) && !live_cands_.empty()) {
                enter_single(live_cands_[0], live_cands_, 0);
                first_cands_ = live_cands_;
                first_via_ = "space";
                return true;
            }
            convert_all();
            return true;
        case Key::Down:
        case Key::Tab:   // 打っている間の候補を選び始める。予測 (読みの続く語) があればそこから (MS-IME と同じ)
            flush_romaji();
            update_live();
            if (!live_cands_.empty()) {
                int sel = 0;
                pred_tab_.clear();
                if (ev.key == Key::Tab)
                    for (size_t i = 0; i < live_cands_.size(); i++)
                        if (contains(live_pred_, live_cands_[i])) pred_tab_.push_back(int(i));
                if (!pred_tab_.empty()) sel = pred_tab_[0];
                enter_single(live_cands_[sel], live_cands_, sel);
                first_cands_ = live_cands_;
                first_via_ = "live";
            }
            return true;
        case Key::Enter:
            flush_romaji();
            if (ev.shift && !live_sugg_.empty()) {   // Shift+Enter: 提案の 1 つ目で確定
                update_live();
                if (!live_sugg_.empty()) { select_suggestion(0); return true; }
            }
            if (live_shown()) {   // ライブ変換: 見えている変換をそのまま確定する
                update_live();
                u16 t = live_top_;
                log({"commit", kana_, t, ctx_, live_cands_, 0, {}, "live"});
                commit_text(frozen_surface() + t);
                to_idle();
                return true;
            }
            log({"raw", kana_, kana_, ctx_});
            commit_text(frozen_surface() + kana_);   // 固定した所は変換した形のまま
            to_idle();
            return true;
        case Key::Escape:
            log({"clear", frozen_reading() + kana_ + romaji_.pending(), u16(), ctx_});
            to_idle();
            return true;
        case Key::Backspace:
            if (romaji_.pending().empty() && caret_ == 0) unfreeze_last();   // 固定した所を消す: 最後の文節を読みに戻す
            if (!romaji_.pending().empty()) {
                romaji_.backspace();
                if (raw_valid_ && !raw_.empty()) raw_.pop_back();
            } else if (caret_ > 0) {
                kana_.erase(caret_ - 1, 1);
                caret_--;
                raw_valid_ = false;
            }
            if (kana_.empty() && romaji_.pending().empty() && frozen_.empty()) to_idle();
            return true;
        case Key::Delete:
            flush_romaji();
            if (caret_ < kana_.size()) { kana_.erase(caret_, 1); raw_valid_ = false; }
            if (kana_.empty() && frozen_.empty()) to_idle();
            return true;
        case Key::Left:
            flush_romaji();
            if (caret_ == 0) unfreeze_last();
            if (caret_ > 0) caret_--;
            return true;
        case Key::Right:
            flush_romaji();
            if (caret_ < kana_.size()) caret_++;
            return true;
        case Key::Home:
            flush_romaji();
            while (unfreeze_last()) {}
            caret_ = 0;
            return true;
        case Key::End:
            flush_romaji();
            caret_ = kana_.size();
            return true;
        case Key::F6: case Key::F7: case Key::F8: case Key::F9: case Key::F10: case Key::Muhenkan:
            flush_romaji();
            if (kana_.empty() && frozen_.empty()) { to_idle(); return true; }
            if (kana_.empty()) unfreeze_last();
            enter_single(kana_, {}, 0);
            first_via_ = "fkey";
            transform(ev.key);
            return true;
        default:   // ↑・PageUp/Down: 入力中は入力欄に渡さない
            return true;
    }
}

// 候補の木の二股: 文節 i (区間の頭) から区間の終わりまでの文節を 1 つにまとめる (別の区切りの候補を選んだ)
void Composer::fork_merge(size_t i) {
    Seg& s = segs_[i];
    int st = -tree_off_;
    for (size_t k = 0; k < i; k++) st += int(segs_[k].reading.size());
    for (auto& n : tree_) {
        if (n.fork.empty() || n.main.empty() || n.main.front().rs != st) continue;
        const int b = n.main.back().re;
        size_t j = i + 1;
        int e = st + int(s.reading.size());
        while (j < segs_.size() && e < b) e += int(segs_[j++].reading.size());
        if (e != b) return;   // 区間の終わりが文節の切れ目でない (文節を伸び縮みさせた後など)
        s.own_reading = s.reading;
        s.own_tail_r = s.tail_r;
        s.own_tail_s = s.tail_s;
        s.merged.assign(segs_.begin() + long(i + 1), segs_.begin() + long(j));
        for (auto& m : s.merged) s.reading += m.reading;
        s.tail_r.clear();
        s.tail_s.clear();
        segs_.erase(segs_.begin() + long(i + 1), segs_.begin() + long(j));
        whole_.clear();
        return;
    }
}

void Composer::fork_unmerge(size_t i) {
    Seg& s = segs_[i];
    if (s.merged.empty() && s.own_reading.empty()) return;
    s.reading = s.own_reading;
    s.tail_r = s.own_tail_r;
    s.tail_s = s.own_tail_s;
    s.own_reading.clear();
    std::vector<Seg> rest = std::move(s.merged);
    s.merged.clear();
    segs_.insert(segs_.begin() + long(i + 1), rest.begin(), rest.end());
}

bool Composer::press_convert(const KeyEvent& ev) {
    Seg& s = segs_[focus_];
    auto set_sel = [&](int i) {
        if (!s.loaded) load_cands(focus_);
        if (s.cands.empty()) return;
        // 最後のページに入るなら、先に続きを読む (候補が途中で切れないように)
        if (i >= 0 && i / PAGE >= (int(s.cands.size()) - 1) / PAGE) load_more(focus_);
        int n = int(s.cands.size());
        const int old = s.sel;
        s.sel = ((i % n) + n) % n;
        s.surface = s.cands[size_t(s.sel)];
        s.changed = true;
        cand_open_ = true;
        // 別の区切りの候補に入った・出た: 区間の文節をまとめる・戻す (s は前の文節なので、後ろを消したり足したりしても有効)
        if (s.fork_idx >= 0 && old == s.fork_idx && s.sel != s.fork_idx) fork_unmerge(focus_);
        if (s.fork_idx >= 0 && s.sel == s.fork_idx && old != s.fork_idx) fork_merge(focus_);
    };
    switch (ev.key) {
        case Key::Char: {
            if (cand_open_ && ev.ch >= u'1' && ev.ch <= u'9') {   // 候補の窓の番号で選ぶ
                int idx = (s.sel / PAGE) * PAGE + (ev.ch - u'1');
                if (idx < int(s.cands.size())) {
                    set_sel(idx);
                    if (!options.always_cands) cand_open_ = false;
                }
                return true;
            }
            // 変換中に次の字を打った: 確定してから打ち始める (文脈は確定した文まで)
            u16 carry = ctx_;
            for (auto& x : segs_) carry += x.surface;
            commit_all();
            start_input();
            ctx_ = tail(carry, CTX_MAX);
            type_char(ev.ch, ev.raw);
            return true;
        }
        case Key::Tab:
            // Tab で予測の候補に入ったときは、Tab・Shift+Tab で予測の候補の中だけを回る (普通の変換候補に移っていた。ブログの報告 2026-10-12)
            if (!pred_tab_.empty()) {
                auto it = std::find(pred_tab_.begin(), pred_tab_.end(), s.sel);
                const int m = int(pred_tab_.size());
                const int k = it == pred_tab_.end() ? 0 : int(it - pred_tab_.begin());
                set_sel(pred_tab_[size_t(((ev.shift ? k - 1 : k + 1) % m + m) % m)]);
                return true;
            }
            set_sel(ev.shift ? s.sel - 1 : s.sel + 1);
            return true;
        case Key::Space:
        case Key::Henkan:
        case Key::Down:
            pred_tab_.clear();   // ほかのキーで候補を動かしたら、普通の並びで
            set_sel(ev.shift && ev.key != Key::Down ? s.sel - 1 : s.sel + 1);
            return true;
        case Key::Up:
            set_sel(s.sel - 1);
            return true;
        case Key::PageDown:
            if (cand_open_) set_sel(std::min(int(s.cands.size()) - 1, (s.sel / PAGE + 1) * PAGE));
            return true;
        case Key::PageUp:
            if (cand_open_) set_sel(std::max(0, (s.sel / PAGE - 1) * PAGE));
            return true;
        case Key::Enter:
            commit_all();
            return true;
        case Key::Escape:
            if (cand_open_ && !options.always_cands) cand_open_ = false;
            else back_to_input();
            return true;
        case Key::Backspace:
            back_to_input();
            return true;
        case Key::Delete:   // Ctrl+Delete: 選んでいる候補を消す (MS-IME と同じ。読みのまま・カタカナは消さない)
            if (ev.ctrl && learning_) {
                if (!s.loaded) load_cands(focus_);
                if (s.cands.empty()) return true;
                const u16 x = s.cands[size_t(std::max(0, std::min(s.sel, int(s.cands.size()) - 1)))];
                if (x == s.reading || x == to_katakana(s.reading)) return true;
                learning_->hide(s.reading, x);
                s.cands.erase(std::remove(s.cands.begin(), s.cands.end(), x), s.cands.end());
                cand_open_ = true;
                set_sel(std::min(s.sel, int(s.cands.size()) - 1));
            }
            return true;
        case Key::Left:
        case Key::Right:
            if (ev.shift) { resize_focus(ev.key == Key::Left ? -1 : 1); return true; }
            cand_open_ = false;
            if (ev.key == Key::Left && focus_ > 0) focus_--;
            if (ev.key == Key::Right && focus_ + 1 < segs_.size()) focus_++;
            return true;
        case Key::Home:
            cand_open_ = false;
            focus_ = 0;
            return true;
        case Key::End:
            cand_open_ = false;
            focus_ = segs_.size() - 1;
            return true;
        case Key::F6: case Key::F7: case Key::F8: case Key::F9: case Key::F10: case Key::Muhenkan:
            transform(ev.key);
            return true;
        default:
            return true;
    }
}

u16 Composer::sugg_note(const u16& x) const {
    // 候補の右に薄い字で添える種類 (Windows・Linux。スマホの青枠の代わり。2026-10-14)
    if ((!typo_top_.empty() && x == typo_top_) || contains(sugg_typo_, x)) return u"もしかして";
    if (contains(date_cands_, x)) return u"日付";
    if (!contains(sugg_set_, x)) return u16();
    int k = 0;
    for (size_t i = 0; i < live_sugg_.size(); i++) if (live_sugg_[i] == x) { k = live_sugg_kinds_[i]; break; }
    if (k == 2) return u"日付";
    if (k == 3) return u"住所";
    if (k == 4) return u"英語";
    bool emoji = false, letters = false;
    for (char16_t c : x) {
        emoji = emoji || (c >= 0xD800 && c <= 0xDBFF) || (c >= 0x2600 && c <= 0x27BF);
        letters = letters || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'ω' || c == u'∀' || c == u'´' || c == u'｀';
    }
    if (emoji) return u"絵文字";
    if (letters || x.size() >= 3) return u"顔文字";
    return u"記号";
}

void Composer::add_datetime(const u16& reading, std::vector<u16>& list) {
    std::vector<u16> ds = datetime_predict(reading, std::time(nullptr));
    for (auto& d : ds) {
        if (contains(list, d)) continue;
        list.push_back(d);
        date_cands_.push_back(d);
        sugg_set_.push_back(d);
    }
}

void Composer::select_suggestion(int index) {
    if (state_ != State::Input || index < 0 || index >= int(live_sugg_.size())) return;
    flush_romaji();
    // 気分の絵文字・顔文字 (6) は絵文字だけが来る: 1 位の後ろに足す
    u16 t = frozen_surface() + (live_sugg_kinds_[size_t(index)] == 6 ? live_top_ : u16()) + live_sugg_[size_t(index)];
    {
        LogEvent e{"commit", frozen_reading() + kana_, t, ctx_, live_sugg_, index};
        e.extra = "suggest";
        log(e);
    }
    if (flow && !options.private_mode) flow->record_suggest(live_sugg_[size_t(index)]);
    commit_text(t);
    to_idle();
    rebuild_view();
}

void Composer::select_candidate(int index) {
    if (state_ == State::Input) {   // 打っている間の候補: それを確定する
        if (index < 0 || index >= int(live_cands_.size())) return;
        u16 c = live_cands_[size_t(index)];
        flush_romaji();
        {
            LogEvent e{"commit", kana_, c, ctx_, live_cands_, index};
            e.segs = {c};
            e.extra = "live,mouse";
            log(e);
        }
        if (learning_ && !options.private_mode && !contains(sugg_set_, c)) learning_->record(kana_, c);
        if (flow && !options.private_mode && contains(sugg_set_, c)) flow->record_suggest(c);
        commit_text(frozen_surface() + c);
        to_idle();
        rebuild_view();
        return;
    }
    if (state_ != State::Convert) return;
    Seg& s = segs_[focus_];
    if (index < 0 || index >= int(s.cands.size())) return;
    s.sel = index;
    s.surface = s.cands[size_t(index)];
    s.changed = true;
    log({"bseg", s.reading, s.surface, left_context(focus_), s.cands, index, {}, "mouse"});
    if (!options.always_cands) cand_open_ = false;
    rebuild_view();
}

// ---------------------------------------------------------------- 打っている間の変換

// 語の並びの読みをつないだもの
static u16 frozen_prefix_reading(const std::vector<Word>& ws) {
    u16 r;
    for (const Word& w : ws) r += w.reading;
    return r;
}

void Composer::update_live() {
    // 打っている間の変換は、候補の窓 (live) かリアルタイム変換 (live_display) のどちらかで要る。窓とは別に選べる (ユーザー 2026-10-14)
    if (!(options.live || options.live_display) || state_ != State::Input || !conv_ || kana_.empty()) {
        live_cands_.clear();
        live_key_.clear();
        return;
    }
    u16 key = conv_ctx() + u'\x01' + kana_;
    if (key == live_key_) return;
    sync_user_dict();
    if (!calibrated_) calibrate();
    Conversion c;
    // 前の方の固定 (live_commit) をオフにしていると、長く打つほど 1 打ごとに読み全体を変換し直して打鍵が引っかかった
    // (132 字で 1 打 0.1 秒超・ブログの方の設定)。表示用には、前の結果の語の区切りのうち末尾から keep_ 字より前の分を使い回し、
    // 後ろだけを変換し直す (そこまでの表記を文脈にして)。スペースで変換するときは全体を変換するので、結果は変わらない
    size_t pre_r = 0;
    u16 pre_s;
    std::vector<Word> pre_w;
    if (!options.live_commit && caret_ == kana_.size() && kana_.size() > keep_ * 2) {
        for (const Word& w : live_words_) {
            if (pre_r + w.reading.size() + keep_ > kana_.size()) break;
            pre_r += w.reading.size();
            pre_s += w.surface;
            pre_w.push_back(w);
        }
        if (pre_r == 0 || kana_.compare(0, pre_r, frozen_prefix_reading(pre_w)) != 0) pre_r = 0;
    }
    if (pre_r > 0) {
        Conversion t = conv_->convert(conv_ctx() + pre_s, kana_.substr(pre_r), kCandidates);
        for (auto& x : t.cands) c.cands.push_back(pre_s + x);
        c.words = pre_w;
        c.words.insert(c.words.end(), t.words.begin(), t.words.end());
        c.sugg = t.sugg;
        c.sugg_kinds = t.sugg_kinds;
    } else {
        c = conv_->convert(conv_ctx(), kana_, kCandidates);
    }
    live_words_ = c.words;
    if (options.live_commit && caret_ == kana_.size() && !alpha_run_ && live_commit(c)) {
        update_live();   // 前の方を固定した: 残りを変換し直す
        return;
    }
    live_key_ = key;
    sugg_focus_ = -1;
    live_sugg_.clear();
    live_sugg_kinds_.clear();
    typo_top_.clear();
    after_emoji_.clear();
    for (size_t i = 0; i < c.sugg.size(); i++) {
        if (i < c.sugg_kinds.size() && c.sugg_kinds[i] == 8) { typo_top_ = c.sugg[i]; continue; }   // 1 位にした打ち間違いの直し
        if (i < c.sugg_kinds.size() && c.sugg_kinds[i] == 9) { after_emoji_.push_back(c.sugg[i]); continue; }   // 確定の後に出す絵文字
        if (!c.cands.empty() && c.sugg[i] == c.cands[0]) continue;
        live_sugg_.push_back(c.sugg[i]);
        live_sugg_kinds_.push_back(i < c.sugg_kinds.size() ? c.sugg_kinds[i] : 0);
    }
    // 選んだことのある提案を、同じ種類の中で前に (スマホ版と同じ。2026-10-14)
    if (flow && live_sugg_.size() > 1) {
        std::vector<size_t> ord(live_sugg_.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        auto first_of = [&](int k) { return size_t(std::find(live_sugg_kinds_.begin(), live_sugg_kinds_.end(), k) - live_sugg_kinds_.begin()); };
        std::stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) {
            const size_t gx = first_of(live_sugg_kinds_[x]), gy = first_of(live_sugg_kinds_[y]);
            if (gx != gy) return gx < gy;
            return flow->suggest_use(live_sugg_[x]) > flow->suggest_use(live_sugg_[y]);
        });
        std::vector<u16> s2;
        std::vector<int> k2;
        for (size_t i : ord) { s2.push_back(live_sugg_[i]); k2.push_back(live_sugg_kinds_[i]); }
        live_sugg_.swap(s2);
        live_sugg_kinds_.swap(k2);
    }
    std::vector<u16> cands = c.cands;
    add_brackets(kana_, cands);   // 学習の並べ替えより前に (よく選ぶ括弧が上に来るように)
    if (learning_) cands = learning_->apply_order(kana_, cands);
    prefer_greetings(kana_, cands);
    live_top_ = cands.empty() ? kana_ : cands[0];
    // 予測 (スマホ版と同じ置き方): 決まり文句は先頭に、読みの続く語 (学習した語・辞書の語) は 1 位の直後に 3 つまで
    const Phrase2* phrase = nullptr;
    for (auto& p : PREDICTIVE_PHRASES) {
        u16 r = p.reading;
        if (kana_.size() >= size_t(p.min_prefix) && kana_.size() < r.size() && r.compare(0, kana_.size(), kana_) == 0) { phrase = &p; break; }
    }
    live_pred_.clear();
    if (kana_.size() >= 2) {
        std::vector<u16> comp;
        if (learning_) for (auto& x : learning_->completions(kana_, 2)) push_unique(comp, x);
        for (auto& x : conv_->complete(kana_, 4)) push_unique(comp, x);
        size_t at = std::min<size_t>(1, cands.size());
        int inserted = 0;
        for (auto& x : comp) {
            if (inserted >= 3) break;
            if (contains(cands, x) || (phrase && x == phrase->text)) continue;
            cands.insert(cands.begin() + long(at++), x);
            live_pred_.push_back(x);
            inserted++;
        }
    }
    if (phrase) {
        u16 t = phrase->text;
        cands.erase(std::remove(cands.begin(), cands.end(), t), cands.end());
        cands.insert(cands.begin(), t);
    }
    if (dict_) {   // ユーザー辞書に登録した語は先頭に
        std::vector<u16> user = dict_->lookup(kana_);
        // 登録した語どうしは選んだ回数の多い順 (前は登録順のままで、学習が効かなかった。ブログの報告 2026-10-18)
        if (learning_ && user.size() > 1) user = learning_->apply_order(kana_, user);
        for (auto it = user.rbegin(); it != user.rend(); ++it) {
            cands.erase(std::remove(cands.begin(), cands.end(), *it), cands.end());
            cands.insert(cands.begin(), *it);
        }
    }
    if (cands.empty()) cands.push_back(kana_);
    // 提案は別の行をやめて候補に混ぜる (種類は View::cand_notes で右に添える) (スマホ版と同じ。ユーザー 2026-10-14)。選んでも学習はしない
    // 候補と交互に 1 つずつ (2 位の後ろから): ひと固めにすると惜しい 2 位以降が遠くなった (ユーザー 2026-10-14)
    sugg_set_.clear();
    sugg_typo_.clear();
    date_cands_.clear();
    size_t at = 2;
    for (size_t si = 0; si < live_sugg_.size(); si++) {
        const u16& s = live_sugg_[si];
        if (contains(cands, s)) continue;
        if (live_sugg_kinds_[si] == 1) sugg_typo_.push_back(s);
        if (at > cands.size()) at = cands.size();
        cands.insert(cands.begin() + long(at), s);
        at += 2;
        sugg_set_.push_back(s);
    }
    add_datetime(kana_, cands);   // 日付・時刻の候補は最後の方に (きょう→今日 は 1 位のまま)
    live_cands_ = std::move(cands);
}

// 長い入力の前の方を固定する (スマホ版 AiConverter.maybeFreeze と同じ決まり、既定の保留と同じ見せ方)。固定したら true。
// 以前は入力欄に確定していた (次々確定) が、文節で直せなくなるのでやめた (2026-10-09 ユーザー)
bool Composer::live_commit(const Conversion& c) {
    if (c.cands.empty() || c.words.size() < 2) {
        prev_segs_.clear();
        return false;
    }
    const u16& top = c.cands[0];
    size_t cum_r = 0, cum_s = 0, freeze_end = 0, freeze_len = 0;
    std::map<size_t, u16> segs;
    for (size_t i = 0; i + 1 < c.words.size(); i++) {   // 最後の語は確定しない
        cum_r += c.words[i].reading.size();
        cum_s += c.words[i].surface.size();
        u16 surf = top.substr(0, std::min(cum_s, top.size()));
        segs[cum_r] = surf;
        bool punct = u16(u"、。！？!?").find(kana_[cum_r - 1]) != u16::npos;
        auto it = prev_segs_.find(cum_r);
        bool stable = it != prev_segs_.end() && it->second == surf;
        if (cum_r + keep_ <= kana_.size() && (stable || punct)) {
            freeze_end = cum_r;
            freeze_len = cum_s;
        }
    }
    prev_segs_ = std::move(segs);
    if (!freeze_end) return false;
    // 固定する語を文節にまとめて頭の文節にする (挨拶のかな書きも当てる: ここで「今日は」が固まらないように)
    std::vector<Word> fw;
    size_t rr = 0;
    for (auto& w : c.words) {
        if (rr >= freeze_end) break;
        fw.push_back(w);
        rr += w.reading.size();
    }
    u16 fs;
    for (auto& p : group_phrases(fw)) {
        Seg s{p.reading, greeting_fix(p.reading, p.surface)};
        if (p.head_r < p.reading.size() && p.head_s < p.surface.size()) {
            s.tail_r = p.reading.substr(p.head_r);
            s.tail_s = p.surface.substr(p.head_s);
        }
        fs += s.surface;
        frozen_.push_back(std::move(s));
    }
    {
        std::vector<u16> cs(c.cands.begin(), c.cands.begin() + long(std::min<size_t>(c.cands.size(), 5)));
        LogEvent e{"auto", kana_.substr(0, freeze_end), fs, ctx_, cs, -1};
        e.extra = "hold," + to_utf8(kana_.substr(freeze_end));   // 固定 (入力欄には確定しない)、まだ固定していない残りの読み
        log(e);
    }
    kana_.erase(0, freeze_end);
    caret_ -= std::min(caret_, freeze_end);
    raw_valid_ = false;
    prev_segs_.clear();
    live_key_.clear();
    return true;
}

// 変換の速さを測って、固定せずに AI 変換に渡す字数を決める (最初に打ったときに 1 回。決まった読みを 3 回変換して速い方の時間)。
// 1 回の変換を BUDGET_MS に収める長さ (時間は読みの長さにほぼ比例) を KEEP〜KEEP_MAX で。遅い PC はスマホ版と同じ 8 字、
// 速い PC は 32 字まで渡して、長い文脈で変換させる
void Composer::calibrate() {
    calibrated_ = true;
    if (!conv_) return;
    static const char16_t* R = u"きょうはとてもいいてんきなのでこうえんまでさんぽにいってみようとおもいます";   // 36 字
    constexpr double BUDGET_MS = 40;
    const u16 r(R);
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        auto t0 = std::chrono::steady_clock::now();
        conv_->convert(u16(), r, kCandidates);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (i > 0) best = std::min(best, ms);   // 1 回目は読み込み・温まりを含むので数えない
    }
    size_t n = best > 0 ? size_t(double(r.size()) * BUDGET_MS / best) : KEEP_MAX;
    keep_ = std::clamp(n, KEEP, KEEP_MAX);
    LogEvent e{"diag", u16(), u16(), u16()};
    e.extra = "keep=" + std::to_string(keep_) + ",ms36=" + std::to_string(int(best * 10) / 10.0);
    log(e);
}

u16 Composer::frozen_reading() const {
    u16 r;
    for (auto& s : frozen_) r += s.reading;
    return r;
}

u16 Composer::frozen_surface() const {
    u16 r;
    for (auto& s : frozen_) r += s.surface;
    return r;
}

// 残りの読みを変換するときの文脈 (入力欄の左 + 固定した表記)
u16 Composer::conv_ctx() const { return tail(ctx_ + frozen_surface(), CTX_MAX); }

// 固定した最後の文節を読みに戻して、入力中の頭につなぐ (固定した所を Backspace・← で直すとき)。戻したら true
bool Composer::unfreeze_last() {
    if (frozen_.empty()) return false;
    u16 r = frozen_.back().reading;
    frozen_.pop_back();
    kana_.insert(0, r);
    caret_ += r.size();
    raw_valid_ = false;
    live_key_.clear();
    prev_segs_.clear();
    return true;
}

// ---------------------------------------------------------------- 変換

void Composer::sync_user_dict() {
    if (!dict_ || !conv_) return;
    dict_->refresh();
    if (dict_->version() != dict_version_) {
        conv_->set_user_words(dict_->engine_forms());
        dict_version_ = dict_->version();
        live_key_.clear();
    }
}

std::vector<Composer::Seg> Composer::convert_phrases(const u16& ctx, const u16& reading, std::vector<u16>* whole) {
    std::vector<Seg> out;
    Conversion c = conv_ ? conv_->convert(ctx, reading, kCandidates) : Conversion();
    last_tree_ = std::move(c.tree);
    if (whole) {
        *whole = c.cands;
        prefer_greetings(reading, *whole);
    }
    std::vector<Word> words = c.words;
    if (words.empty()) words.push_back({reading, c.cands.empty() ? reading : c.cands[0]});
    for (auto& p : group_phrases(words)) {
        Seg s{p.reading, greeting_fix(p.reading, p.surface)};
        if (p.head_r < p.reading.size() && p.head_s < p.surface.size()) {
            s.tail_r = p.reading.substr(p.head_r);
            s.tail_s = p.surface.substr(p.head_s);
        }
        // 自分で選び直したことのある文節の読みは、そのとき選んだ表記にする
        if (learning_) {
            u16 t = learning_->top(p.reading);
            if (!t.empty()) s.surface = t;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void Composer::convert_all() {
    flush_romaji();
    if (kana_.empty()) { to_idle(); return; }
    sync_user_dict();
    segs_ = frozen_;
    std::vector<Seg> rest = convert_phrases(conv_ctx(), kana_, &whole_);
    tree_ = std::move(last_tree_);
    tree_off_ = 0;
    for (auto& s : segs_) tree_off_ += int(s.reading.size());
    // 文節に分ける変換 (PC の IME の伝統) はそのままに、打っている間の一覧 (Tab・スマホと同じ) の工夫を受け入れる
    // (ユーザー 2026-10-16「space は伝統的な挙動で、そのうえで色んな機能を」): 文全体の候補はその一覧 (学習の並べ替え・
    // ユーザー辞書・挨拶・括弧・提案) から予測 (読みより長い語) を除いたもの。文全体で選び直したことがあるなどで一覧の 1 位が
    // AI の 1 位と違うときは、その文を 1 つの文節で出す (文節に分けた形だとその 1 位を作れない)
    if (!live_cands_.empty() && live_key_ == conv_ctx() + u'\x01' + kana_) {
        std::vector<u16> live;
        for (auto& x : live_cands_)
            if (!contains(live_pred_, x)) live.push_back(x);
        u16 joined;
        for (auto& s : rest) joined += s.surface;
        if (!live.empty() && !whole_.empty() && live[0] != whole_[0] && live[0] != joined) {
            Seg one{kana_, live[0]};
            rest.assign(1, std::move(one));
            tree_.clear();   // 木は AI の 1 位の形なので合わない
        }
        if (!live.empty()) whole_ = std::move(live);
    }
    size_t first_new = segs_.size();
    segs_.insert(segs_.end(), rest.begin(), rest.end());
    frozen_.clear();
    if (learning_)
        for (size_t i = first_new; i < segs_.size(); i++)
            if (learning_->hidden(segs_[i].reading, segs_[i].surface)) {
                segs_[i].surface.clear();   // load_cands が今の表記を先頭に入れないように
                load_cands(i);
                if (!segs_[i].cands.empty() && segs_[i].cands[0].empty()) segs_[i].cands.erase(segs_[i].cands.begin());
                segs_[i].surface = segs_[i].cands.empty() ? segs_[i].reading : segs_[i].cands[0];
                segs_[i].sel = 0;
            }
    first_cands_ = whole_;
    if (!segs_.empty() && segs_.size() > rest.size()) {   // 記録用の文全体の候補にも固定した表記をつなぐ
        u16 fs;
        for (size_t i = 0; i + rest.size() < segs_.size(); i++) fs += segs_[i].surface;
        for (auto& x : first_cands_) x = fs + x;
    }
    first_via_ = "space";
    if (segs_.size() != 1) whole_.clear();
    focus_ = 0;
    cand_open_ = false;
    state_ = State::Convert;
}

void Composer::enter_single(const u16& surface, std::vector<u16> cands, int sel) {
    Seg s{kana_, surface};
    if (!cands.empty()) {
        push_unique(cands, kana_);
        push_unique(cands, to_katakana(kana_));
        s.cands = std::move(cands);
        s.loaded = true;
        s.sel = sel;
        cand_open_ = true;
    } else {
        cand_open_ = false;
    }
    bool had_frozen = !frozen_.empty();
    segs_ = frozen_;
    frozen_.clear();
    segs_.push_back(std::move(s));
    tree_.clear();   // 文 1 つの変換: 木は文節の札をそのまま並べる (rebuild_view)
    if (had_frozen) whole_.clear();
    else whole_.clear();
    focus_ = segs_.size() - 1;
    state_ = State::Convert;
}

u16 Composer::left_context(size_t seg) const {
    u16 c = ctx_;
    for (size_t i = 0; i < seg && i < segs_.size(); i++) c += segs_[i].surface;
    return tail(c, CTX_MAX);
}

void Composer::load_cands(size_t i) {
    Seg& s = segs_[i];
    std::vector<u16> list;
    if (dict_) for (auto& w : dict_->lookup(s.reading)) push_unique(list, w);
    if (segs_.size() == 1 && !whole_.empty()) {
        for (auto& c : whole_) push_unique(list, c);
    } else if (conv_) {
        // 自立語だけを変換して付属語をつなぐ (がくせい|です → 学生です・楽聖です・学制です)。
        // 付属語が無い文節 (または分けられない文節) は文節ごと変換する
        u16 head = s.reading.substr(0, s.reading.size() - s.tail_r.size());
        for (auto& c : conv_->convert(left_context(i), head, kCandidates).cands) push_unique(list, c + s.tail_s);
        if (!s.tail_r.empty()) push_unique(list, head + s.tail_s);
    }
    add_brackets(s.reading, list);   // 学習の並べ替えより前に (よく選ぶ括弧が上に来るように)
    if (learning_) list = learning_->apply_order(s.reading, list);
    // いまの表記を先頭に (選ぶ前の並びがいちばん上から始まるように)
    auto it = std::find(list.begin(), list.end(), s.surface);
    if (it != list.end()) list.erase(it);
    list.insert(list.begin(), s.surface);
    push_unique(list, s.reading);
    push_unique(list, to_katakana(s.reading));
    // 候補の木: 木に出ている表記 (1 位と強い別の候補) と、二股の頭なら別の区切りの文を、強い順に先頭に寄せる。
    // 窓は最初はこれだけを出し、その下で ↓ を押すと残りの候補を出す (tree_k = 先頭の木の候補の数。0 = 木に無い文節)
    s.tree_k = 0;
    s.fork_idx = -1;
    std::vector<float> scores;
    if (options.cand_tree && !tree_.empty()) {
        int r = -tree_off_;
        for (size_t k = 0; k < i; k++) r += int(segs_[k].reading.size());
        const int re = r + int(s.reading.size());
        for (auto& n : tree_)
            for (auto& w : n.main) {
                if (w.rs != r || w.re != re) continue;
                auto score_of = [&](const u16& x) {
                    if (x == w.surface) return w.s;
                    for (auto& a : w.alts) if (a.first == x) return a.second;
                    return -1.f;
                };
                std::vector<std::pair<u16, float>> top{{list[0], score_of(list[0])}};
                auto has = [&](const u16& x) { for (auto& t : top) if (t.first == x) return true; return false; };
                if (!has(w.surface)) top.push_back({w.surface, w.s});
                for (auto& a : w.alts)
                    if (a.second >= 0.03f && top.size() < 5 && !has(a.first)) top.push_back(a);
                // 二股の頭: 別の区切りの文を強さの順の位置に (強さ = 別の区切りの語のうち弱い方)
                if (&w == &n.main.front() && !n.fork.empty()) {
                    u16 fs;
                    float sc = 1;
                    for (auto& fw : n.fork) { fs += fw.surface; sc = std::min(sc, fw.s); }
                    if (!has(fs)) {
                        size_t at = 1;
                        while (at < top.size() && top[at].second >= sc) at++;
                        top.insert(top.begin() + long(at), {fs, sc});
                        s.fork_idx = int(at);
                    }
                }
                std::vector<u16> rest;
                for (auto& x : list) if (!has(x)) rest.push_back(x);
                s.tree_k = int(top.size());
                list.clear();
                for (auto& t : top) { list.push_back(t.first); scores.push_back(t.second); }
                list.insert(list.end(), rest.begin(), rest.end());
            }
    }
    scores.resize(list.size(), -1.f);
    s.scores = std::move(scores);
    s.cands = std::move(list);
    s.sel = 0;
    s.loaded = true;
}

void Composer::load_more(size_t i) {
    Seg& s = segs_[i];
    if (!conv_ || s.limit >= 2000) return;
    s.limit = std::max(s.limit, kCandidates) * 4;
    u16 head = s.reading.substr(0, s.reading.size() - s.tail_r.size());
    std::vector<u16> more;
    if (segs_.size() == 1 && !whole_.empty()) {   // 文全体の候補: 変換の上位をもっと
        for (auto& c : conv_->convert(left_context(i), s.reading, s.limit).cands) more.push_back(c);
    } else {
        for (auto& c : conv_->convert(left_context(i), head, s.limit).cands) more.push_back(c + s.tail_s);
    }
    // その読みの辞書の語を全部 (読みがちょうどその語。打ち切らない)
    for (auto& w : conv_->complete(head, s.limit, 0)) more.push_back(w + s.tail_s);
    for (auto& c : more) push_unique(s.cands, c);
}

void Composer::resize_focus(int delta) {
    cand_open_ = false;
    Seg& s = segs_[focus_];
    u16 rest;
    for (size_t i = focus_ + 1; i < segs_.size(); i++) rest += segs_[i].reading;
    int len = int(s.reading.size()) + delta;
    if (len < 1 || len > int(s.reading.size() + rest.size())) return;
    u16 all = s.reading + rest;
    u16 mine = all.substr(0, size_t(len));
    rest = all.substr(size_t(len));
    Conversion c = conv_ ? conv_->convert(left_context(focus_), mine, kCandidates) : Conversion();
    Seg ns{mine, c.cands.empty() ? mine : c.cands[0]};
    std::vector<Phrase> ph = group_phrases(c.words);   // 伸び縮みした文節の付属語 (候補を作るときに使う)
    if (ph.size() == 1 && ph[0].reading == mine && ph[0].head_r < mine.size() && ph[0].head_s < ph[0].surface.size()) {
        ns.tail_r = mine.substr(ph[0].head_r);
        ns.tail_s = ph[0].surface.substr(ph[0].head_s);
    }
    ns.changed = true;
    segs_.resize(focus_);
    segs_.push_back(std::move(ns));
    if (!rest.empty()) {
        std::vector<Seg> after = convert_phrases(left_context(focus_ + 1), rest, nullptr);
        segs_.insert(segs_.end(), after.begin(), after.end());
    }
    whole_.clear();
}

u16 Composer::alpha_source(const u16& reading) const {
    if (segs_.size() == 1 && raw_valid_ && !raw_.empty()) return raw_;
    return romaji_.to_romaji(reading);
}

void Composer::transform(Key f) {
    Seg& s = segs_[focus_];
    u16 out;
    switch (f) {
        case Key::F6: out = to_hiragana(s.reading); break;
        case Key::F7: out = to_katakana(s.reading); s.changed = true; break;
        case Key::F8: out = to_halfwidth_kana(s.reading); break;
        case Key::Muhenkan:   // 押すたびに カタカナ → 半角カタカナ → ひらがな
            f_cycle_ = (last_f_ == f) ? (f_cycle_ + 1) % 3 : 0;
            out = f_cycle_ == 0 ? to_katakana(s.reading) : f_cycle_ == 1 ? to_halfwidth_kana(s.reading) : to_hiragana(s.reading);
            if (f_cycle_ == 0) s.changed = true;
            break;
        case Key::F9:
        case Key::F10: {
            f_cycle_ = (last_f_ == f) ? (f_cycle_ + 1) % 3 : 0;
            u16 a = alpha_source(s.reading);
            a = to_halfwidth_ascii(a);
            a = f_cycle_ == 0 ? lower(a) : f_cycle_ == 1 ? upper(a) : capital(a);
            // かなの記号は英数字の記号に戻す (ー → -)
            for (auto& c : a) if (c == u'ー') c = u'-';
            out = f == Key::F9 ? to_fullwidth_ascii(a) : a;
            break;
        }
        default: return;
    }
    last_f_ = f;
    s.surface = out;
    s.cands.clear();
    s.loaded = false;
}

void Composer::back_to_input() {
    {
        u16 shown, reading;
        for (auto& s : segs_) { shown += s.surface; reading += s.reading; }
        log({"cancel", reading, shown, ctx_, first_cands_});
    }
    kana_.clear();
    for (auto& s : segs_) kana_ += s.reading;
    caret_ = kana_.size();
    segs_.clear();
    whole_.clear();
    cand_open_ = false;
    state_ = State::Input;
    live_key_.clear();
    prev_segs_.clear();
}

void Composer::commit_all() {
    u16 t, reading;
    std::vector<u16> segs;
    int changed = 0;
    for (auto& s : segs_) {
        t += s.surface;
        reading += s.reading;
        segs.push_back(s.surface);
        if (s.changed) {
            changed++;
            if (learning_ && !options.private_mode && !contains(sugg_set_, s.surface)) learning_->record(s.reading, s.surface);
            if (flow && !options.private_mode && contains(sugg_set_, s.surface)) flow->record_suggest(s.surface);
            log({"bseg", s.reading, s.surface, u16(), s.cands, s.sel});
        }
    }
    if (on_log) {
        int idx = -1;
        for (size_t i = 0; i < first_cands_.size(); i++) if (first_cands_[i] == t) idx = int(i);
        LogEvent e{"commit", reading, t, ctx_, first_cands_, idx, segs};
        e.extra = first_via_ + ",changed=" + std::to_string(changed);
        log(e);
    }
    commit_text(t);
    to_idle();
}

void Composer::log(LogEvent e) {
    if (on_log) on_log(e);
}

void Composer::commit_text(const u16& t) {
    commit_ += t;
    if (!t.empty() && !kana_.empty() && t != kana_) last_commit_ = {t, frozen_reading() + kana_};
    else last_commit_ = {};
    history_ = tail(history_ + t, CTX_MAX);
    if (!t.empty() && !kana_.empty() && !options.private_mode) {   // 再変換のための (表記, 読み)
        recent_.push_back({t, frozen_reading() + kana_});
        if (recent_.size() > 200) recent_.erase(recent_.begin());
    }
    if (flow && !options.private_mode) {
        flow->feed(t);
        post_ = flow->predict();
        flow->save();   // 節が閉じたときだけ書く (dirty)
    }
    after_emoji_.clear();   // 文の語の絵文字は PC 版では出さない (要らない、ユーザー 2026-10-14)
}

u16 Composer::reading_of(const u16& text) const {
    if (text.empty()) return u16();
    bool kana_only = true;
    for (char16_t c : text) kana_only = kana_only && (is_hira(c) || (c >= 0x309D && c <= 0x309E) || (c >= 0x30A1 && c <= 0x30FE));
    if (kana_only) return to_hiragana(text);
    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it)
        if (it->first == text) return it->second;
    return u16();
}

bool Composer::start_reconvert(const u16& reading) {
    if (reading.empty() || state_ != State::Idle) return false;
    start_input();
    insert(reading);
    convert_all();
    rebuild_view();
    return true;
}

void Composer::select_post(int index) {
    if (state_ != State::Idle || index < 0 || index >= int(post_.size())) return;
    const u16 t = post_[size_t(index)];
    commit_text(t);
    rebuild_view();
}

bool Composer::undo_commit() {
    if (last_commit_.first.empty() || state_ != State::Idle) return false;
    const u16 surface = last_commit_.first, reading = last_commit_.second;
    last_commit_ = {};
    // 文脈から取り消した確定を外す
    if (history_.size() >= surface.size() && history_.compare(history_.size() - surface.size(), surface.size(), surface) == 0)
        history_.resize(history_.size() - surface.size());
    return start_reconvert(reading);
}

void Composer::passthrough(const KeyEvent& ev) {
    last_commit_ = {};
    if (!flow || state_ != State::Idle) return;
    if (ev.key == Key::Backspace) flow->backspace();
    else if (ev.key == Key::Enter) flow->boundary();
    else flow->reset();   // 矢印・Home など: カーソルが動いた
    post_.clear();
    flow->save();
    rebuild_view();
}

void Composer::to_idle() {
    state_ = State::Idle;
    pred_tab_.clear();
    kana_.clear();
    caret_ = 0;
    romaji_.reset();
    segs_.clear();
    whole_.clear();
    first_cands_.clear();
    first_via_.clear();
    cand_open_ = false;
    alpha_run_ = false;
    live_key_.clear();
    live_cands_.clear();
    live_sugg_.clear();
    sugg_focus_ = -1;
    live_sugg_kinds_.clear();
    prev_segs_.clear();
    frozen_.clear();
    date_cands_.clear();
}

void Composer::finish() {
    if (state_ == State::Input) {
        flush_romaji();
        // ライブ変換で変換した文が見えているなら、それで確定する (Enter と同じ)。英語入力に切り替えたときに、
        // 見えていた変換がかなに戻って確定され、取り消されたように見えた (ユーザー 2026-10-09)
        if (options.live_display && !kana_.empty()) update_live();
        if (live_shown()) {
            log({"commit", kana_, live_top_, ctx_, live_cands_, 0, {}, "live"});
            commit_text(frozen_surface() + live_top_);
        } else {
            commit_text(frozen_surface() + kana_);
        }
        to_idle();
    } else if (state_ == State::Convert) {
        commit_all();
    }
    rebuild_view();
}

void Composer::cancel() {
    to_idle();
    rebuild_view();
}

// ---------------------------------------------------------------- 見せ方

// ライブ変換で変換した文を見せられるか: 設定がオンで、カーソルが読みの末尾にあり、今の読みの変換が出ている
// (読みの途中にカーソルを動かして直しているときは、かなで見せる)
bool Composer::live_shown() const {
    return options.live_display && state_ == State::Input && !kana_.empty() && caret_ == kana_.size() &&
           !alpha_run_ && !live_top_.empty() && live_key_ == conv_ctx() + u'\x01' + kana_;
}

void Composer::rebuild_view() {
    View v;
    if (state_ == State::Idle) v.post = post_;
    if (state_ == State::Input) {
        const u16& p = romaji_.pending();
        const u16 fz = frozen_surface();   // 固定した前の方 (変換した形で見せる)
        const int f = int(fz.size());
        if (f) v.spans.push_back({0, f, SpanKind::Converted});
        if (live_shown()) {   // ライブ変換: 変換した文 + 打ちかけのローマ字
            v.text = fz + live_top_ + p;
            v.caret = int(v.text.size());
            if (!live_top_.empty()) v.spans.push_back({f, int(live_top_.size()), SpanKind::Converted});
            if (!p.empty()) v.spans.push_back({f + int(live_top_.size()), int(p.size()), SpanKind::Input});
        } else {
            u16 in = kana_.substr(0, caret_) + p + kana_.substr(caret_);
            v.text = fz + in;
            v.caret = f + int(caret_ + p.size());
            if (!in.empty()) v.spans.push_back({f, int(in.size()), SpanKind::Input});
        }
        if (!live_cands_.empty() && options.live) {   // 打っている間の候補の窓は live のときだけ (リアルタイム変換だけでも動く)
            v.cand_open = true;
            v.cands = live_cands_;
            for (auto& x : v.cands) v.cand_notes.push_back(sugg_note(x));
            v.cand_sel = -1;
        }
    } else if (state_ == State::Convert) {
        for (size_t i = 0; i < segs_.size(); i++) {
            int st = int(v.text.size());
            v.text += segs_[i].surface;
            int len = int(segs_[i].surface.size());
            if (len) v.spans.push_back({st, len, i == focus_ ? SpanKind::Focused : SpanKind::Converted});
            if (i == focus_) { v.anchor = st; v.caret = st + len; }
        }
        if (cand_open_) {
            v.cand_open = true;
            v.cands = segs_[focus_].cands;
            if (segs_.size() == 1) for (auto& x : v.cands) v.cand_notes.push_back(sugg_note(x));
            v.cand_sel = segs_[focus_].sel;
            v.cand_short = segs_[focus_].tree_k;
            v.cand_scores = segs_[focus_].scores;
            v.cand_scores.resize(v.cands.size(), -1.f);
            v.cand_fork = segs_[focus_].fork_idx;
        }
        if (options.cand_tree && tree_.empty()) {
            // 木が無い (文 1 つで変換した・固定した前の方がある など): 文節の札だけを並べる (強さは分からない = -1)
            int r = 0;
            for (auto& sg : segs_) {
                TreeNode n;
                TreeWord w;
                w.surface = sg.surface;
                w.rs = r;
                w.re = r += int(sg.reading.size());
                w.s = -1;
                n.main.push_back(std::move(w));
                v.tree.push_back(std::move(n));
            }
            int f = 0;
            for (size_t i = 0; i < focus_; i++) f += int(segs_[i].reading.size());
            v.tree_rs = f;
            v.tree_re = f + int(segs_[focus_].reading.size());
        }
        if (options.cand_tree && !tree_.empty()) {
            // 木の語の区間と同じ文節は、今選んでいる表記で見せる (選び直したら木にも出る)
            v.tree = tree_;
            {   // 固定した前の方の文節 (木の外) も、強さの分からない札として頭に
                std::vector<TreeNode> pre;
                int q = -tree_off_;
                for (auto& sg : segs_) {
                    if (q >= 0) break;
                    TreeNode n;
                    TreeWord w;
                    w.surface = sg.surface;
                    w.rs = q;
                    w.re = q += int(sg.reading.size());
                    w.s = -1;
                    n.main.push_back(std::move(w));
                    pre.push_back(std::move(n));
                }
                v.tree.insert(v.tree.begin(), pre.begin(), pre.end());
            }
            int r = -tree_off_;
            for (size_t i = 0; i < segs_.size(); i++) {
                const int rs = r, re = r + int(segs_[i].reading.size());
                r = re;
                if (i == focus_) { v.tree_rs = rs; v.tree_re = re; }
                // 別の区切りを選んでまとめた文節: 木はそのまま (候補の列の別の区切りの行にケーブルがつながる)
                if (!segs_[i].own_reading.empty()) continue;
                for (auto& n : v.tree)
                    for (auto& w : n.main)
                        if (w.rs == rs && w.re == re && w.surface != segs_[i].surface) {
                            // 別の表記にしたら、その表記と元の 1 位を入れ替える (強さも)
                            auto it = std::find_if(w.alts.begin(), w.alts.end(), [&](auto& a) { return a.first == segs_[i].surface; });
                            float s = it != w.alts.end() ? it->second : 0.f;
                            if (it != w.alts.end()) w.alts.erase(it);
                            w.alts.insert(w.alts.begin(), {w.surface, w.s});
                            w.surface = segs_[i].surface;
                            w.s = s;
                        }
            }
        }
    }
    view_ = std::move(v);
}

}  // namespace shunti

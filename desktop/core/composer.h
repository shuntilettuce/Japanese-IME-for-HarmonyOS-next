// PC 版の入力の状態と操作 (Windows の TSF・Linux の Fcitx5 から同じものを使う)。
//
// ハードウェアキーボードで打つ PC の IME の決まり (MS-IME・Google 日本語入力と同じ操作):
//   ローマ字で打つ → 空白 (変換キー) で AI 変換 → 文節ごとに直す (←→ で文節を選び、Shift+←→ で伸び縮み、
//   空白・↓ で候補を選ぶ) → Enter で確定。F6〜F10 でひらがな・カタカナ・半角カナ・全角英数・半角英数。
//   無変換キーは押すたびに カタカナ → 半角カタカナ → ひらがな。
// 変換は文全体を一度にモデルに通し (左の文脈 = 入力欄のカーソルの左)、その 1 位を品詞で文節に分ける。
// 文節の候補は、その文節の自立語を、左の文節までを文脈にして変換し、付属語をつないだもの。
// 学習は、変換のあとで自分で選び直した文節だけを覚える (手を入れなかった文節は、文脈で決まる AI の結果に任せる)。
//
// スマホ版 (Android・HarmonyOS) と同じ動き:
//   打っている間も AI 変換を回し、候補の窓に出しておく (↓・Tab で選び始める)。
//   リアルタイム確定: 長く打つと、前の方で 2 回続けて同じ変換になった所 (または句読点の後ろ) までを確定する
//   (末尾の KEEP 字は残す)。確定した分は文脈としてモデルに渡すので、後ろの変換の質は落ちない。
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "converter.h"
#include "flow.h"
#include "romaji.h"
#include "store.h"
#include "text.h"

namespace shunti {

enum class Key {
    Char, Space, Enter, Backspace, Delete, Escape, Left, Right, Up, Down, Home, End, Tab, PageUp, PageDown,
    F6, F7, F8, F9, F10, Henkan, Muhenkan,
};

struct KeyEvent {
    Key key = Key::Char;
    char16_t ch = 0;     // Key::Char のときの字 (キーボードの配列に従って打たれた字)
    bool shift = false, ctrl = false, alt = false;
    bool raw = false;    // かなにせず打ったまま入れる (テンキー)
};

enum class SpanKind { Input, Converted, Focused };
struct Span { int start, len; SpanKind kind; };

// 入力中の文字の見せ方と候補の窓
struct View {
    u16 text;
    std::vector<Span> spans;
    int caret = 0;
    int anchor = 0;               // 候補の窓をこの字の下に出す
    bool cand_open = false;
    std::vector<u16> cands;
    std::vector<u16> cand_notes;  // 候補ごとに右に薄く添える種類 (もしかして・絵文字・日付 …。空なら無し)
    int cand_sel = -1;            // 選んでいる候補 (-1 = まだ選んでいない。打っている間の候補)
    // 提案 (打っている間だけ。候補の窓の上の行に出す)。sugg = 見せる形、sugg_text = 確定する文 (固定した前の方込み)
    std::vector<u16> sugg, sugg_text;
    std::vector<int> sugg_kinds;
    int sugg_sel = -1;            // 提案の列で選んでいるもの (-1 = 提案の列に入っていない)
    // 文の流れの続き (確定した後、何も打っていないときだけ。Tab で 1 つ目を入れる)
    std::vector<u16> post;
    // 候補の木 (変換中だけ。候補の窓の上の帯)。tree_rs〜tree_re = 選んでいる文節の読みの区間 (木の読みの位置)
    std::vector<TreeNode> tree;
    int tree_rs = -1, tree_re = -1;
    int cand_short = 0;
    std::vector<float> cand_scores;   // 候補ごとの木の強さ (-1 = 分からない)
    int cand_fork = -1;               // 別の区切りの候補の番号 (-1 = 無し)           // 候補の木: 選ぶ位置が cand_short 未満のあいだは先頭の cand_short 個だけ出す (0 = 全部)
};

// 入力の記録 (デバッグ用のビルドだけが受け取って書く。HarmonyOS 版 InputLog.ets と同じ種別とキー)。
//   commit = 変換して確定 (r 読み・s 表記・c 候補・i 選んだ番号・b 左の文脈・g 文節)
//   bseg   = 文節を選び直した (r・s・c・i)
//   auto   = リアルタイム確定で前の方を確定した (r・s・c = そのときの変換の候補・b)
//   raw    = かなのまま確定した / cancel = 変換をやめてかなに戻した (s = 出ていた変換) / clear = 入力を捨てた
struct LogEvent {
    const char* kind = "";
    u16 reading, surface, ctx;
    std::vector<u16> cands;
    int index = -1;
    std::vector<u16> segs;      // 文節の表記 (commit のとき)
    std::string extra;          // x (補足)
};

struct ComposerOptions {
    bool live = true;          // 打っている間も AI 変換して候補の窓に出す
    bool live_commit = true;   // 長い入力の前の方を固定する (保留: 入力欄には確定せず、変換した形で見せて文節で直せる)
    bool live_display = false; // ライブ変換: 入力中の文字を、空白を押す前から AI の 1 位で見せる (Enter でそのまま確定)
    bool always_cands = true;  // 変換中は候補の窓をいつも出す
    bool cand_tree = false;     // 変換中は候補の窓の上に候補の木 (文節の強さ・つながり・迷う区切り) を出す
    bool space_fullwidth = true;   // 何も打っていないときの空白を全角に (Shift で逆)
    int punct = 0;                 // 句読点: 0 = 、。 1 = ，． 2 = 、． 3 = ，。
    bool digits_fullwidth = false; // 数字を全角で入れる
    u16 half_symbols;              // 半角で入れる記号 (詳細設定)
    int numpad = 0;                // テンキー: 0 = 半角、1 = 数字の設定に合わせる、2 = 全角
    bool private_mode = false;     // 学習・文の流れ・提案の学習を残さない
};

class Composer {
public:
    static inline int PAGE = 9;         // 候補の窓の 1 ページの数 (1〜9 で選ぶ。詳細設定で 5〜9)
    static constexpr size_t KEEP = 8;        // 前の方を固定するとき、固定せずに残す末尾の字数の最小 (スマホ版と同じ)
    static constexpr size_t KEEP_MAX = 32;   // 速い PC で残す字数の上限 (モデルが学んだ読みの長さ)

    Composer(Converter* conv, Learning* learning, UserDict* dict);

    ComposerOptions options;
    // 入力の記録を受け取る (空なら何もしない。リリース用のビルドでは空のまま)
    std::function<void(const LogEvent&)> on_log;

    // 入力欄のカーソルの左の文 (変換の文脈)。読めない入力欄では空を返してよい (そのときはこれまでに確定した文を使う)
    void set_context_provider(std::function<u16()> f) { context_ = std::move(f); }
    // 入力欄が変わったとき: これまでに確定した文 (文脈の代わり) を忘れる
    void reset_history() { history_.clear(); post_.clear(); last_commit_ = {}; if (flow) flow->reset(); }
    // 確定の取り消し (Ctrl+Backspace、Google 日本語入力と同じ): 確定した直後だけ、その (表記, 読み)。無ければ表記が空
    const std::pair<u16, u16>& undoable() const { return last_commit_; }
    // 確定の取り消し: 入力欄から表記を消したあとで呼ぶ。読みに戻して変換し直す (変換中にする)
    bool undo_commit();
    // 再変換: 確定した文を読みに戻せるか (読みが分からなければ空)。かなだけの文はそのまま、漢字まじりはこれまで確定した記録から
    u16 reading_of(const u16& text) const;
    // 再変換を始める (読みを入れて変換した状態にする)。始めたら true
    bool start_reconvert(const u16& reading);
    // ローマ字の表に足す (詳細設定の「ローマ字の表」)
    void add_romaji(const u16& key, const u16& kana) { romaji_.add_pair(key, kana); }
    void reset_romaji() { romaji_ = Romaji(); }   // 足した綴りを外して元の表に
    void clear_recent() { recent_.clear(); }      // 再変換のために覚えた (表記, 読み) を消す (プライベートにしたとき)
    // 文の流れの予測 (無ければ使わない)。確定した文字を覚え、確定の後に続きを view().post に出す
    SentenceFlow* flow = nullptr;
    // 何も打っていないときに、こちらで扱わずアプリへ渡したキー (Backspace・Enter・矢印など)。文の流れに知らせる
    void passthrough(const KeyEvent& ev);

    // このキーをこちらで扱うか (状態は変えない。TSF の OnTestKeyDown 用)
    bool will_handle(const KeyEvent& ev) const;
    // キーを扱う。扱ったら true。確定する文は take_commit で、入力中の見せ方は view で受け取る
    bool press(const KeyEvent& ev);
    u16 take_commit() { u16 c; c.swap(commit_); return c; }
    const View& view() const { return view_; }
    // 固定せずに AI 変換に渡す末尾の字数 (calibrate で決めた値。テスト・記録用)
    size_t keep() const { return keep_; }
    void set_keep(size_t n) { keep_ = n; calibrated_ = true; }
    bool composing() const { return state_ != State::Idle; }
    bool converting() const { return state_ == State::Convert; }
    // かなを直接入れる (かな入力の配列: JIS かな・NICOLA・薙刀式)。゛ ゜ は前のかなに合わせる (か + ゛ -> が)
    void type_kana(const u16& s);

    // 候補の窓の index 番目をマウスで選んだ
    void select_candidate(int index);
    // 提案の index 番目を選んだ (マウス、または Shift+Enter で 0 番目)。打ったとおりの変換ではないので学習しない
    void select_suggestion(int index);
    // 文の流れの続き (view().post) の index 番目を入れた (マウス)
    void select_post(int index);
    // 入力中のものをいま見えているまま確定する (入力欄を離れたときなど)。確定した文は take_commit で
    void finish();
    // 入力中のものを捨てる
    void cancel();

private:
    enum class State { Idle, Input, Convert };
    std::vector<u16> post_;   // 文の流れの続き (view().post)
    struct Seg {
        u16 reading, surface;
        u16 tail_r, tail_s;      // 付属語の読みと表記 (候補は自立語だけを変換して、これをつなぐ)
        std::vector<u16> cands;
        bool loaded = false;
        int sel = 0;
        bool changed = false;    // 変換のあとで選び直した (確定したときに学習する)
        int limit = 0;           // 読み込んだ候補の数の上限 (最後のページに来たら増やして続きを読む)
        int tree_k = 0;          // 候補の木: 先頭の木の候補の数 (窓は選ぶ位置がこれ未満のあいだこれだけ出す)
        std::vector<float> scores;   // 候補の木の強さ (cands と同じ並び。分からなければ -1)
        // 候補の木の二股: cands[fork_idx] = 別の区切りの文 (区間の頭の文節だけが持つ)。選ぶと区間の文節を 1 つにまとめ、
        // 離れると元に戻す (merged = まとめた後ろの文節、own_* = まとめる前の自分の読みと付属語)
        int fork_idx = -1;
        std::vector<Seg> merged;
        u16 own_reading, own_tail_r, own_tail_s;
    };

    void start_input();
    void insert(const u16& t);
    void flush_romaji();
    void type_char(char16_t c, bool raw = false);
    bool press_input(const KeyEvent& ev);
    bool press_convert(const KeyEvent& ev);
    void sync_user_dict();
    void convert_all();
    void enter_single(const u16& surface, std::vector<u16> cands, int sel);
    std::vector<Seg> convert_phrases(const u16& ctx, const u16& reading, std::vector<u16>* whole);
    u16 left_context(size_t seg) const;
    void load_cands(size_t i);
    void load_more(size_t i);   // 最後のページに来た: 候補の続き (変換の上位をもっと + その読みの辞書の語を全部) を足す
    void resize_focus(int delta);
    void fork_merge(size_t i);     // 候補の木の二股: 文節 i から区間の終わりまでを 1 つにまとめる
    void fork_unmerge(size_t i);   // まとめたのを元の文節に戻す
    void transform(Key f);
    u16 alpha_source(const u16& reading) const;
    void back_to_input();
    void commit_all();
    void commit_text(const u16& t);
    void to_idle();
    void log(LogEvent e);
    void update_live();
    bool live_commit(const Conversion& c);
    u16 frozen_reading() const;
    u16 frozen_surface() const;
    u16 conv_ctx() const;
    void prime_keys(const u16& reading);   // 打った鍵の列を変換に渡す (読みが打った鍵から作れるときだけ)
    bool unfreeze_last();
    void calibrate();
    bool live_shown() const;
    void rebuild_view();

    Converter* conv_;
    Learning* learning_;
    UserDict* dict_;
    std::function<u16()> context_;

    State state_ = State::Idle;
    u16 ctx_;          // 左の文脈 (入力を始めたときのカーソルの左 + その後にリアルタイム確定した分)
    u16 history_;      // これまでに確定した文 (入力欄の文が読めないときの文脈)
    std::pair<u16, u16> last_commit_;   // 直前の確定 (表記, 読み)。確定の取り消しのため (次のキーで忘れる)
    std::vector<std::pair<u16, u16>> recent_;   // 最近確定した (表記, 読み)。再変換で読みに戻すため (200 件まで)
    u16 commit_;
    View view_;

    // 入力 (かな)
    u16 kana_;
    size_t caret_ = 0;
    Romaji romaji_;
    u16 raw_;               // 打ったキーそのまま (英字に戻すとき用。途中を直したら使えない)
    bool raw_valid_ = true;
    bool alpha_run_ = false;   // 大文字で始めた英字の続き (かなにしない)

    // 打っている間の変換
    u16 live_key_;                       // live_cands_ を作ったときの文脈と読み
    std::vector<u16> live_cands_;
    std::vector<u16> live_sugg_;          // 提案 (Conversion::sugg。打っている間の変換のもの)
    std::vector<int> live_sugg_kinds_;
    u16 typo_top_;
    std::vector<u16> live_pred_;          // 打っている間の候補に足した予測 (読みの続く語)。Tab はここから選ぶ
    std::vector<u16> after_emoji_;        // 1 位の文の語に当たる絵文字・顔文字 (確定の後の続き Tab の窓に)                        // 1 位にした打ち間違いの直し (“ ” で囲んで見せる)
    std::vector<u16> sugg_typo_;          // そのうち打ち間違いの直し (“ ” で囲んで見せる)
    u16 sugg_note(const u16& x) const;   // 候補の右に添える種類 (もしかして・絵文字 …)
    std::vector<u16> sugg_set_;           // 候補に混ぜた提案 (✦ を付けて見せる・学習しない)
    std::vector<u16> date_cands_;         // 日付・時刻の候補 (学習しない。sugg_note で日付と添える)
    void add_datetime(const u16& reading, std::vector<u16>& list);   // 日付・時刻の候補を list の後ろに足す (学習しない)
    int sugg_focus_ = -1;                 // 提案の列で選んでいるもの (Shift+↓ / Shift+Tab で入る。-1 = 入っていない)
    u16 live_top_;                       // 打っている間の変換そのものの 1 位 (予測・決まり文句を足す前)。ライブ変換で見せる
    std::map<size_t, u16> prev_segs_;    // 前回の 1 位の (語の区切りの読みの位置 -> そこまでの表記)
    // 固定した前の方 (保留)。入力欄には確定せず、変換した形で入力中の頭に見せる。変換 (空白) すると頭の文節になり、選び直せる。
    // 変換するのは残りの kana_ だけ (文脈は ctx_ + 固定した表記)。長い文でも変換が重くならず、モデルの読みの長さにも収まる
    std::vector<Seg> frozen_;
    // 固定せずに AI 変換に渡す末尾の字数。最初に打ったときに変換の速さを測って決める (calibrate): 速い PC ほど長く渡す
    size_t keep_ = KEEP;
    bool calibrated_ = false;

    // 変換
    std::vector<Seg> segs_;
    std::vector<u16> whole_;   // 文全体の候補 (文節が 1 つのとき、その候補に使う)
    std::vector<u16> first_cands_;   // 変換したときの文全体の候補 (記録用)
    std::vector<TreeNode> tree_;     // 直前のスペースの変換の候補の木 (convert_phrases が最後の変換のものを入れる)
    std::vector<TreeNode> last_tree_;
    int tree_off_ = 0;               // 木の読みの位置 0 が kana_ のどこか (固定した前の方の読みの長さ)
    std::vector<Word> live_words_;   // 直前の打っている間の変換の 1 位の語の区切り (固定オフのとき、前の方を使い回す)
    std::vector<int> pred_tab_;      // Tab で予測の候補に入ったとき、候補の中の予測の位置 (Tab はこの中だけを回る)
    std::string first_via_;          // どうやって変換を始めたか (space・live・fkey。記録用)
    size_t focus_ = 0;
    bool cand_open_ = false;
    uint32_t dict_version_ = 0;   // 変換の網に入れたユーザー辞書の版
    Key last_f_ = Key::Char;      // 続けて押した F9・F10・無変換 (押すたびに形を回す)
    int f_cycle_ = 0;
};

// 読み reading とその表記 surf で、挨拶の「今日は」をかな書きに直す (きょうは を打っていないときだけ。スマホ版と同じ)
u16 greeting_fix(const u16& reading, const u16& surf);

}  // namespace shunti

// PC 版の設定 (設定画面が書き、IME が読む。%APPDATA%\shunti IME\settings.json)。
// IME はアプリごとのプロセスに読み込まれるので、入力を始めるたびにファイルの更新を見て読み直す。
#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace shunti {

// 型ごとの Ctrl+字 (ひらがな・カタカナ・半角カナ・全角英数・半角英数)
inline const char* keymap_ctrl_chars(const std::string& keymap) {
    if (keymap == "atok") return "UIOP@";
    if (keymap == "kotoeri") return "JK;L:";
    return "UIOPT";
}

// 既定のショートカットキーの一覧。ctrl は型ごとの 5 字、ctrl_space・muhenkan_off・reconvert が false の機能は
// 入れない (古い settings.json を読み替えるときに、オフにしていた機能をそのままにするため)。
inline std::vector<std::pair<std::string, std::string>> build_default_shortcuts(
    const std::string& keymap, const std::string& ctrl, bool ctrl_space, bool muhenkan_off, bool reconvert) {
    std::vector<std::pair<std::string, std::string>> out;
    static const char* f[5] = {"hiragana", "katakana", "halfkana", "zen_alpha", "han_alpha"};
    for (int i = 0; i < 5 && i < int(ctrl.size()); i++) {
        if (ctrl[size_t(i)] == '-') continue;   // 割り当て無し
        out.push_back({"Ctrl+" + std::string(1, ctrl[size_t(i)]), f[i]});
    }
    out.push_back({"Ctrl+M", "confirm"});
    if (keymap == "kotoeri") {
        out.push_back({"Ctrl+I", "seg_shrink"});
        out.push_back({"Ctrl+Q", "seg_shrink"});
        out.push_back({"Ctrl+O", "seg_grow"});
        out.push_back({"Ctrl+W", "seg_grow"});
    } else {
        out.push_back({"Ctrl+K", "seg_shrink"});
        out.push_back({"Ctrl+L", "seg_grow"});
    }
    out.push_back({"Ctrl+Delete", "cand_delete"});
    out.push_back({"Ctrl+Backspace", "undo_commit"});
    out.push_back({"Ctrl+Shift+F10", "private_toggle"});
    if (ctrl_space) out.push_back({"Ctrl+Space", "ime_toggle"});
    out.push_back({"Henkan", "ime_on"});
    out.push_back({"Hiragana", "ime_on"});
    if (muhenkan_off) out.push_back({"Muhenkan", "ime_off"});
    if (reconvert) {
        if (keymap == "atok") out.push_back({"Shift+Henkan", "reconvert"});
        else if (keymap == "kotoeri") out.push_back({"Ctrl+Shift+R", "reconvert"});
        else out.push_back({"Henkan", "reconvert"});
    }
    return out;
}

// 既定のショートカットキー (型ごと)。Settings の既定・古い設定の移し替え・詳細設定の「初期状態に戻す」に使う。
inline std::vector<std::pair<std::string, std::string>> default_shortcuts(const std::string& keymap) {
    return build_default_shortcuts(keymap, keymap_ctrl_chars(keymap), true, true, true);
}

struct Settings {
    std::string theme = "auto";      // 候補の窓の色: auto (Windows に合わせる)・light・dark
    bool space_fullwidth = true;     // 何も打っていないときの空白を全角に (Shift で逆)
    int punct = 0;                   // 句読点: 0 = 、。 1 = ，． 2 = 、． 3 = ，。
    bool digits_fullwidth = false;   // 数字を全角で入れる
    bool live = true;                // 打っている間も候補を出す
    bool live_commit = true;         // リアルタイム確定
    bool live_display = false;       // ライブ変換: 入力中の文字を、空白を押す前から変換して見せる
    bool caps_alpha = true;          // 大文字で打ち始めたら英字のまま (ComposerOptions::caps_alpha)
    bool personal = true;            // M5: 書き方を覚えて変換に使う (オフなら学習の記録の並べ替えだけ。今までと同じ)
    bool typo_fix = true;            // ローマ字の打ち間違いを直す (オフなら打ったとおり。ブログの要望 2026-10-26)
    bool latin_auto = true;          // 英字の打ち分け (ローマ字で打った英単語を文脈で英字にする)。暴発が嫌ならオフ
    bool space_literal = false;      // 空白キーで変換しない (空白も読みに入れて変換を続ける。ComposerOptions::space_literal)
    bool cand_tree = false;          // 新デザインの候補バー (候補の木: 文節の札・強さの色・別の区切り)。既定はオフ
    bool ctrl_space = false;         // Ctrl+Space でも日本語入力のオン・オフ (shortcuts に移した。古い設定の読み替え用)
    std::string model = "standard";  // 変換モデル: light (XS3・メモリの少ない PC 向け)・standard (S7)
    // ---- 詳細設定 (2026-10-15。MS-IME・Beki IME などにある設定) ----
    std::string half_symbols;        // 半角で入れる記号 (! ? ( ) @ など。ここに無い記号は全角。, . / - [ ] は 、。・ー「」 の代わりに半角)
    int numpad = 0;                  // テンキー: 0 = いつも半角、1 = 数字の設定に合わせる、2 = いつも全角
    int page_size = 9;               // 候補の窓の 1 ページの数 (5〜9)
    bool muhenkan_off = true;        // 無変換キーで日本語入力をオフ (打っていないとき。shortcuts に移した。読み替え用)
    bool reconvert = true;           // 選んだ文字を変換キーで変換し直す (再変換。shortcuts に移した。読み替え用)
    bool app_default_open = true;    // 初めて使うソフトでの日本語入力 (以後はソフトごとに前の状態を覚える。ブログの要望 2026-10-18)
    bool private_mode = false;       // プライベート: 学習・文の流れ・提案の学習を残さない (Ctrl+Shift+F10 でも切り替え)
    // 打っている間の Ctrl + 字: ひらがな・カタカナ・半角カタカナ・全角英数・半角英数 (- は割り当て無し。英字か @ ; :)
    // (shortcuts に移した。古い設定の読み替え用)
    std::string ctrl_keys = "UIOPT";
    // キー割り当ての型 (Google 日本語入力 = Mozc の keymap に合わせた): msime / atok / kotoeri。
    // 文節の伸び縮み (msime・atok は Ctrl+K/L、kotoeri は Ctrl+I/O)、ATOK は ←→ だけで伸び縮み (Shift+←→ で文節を移る)、
    // 再変換のキー (msime 変換 / atok Shift+変換 / kotoeri Ctrl+Shift+R)。Ctrl+M で確定はどれも
    std::string keymap = "msime";
    // キー配列 (PC の物理キーを別の配列として読む。layouts/*.tsv、core/layout.h)。空 = ふつうのローマ字入力。
    // 名前は layout_<名前>.tsv の <名前> (設定のフォルダ → インストール先の順に探す): onishi・dvorak・jis_kana・nicola・naginata
    std::string layout;
    int layout_window = 100;  // 1 つだけ押していて この時間ほかと重ならなければ単打に (ms。実際のキーの届く遅れ 15〜30ms の余裕込み)
    int layout_overlap = 50;
    int layout_thumbs = 0;           // 親指キー: 0 無変換・変換 / 1 無変換・空白 / 2 空白・変換
    bool layout_continuous = true;   // 連続シフト
    bool layout_thumb_tap = true;    // 親指キーの単独打鍵を本来のキーとして
    bool layout_repeat = true;       // 押しっぱなしで繰り返す  // 同時押しとみなす重なり (短い方の押していた時間の %。速く打つ人は大きく)
    // ショートカットキー (既定も自分で追加も同じ一覧)。(キー "Ctrl+Shift+K"、機能 id "katakana")。
    // settings.json には "Ctrl+Shift+K=katakana" の形。上から順に見て、使える状態のものが効く
    std::vector<std::pair<std::string, std::string>> shortcuts = default_shortcuts("msime");
};

// 変換モデルの設定の値 -> モデルのファイル名 (辞書と同じフォルダ)。知らない値は standard
const char* model_file_name(const std::string& model);

// 読む (無い・壊れていれば既定のまま)。書く (一時ファイルから置き換える)
bool load_settings(const std::filesystem::path& p, Settings& s);
bool save_settings(const std::filesystem::path& p, const Settings& s);

// ファイルが変わっていたら読み直すための控え
class SettingsFile {
public:
    explicit SettingsFile(std::filesystem::path p) : path_(std::move(p)) {}
    // 変わっていれば読み直して true
    bool refresh();
    const Settings& get() const { return s_; }

private:
    std::filesystem::path path_;
    std::filesystem::file_time_type stamp_{};
    bool loaded_ = false;
    Settings s_;
};

}  // namespace shunti

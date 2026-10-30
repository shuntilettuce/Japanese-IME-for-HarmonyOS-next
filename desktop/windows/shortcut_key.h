// ショートカットキー (詳細設定で自分で追加) のキー "Ctrl+Shift+K" ⇔ VK + 修飾 と、機能 (id → 表示名)。
// IME (text_service.cpp) と設定 (settings_app.cpp) の両方で使う。Windows 専用 (VK を使うので core には置かない)。
#pragma once
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace shunti {
namespace win {

// キー文字列の分解。読めない文字列は ok = false
struct ShortcutKey {
    UINT vk = 0;
    bool ctrl = false, shift = false, alt = false;
    bool ok = false;
};

namespace {

// 名前の付いたキー (A〜Z・0〜9・F1〜F24 は別途。OEM キーなどは "VK_XX" の 16 進)
struct KeyName { const char* name; UINT vk; };
const KeyName KEY_NAMES[] = {
    {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Backspace", VK_BACK}, {"Delete", VK_DELETE},
    {"Escape", VK_ESCAPE}, {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
    {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
    {"Henkan", VK_CONVERT}, {"Muhenkan", VK_NONCONVERT}, {"Kana", VK_KANA}, {"Hiragana", 0xF2 /* VK_DBE_HIRAGANA */},
};

UINT vk_from_name(const std::string& name) {
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return UINT(c);
        if (c >= 'a' && c <= 'z') return UINT(c - 32);
        if (c >= '0' && c <= '9') return UINT(c);
    }
    if (name.size() >= 2 && name[0] == 'F') {   // F1〜F24
        int n = 0;
        for (size_t i = 1; i < name.size(); i++) {
            if (name[i] < '0' || name[i] > '9') { n = -1; break; }
            n = n * 10 + (name[i] - '0');
        }
        if (n >= 1 && n <= 24) return UINT(VK_F1 + n - 1);
    }
    for (auto& k : KEY_NAMES)
        if (name == k.name) return k.vk;
    if (name.size() >= 4 && name.compare(0, 3, "VK_") == 0) {   // 記号キー (VK_BA など 16 進)
        UINT v = 0;
        bool any = false;
        for (size_t i = 3; i < name.size(); i++) {
            char c = name[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = (v << 4) | UINT(d);
            any = true;
        }
        if (any) return v;
    }
    return 0;
}

std::string name_from_vk(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    for (auto& k : KEY_NAMES)
        if (k.vk == vk) return k.name;
    char buf[16];
    snprintf(buf, sizeof buf, "VK_%02X", int(vk));
    return buf;
}

}  // namespace

inline ShortcutKey parse_shortcut_key(const std::string& s) {
    ShortcutKey k;
    std::string rest = s;
    auto take = [&](const char* mod) {
        const std::string m(mod);
        if (rest.compare(0, m.size(), m) == 0) { rest.erase(0, m.size()); return true; }
        return false;
    };
    k.ctrl = take("Ctrl+");
    k.shift = take("Shift+");
    k.alt = take("Alt+");
    if (rest.empty()) return k;   // キー名が無い
    const UINT vk = vk_from_name(rest);
    if (!vk) return k;
    k.vk = vk;
    k.ok = true;
    return k;
}

inline std::string shortcut_key_string(UINT vk, bool ctrl, bool shift, bool alt) {
    std::string s;
    if (ctrl) s += "Ctrl+";
    if (shift) s += "Shift+";
    if (alt) s += "Alt+";
    s += name_from_vk(vk);
    return s;
}

inline std::string shortcut_key_string(const ShortcutKey& k) {
    return shortcut_key_string(k.vk, k.ctrl, k.shift, k.alt);
}

// 機能 (id は settings.json に書く値)。composing_only: 入力中 (読みがあるとき) だけ効く
struct ShortcutFunc { const char* id; const wchar_t* name; bool composing_only; };

inline const std::vector<ShortcutFunc>& shortcut_funcs() {
    static const std::vector<ShortcutFunc> f = {
        // 入力中 (読みがあるとき) だけ効く
        {"confirm", L"確定 (Enter)", true},
        {"cancel", L"取り消し (Escape)", true},
        {"convert", L"変換・次の候補 (Space)", true},
        {"prev_cand", L"前の候補 (Up)", true},
        {"hiragana", L"ひらがなに (F6)", true},
        {"katakana", L"カタカナに (F7)", true},
        {"halfkana", L"半角カナに (F8)", true},
        {"zen_alpha", L"全角英字に (F9)", true},
        {"han_alpha", L"半角英字に (F10)", true},
        {"seg_shrink", L"文節を縮める (Shift+←)", true},
        {"seg_grow", L"文節を伸ばす (Shift+→)", true},
        {"seg_prev", L"前の文節へ (←)", true},
        {"seg_next", L"次の文節へ (→)", true},
        {"cand_delete", L"候補を削除", true},
        {"backspace", L"1 字消す", true},
        {"delete_char", L"後ろの 1 字を消す", true},
        {"clear_all", L"入力を全部消す", true},
        {"seg_first", L"最初の文節へ・先頭へ", true},
        {"seg_last", L"最後の文節へ・末尾へ", true},
        {"page_next", L"候補の次のページ", true},
        {"page_prev", L"候補の前のページ", true},
        {"sugg_next", L"予測の候補を選ぶ", true},
        {"sugg_prev", L"予測の候補を戻る", true},
        {"sugg_commit", L"予測の 1 つ目で確定", true},
        {"kana_cycle", L"カタカナ → 半角 → ひらがな と順に", true},
        {"next_cand_down", L"次の候補", true},
        // いつでも効く
        {"ime_toggle", L"日本語入力のオン・オフ", false},
        {"ime_on", L"日本語入力をオン", false},
        {"ime_off", L"日本語入力をオフ", false},
        {"private_toggle", L"学習を一時的にオフ (切替)", false},
        {"undo_commit", L"確定の取り消し", false},
        {"reconvert", L"再変換", false},
        {"romaji_reconvert", L"選んだローマ字を日本語に", false},
        {"open_settings", L"設定を開く", false},
        {"space_full", L"全角の空白を入れる", false},
        {"space_half", L"半角の空白を入れる", false},
        {"open_dict", L"ユーザー辞書を開く", false},
        {"model_toggle", L"変換モデルの切り替え", false},
    };
    return f;
}

inline const wchar_t* shortcut_func_name(const std::string& id) {
    for (auto& f : shortcut_funcs())
        if (id == f.id) return f.name;
    return L"";
}

}  // namespace win
}  // namespace shunti

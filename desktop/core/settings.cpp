#include "settings.h"

#include <algorithm>
#include <system_error>

#include "json.h"
#include "store.h"

namespace fs = std::filesystem;

namespace shunti {

bool load_settings(const fs::path& p, Settings& s) {
    s = Settings();
    std::string text;
    if (!read_file(p, text)) return false;
    json::Value v;
    if (!json::Parser(text).parse(v) || v.type != json::Value::Object) return false;
    auto str = [&](const char* k, std::string& out) {
        const json::Value* x = v.get(k);
        if (x && x->type == json::Value::String) out = x->str;
    };
    auto boolean = [&](const char* k, bool& out) {
        const json::Value* x = v.get(k);
        if (x && x->type == json::Value::Bool) out = x->b;
    };
    str("theme", s.theme);
    boolean("space_fullwidth", s.space_fullwidth);
    boolean("digits_fullwidth", s.digits_fullwidth);
    boolean("live", s.live);
    boolean("live_commit", s.live_commit);
    boolean("live_display", s.live_display);
    boolean("cand_tree", s.cand_tree);
    boolean("space_literal", s.space_literal);
    boolean("latin_auto", s.latin_auto);
    boolean("caps_alpha", s.caps_alpha);
    boolean("ctrl_space", s.ctrl_space);
    str("model", s.model);
    if (const json::Value* x = v.get("punct"); x && x->type == json::Value::Number) s.punct = int(x->num) & 3;
    str("half_symbols", s.half_symbols);
    if (const json::Value* x = v.get("numpad"); x && x->type == json::Value::Number) s.numpad = std::clamp(int(x->num), 0, 2);
    if (const json::Value* x = v.get("page_size"); x && x->type == json::Value::Number) s.page_size = std::clamp(int(x->num), 5, 9);
    boolean("muhenkan_off", s.muhenkan_off);
    boolean("reconvert", s.reconvert);
    boolean("private_mode", s.private_mode);
    boolean("app_default_open", s.app_default_open);
    str("keymap", s.keymap);
    if (const json::Value* x = v.get("ctrl_keys"); x && x->type == json::Value::String)
        s.ctrl_keys = x->str;
    else
        s.ctrl_keys = keymap_ctrl_chars(s.keymap);
    if (s.ctrl_keys.size() != 5) s.ctrl_keys = keymap_ctrl_chars(s.keymap);
    for (auto& ch : s.ctrl_keys) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
    for (auto& ch : s.ctrl_keys) if (!((ch >= 'A' && ch <= 'Z') || ch == '@' || ch == ';' || ch == ':' || ch == '-')) ch = '-';
    str("layout", s.layout);
    if (const json::Value* x = v.get("layout_window"); x && x->type == json::Value::Number) s.layout_window = std::clamp(int(x->num), 20, 400);
    if (const json::Value* x = v.get("layout_overlap"); x && x->type == json::Value::Number) s.layout_overlap = std::clamp(int(x->num), 10, 95);
    if (const json::Value* x = v.get("layout_thumbs"); x && x->type == json::Value::Number) s.layout_thumbs = std::clamp(int(x->num), 0, 2);
    boolean("layout_continuous", s.layout_continuous);
    boolean("layout_thumb_tap", s.layout_thumb_tap);
    boolean("layout_repeat", s.layout_repeat);
    // キー割り当ての一覧。shortcuts_v が 2 ならそのまま、それ以外 (古い設定) は既定を今の設定に合わせて作り、
    // その後ろに自分で足したものを付ける (移し替え)
    std::vector<std::pair<std::string, std::string>> listed;
    if (const json::Value* x = v.get("shortcuts")) {
        if (x->type == json::Value::Array) {
            for (const auto& e : x->arr) {
                if (e.type != json::Value::String) continue;   // 読めない項目は飛ばす
                const size_t eq = e.str.find('=');
                if (eq == std::string::npos || eq == 0 || eq + 1 == e.str.size()) continue;
                listed.emplace_back(e.str.substr(0, eq), e.str.substr(eq + 1));
            }
        }
    }
    if (s.keymap != "atok" && s.keymap != "kotoeri") s.keymap = "msime";
    int shortcuts_v = 0;
    if (const json::Value* x = v.get("shortcuts_v"); x && x->type == json::Value::Number) shortcuts_v = int(x->num);
    if (shortcuts_v == 2) {
        s.shortcuts = std::move(listed);
    } else {
        auto defs = build_default_shortcuts(s.keymap, s.ctrl_keys, s.ctrl_space, s.muhenkan_off, s.reconvert);
        defs.insert(defs.end(), listed.begin(), listed.end());
        s.shortcuts = std::move(defs);
    }
    return true;
}

bool save_settings(const fs::path& p, const Settings& s) {
    auto b = [](bool x) { return x ? "true" : "false"; };
    std::string o = "{\n";
    o += "  \"theme\": " + json::quote(s.theme) + ",\n";
    o += std::string("  \"space_fullwidth\": ") + b(s.space_fullwidth) + ",\n";
    o += "  \"punct\": " + std::to_string(s.punct) + ",\n";
    o += std::string("  \"digits_fullwidth\": ") + b(s.digits_fullwidth) + ",\n";
    o += std::string("  \"live\": ") + b(s.live) + ",\n";
    o += std::string("  \"live_commit\": ") + b(s.live_commit) + ",\n";
    o += std::string("  \"live_display\": ") + b(s.live_display) + ",\n";
    o += std::string("  \"cand_tree\": ") + b(s.cand_tree) + ",\n";
    o += std::string("  \"space_literal\": ") + b(s.space_literal) + ",\n";
    o += std::string("  \"latin_auto\": ") + b(s.latin_auto) + ",\n";
    o += std::string("  \"caps_alpha\": ") + b(s.caps_alpha) + ",\n";
    o += "  \"model\": " + json::quote(s.model) + ",\n";
    o += "  \"half_symbols\": " + json::quote(s.half_symbols) + ",\n";
    o += "  \"numpad\": " + std::to_string(s.numpad) + ",\n";
    o += "  \"page_size\": " + std::to_string(s.page_size) + ",\n";
    o += std::string("  \"private_mode\": ") + b(s.private_mode) + ",\n";
    o += std::string("  \"app_default_open\": ") + b(s.app_default_open) + ",\n";
    o += "  \"keymap\": " + json::quote(s.keymap) + ",\n";
    o += "  \"layout\": " + json::quote(s.layout) + ",\n";
    o += "  \"layout_window\": " + std::to_string(s.layout_window) + ",\n";
    o += "  \"layout_overlap\": " + std::to_string(s.layout_overlap) + ",\n";
    o += "  \"layout_thumbs\": " + std::to_string(s.layout_thumbs) + ",\n";
    o += std::string("  \"layout_continuous\": ") + b(s.layout_continuous) + ",\n";
    o += std::string("  \"layout_thumb_tap\": ") + b(s.layout_thumb_tap) + ",\n";
    o += std::string("  \"layout_repeat\": ") + b(s.layout_repeat) + ",\n";
    o += "  \"shortcuts_v\": 2,\n";
    o += "  \"shortcuts\": [";
    for (size_t i = 0; i < s.shortcuts.size(); i++) {
        if (i) o += ", ";
        o += json::quote(s.shortcuts[i].first + "=" + s.shortcuts[i].second);
    }
    o += "]\n";
    o += "}\n";
    return write_file_atomic(p, o);
}

const char* model_file_name(const std::string& model) {
    if (model == "light") return "kkc_model_light.bin";
    // 高精度 (M4) は廃止: S7 (標準) の方が精度が上で、誤字も直せる (2026-10-14)。前に high を選んでいた人も標準を読む
    return "kkc_model.bin";
}

bool SettingsFile::refresh() {
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(path_, ec);
    bool exists = !ec;
    if (loaded_ && (!exists || t == stamp_)) return false;
    loaded_ = true;
    stamp_ = t;
    load_settings(path_, s_);
    return true;
}

}  // namespace shunti

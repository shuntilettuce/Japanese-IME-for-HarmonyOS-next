#include "strings.h"

#include <windows.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "../core/json.h"

namespace shunti {
namespace {

std::wstring utf8_to_w(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// 番号 → 文言の表。初めて使うときに、この実行ファイル (exe・dll) に埋めた RCDATA "STRINGS" (strings.json) を読む
std::map<int, std::wstring>& table() {
    static std::map<int, std::wstring> t;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&table), &m);
        if (!m) return;
        HRSRC r = FindResourceW(m, L"STRINGS", RT_RCDATA);
        if (!r) return;
        HGLOBAL g = LoadResource(m, r);
        if (!g) return;
        const void* p = LockResource(g);
        const DWORD n = SizeofResource(m, r);
        if (!p || !n) return;
        const std::string text(static_cast<const char*>(p), n);
        json::Value v;
        if (!json::Parser(text).parse(v) || v.type != json::Value::Object) return;
        for (const auto& kv : v.obj) {
            if (kv.second.type != json::Value::String) continue;
            t[std::atoi(kv.first.c_str())] = utf8_to_w(kv.second.str);
        }
    });
    return t;
}

}  // namespace

const wchar_t* T(int id) {
    const auto& t = table();
    const auto it = t.find(id);
    if (it != t.end()) return it->second.c_str();
    static thread_local std::wstring miss;
    miss = L"#" + std::to_wstring(id);
    return miss.c_str();
}

}  // namespace shunti

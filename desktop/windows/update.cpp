#include "update.h"
#include "strings.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace shunti::update {
namespace {

const wchar_t* REPO = L"/repos/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases?per_page=30";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// https の GET。リダイレクト (GitHub の添付は別のホストへ飛ぶ) は WinHTTP が追う。本文を返す (失敗は空で err に理由)
bool http_get(const std::wstring& url, std::string& body, std::wstring& err) {
    URL_COMPONENTS u = {sizeof u};
    wchar_t host[256] = L"", path[2048] = L"";
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { err = T(148); return false; }
    HINTERNET s = WinHttpOpen(L"shunti-IME-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = T(149); return false; }
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = false;
    if (r) {
        const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(r, hdr, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code == 200) {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                    std::string buf(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf.data(), avail, &got) || got == 0) break;
                    body.append(buf.data(), got);
                }
                ok = true;
            } else {
                err = T(150) + std::to_wstring(code) + T(151);
            }
        } else {
            err = T(152) + std::to_wstring(GetLastError()) + L")";
        }
        WinHttpCloseHandle(r);
    } else if (err.empty()) {
        err = T(149);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// JSON の中の "key":"値" を pos から探す (GitHub の返事だけを読むので、簡単な探し方で足りる)
bool next_value(const std::string& j, const std::string& key, size_t& pos, std::string& out) {
    const std::string k = "\"" + key + "\"";
    size_t p = j.find(k, pos);
    if (p == std::string::npos) return false;
    p = j.find('"', j.find(':', p + k.size()) + 1);
    if (p == std::string::npos) return false;
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return false;
    out = j.substr(p + 1, e - p - 1);
    pos = e + 1;
    return true;
}

std::vector<int> parts(const std::wstring& v) {
    std::vector<int> o;
    int x = 0;
    bool any = false;
    for (wchar_t c : v) {
        if (c >= L'0' && c <= L'9') { x = x * 10 + (c - L'0'); any = true; }
        else if (c == L'.') { o.push_back(x); x = 0; any = false; }
        else break;
    }
    if (any) o.push_back(x);
    return o;
}

}  // namespace

bool older(const std::wstring& a, const std::wstring& b) {
    std::vector<int> x = parts(a), y = parts(b);
    while (x.size() < y.size()) x.push_back(0);
    while (y.size() < x.size()) y.push_back(0);
    return x < y;
}

Latest find_latest() {
    Latest best;
    std::string body;
    if (!http_get(std::wstring(L"https://api.github.com") + REPO, body, best.error)) return best;
    // Release ごとに tag_name・html_url・添付の browser_download_url が並ぶ (新しい順)
    size_t pos = 0;
    std::string tag;
    while (next_value(body, "tag_name", pos, tag)) {
        const size_t next_tag = body.find("\"tag_name\"", pos);
        if (tag.rfind("windows-v", 0) != 0) continue;
        const std::wstring ver = widen(tag.substr(9));
        if (!best.version.empty() && !older(best.version, ver)) continue;
        // この Release の範囲で zip とページを探す
        const std::string block = body.substr(pos, next_tag == std::string::npos ? std::string::npos : next_tag - pos);
        size_t q = 0;
        std::string url, page;
        std::wstring zip;
        while (next_value(block, "browser_download_url", q, url))
            if (url.find("shunti-IME-windows-") != std::string::npos && url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0) zip = widen(url);
        page = "https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases/tag/" + tag;
        if (zip.empty()) continue;
        best.version = ver;
        best.zip_url = zip;
        best.page_url = widen(page);
    }
    if (best.version.empty() && best.error.empty()) best.error = T(153);
    return best;
}

bool download_and_install(const Latest& l, std::wstring& err) {
    std::string zip;
    if (!http_get(l.zip_url, zip, err)) return false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path dir = std::filesystem::path(tmp) / (L"shunti-IME-update-" + l.version);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path zp = dir / L"package.zip";
    HANDLE f = CreateFileW(zp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = T(154); return false; }
    DWORD w = 0;
    const bool wrote = WriteFile(f, zip.data(), DWORD(zip.size()), &w, nullptr) && w == zip.size();
    CloseHandle(f);
    if (!wrote) { err = T(154); return false; }
    // 展開 (Windows 10 以降の PowerShell の Expand-Archive)
    std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zp.wstring() +
                       L"' -DestinationPath '" + (dir / L"x").wstring() + L"' -Force\"";
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = T(155);
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) { err = T(155); return false; }
    // install.ps1 は zip の中のフォルダ (shunti-IME-windows-x.y.z) にある
    std::filesystem::path ps;
    for (auto& e : std::filesystem::recursive_directory_iterator(dir / L"x", ec))
        if (e.path().filename() == L"install.ps1") { ps = e.path(); break; }
    if (ps.empty()) { err = T(156); return false; }
    // install.ps1 が自分で管理者の確認 (UAC) を出して写す。窓は出したまま (結果を見られるように)
    const std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + ps.wstring() + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), ps.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
        err = T(157);
        return false;
    }
    return true;
}

}  // namespace shunti::update

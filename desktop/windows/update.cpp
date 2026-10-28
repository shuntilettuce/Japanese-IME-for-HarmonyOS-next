#include "update.h"
#include "strings.h"
#include "../core/release_info.h"
#include "../core/text.h"

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

}  // namespace

bool older(const std::wstring& a, const std::wstring& b) {
    auto u8 = [](const std::wstring& w) { return to_utf8(u16(w.begin(), w.end())); };
    return version_older(u8(a), u8(b));
}

Latest find_latest() {
    Latest best;
    std::string body;
    if (!http_get(std::wstring(L"https://api.github.com") + REPO, body, best.error)) return best;
    const ReleaseInfo r = latest_release(body, "windows-v", "shunti-IME-windows-", ".zip");
    if (r.version.empty()) { best.error = T(153); return best; }
    best.version = widen(r.version);
    best.zip_url = widen(r.asset_url);
    best.page_url = widen(r.page_url);
    for (auto& c : r.changes) best.changes.push_back(widen(c));
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

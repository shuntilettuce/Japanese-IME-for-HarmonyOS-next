// shunti-IME-setup.exe: ネットからのインストーラー。GitHub の最新の Windows 版をダウンロードして入れる
// (zip を展開して install.bat を押す手間を無くす。入れ直し・更新にも使える)
#include <windows.h>

#include <string>

#include "update.h"
#include "strings.h"

using shunti::T;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const wchar_t* title = T(141);
    shunti::update::Latest l = shunti::update::find_latest();
    if (l.version.empty()) {
        MessageBoxW(nullptr, (T(142) + l.error +
                              T(143)).c_str(),
                    title, MB_ICONWARNING);
        return 1;
    }
    const std::wstring q = L"shunti IME " + l.version + T(144) + T(145) + T(146);
    if (MessageBoxW(nullptr, q.c_str(), title, MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
    std::wstring err;
    if (!shunti::update::download_and_install(l, err)) {
        MessageBoxW(nullptr, (T(147) + err).c_str(), title, MB_ICONERROR);
        return 1;
    }
    return 0;
}

// 更新の確認と入れ直し (設定画面の「更新を確認」と、ネットからのインストーラー shunti-IME-setup.exe で使う)。
// GitHub の Release から Windows 版の最新 (タグ windows-vX.Y.Z) を探し、zip をダウンロードして展開し、install.ps1 で入れる
#pragma once
#include <string>
#include <vector>

namespace shunti::update {

struct Latest {
    std::wstring version;   // 0.6.1 (見つからなければ空)
    std::wstring zip_url;   // shunti-IME-windows-0.6.1.zip の URL
    std::wstring page_url;  // Release のページ (変更点)
    std::vector<std::wstring> changes;   // 変更点の箇条書き (Release の本文の「- 」の行。太字の見出しがあればそれだけ)
    std::wstring error;     // 調べられなかった理由
};

// 最新の Windows 版を調べる (試用版も含む)
Latest find_latest();
// a < b か ("0.6.0" < "0.6.1")
bool older(const std::wstring& a, const std::wstring& b);
// zip をダウンロードして展開し、install.ps1 を (管理者の確認つきで) 起動する。起動できたら true。失敗の理由は err に
bool download_and_install(const Latest& l, std::wstring& err);

}  // namespace shunti::update

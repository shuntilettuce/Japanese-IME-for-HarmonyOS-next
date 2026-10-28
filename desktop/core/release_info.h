// GitHub の Release の一覧 (api.github.com/repos/.../releases の JSON) から、その OS の最新の版と主な変更点を読む。
// 取りに行くのは各 OS (Windows は WinHTTP、Linux は curl) で、ここは読むだけ
#pragma once
#include <string>
#include <vector>

namespace shunti {

struct ReleaseInfo {
    std::string version;    // 0.7.3 (見つからなければ空)
    std::string asset_url;  // 添付 (zip・deb) の URL
    std::string page_url;   // Release のページ
    // 確認の画面に出す主な変更点。本文に <!-- 更新の確認 ... --> (ページには出ない) があればその中の行、
    // 無ければ本文の箇条書き (「- **見出し**: 説明」は見出しだけ) を最大 8 行
    std::vector<std::string> changes;
};

// tag_prefix (windows-v・linux-v) のタグで、添付の名前が asset_prefix で始まり asset_suffix で終わるものがある Release のうち、いちばん新しい版
ReleaseInfo latest_release(const std::string& json, const std::string& tag_prefix, const std::string& asset_prefix,
                           const std::string& asset_suffix);
// a < b か ("0.7.2" < "0.7.3")
bool version_older(const std::string& a, const std::string& b);
// 本文 (Markdown) から確認の画面に出す変更点 (latest_release が使う。試験用に出しておく)
std::vector<std::string> release_change_lines(const std::string& body);

}  // namespace shunti

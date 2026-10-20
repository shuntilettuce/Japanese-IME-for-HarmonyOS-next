// 画面に出る日本語の文言。strings.json (リソース RCDATA "STRINGS") の番号を引く。無ければ L"#番号" を返す
#pragma once

namespace shunti {
// strings.json の番号 → 文言 (UTF-8 の strings.json を実行ファイルに埋め込んである)。見つからなければ L"#番号"
const wchar_t* T(int id);
}  // namespace shunti

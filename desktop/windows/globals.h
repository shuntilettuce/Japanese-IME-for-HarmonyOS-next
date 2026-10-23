// Windows 版 (TSF のテキストサービス) の共通のもの: GUID、DLL の参照数、変換エンジンと学習・ユーザー辞書
#pragma once
#include <windows.h>

#include <filesystem>
#include <string>

#include "../core/converter.h"
#include "../core/flow.h"
#include "../core/settings.h"
#include "../core/store.h"

namespace shunti {
namespace win {

// {3CA2ED9D-36D7-4E5E-92FB-5A702C39A46B} テキストサービスの CLSID
inline const GUID CLSID_TextService = {0x3ca2ed9d, 0x36d7, 0x4e5e, {0x92, 0xfb, 0x5a, 0x70, 0x2c, 0x39, 0xa4, 0x6b}};
// {80F9DCBC-6C43-4043-B004-C6275415630E} 入力の方法 (日本語のプロファイル)
inline const GUID GUID_Profile = {0x80f9dcbc, 0x6c43, 0x4043, {0xb0, 0x04, 0xc6, 0x27, 0x54, 0x15, 0x63, 0x0e}};
// 入力中の文字の見せ方 (入力中・変換済み・選んでいる文節)
// {F0F4A827-FDB8-4FEB-89DC-470D92BD1BEF}
inline const GUID GUID_AttrInput = {0xf0f4a827, 0xfdb8, 0x4feb, {0x89, 0xdc, 0x47, 0x0d, 0x92, 0xbd, 0x1b, 0xef}};
// {597C1D27-D6CF-4E37-8086-C0052FF4F48C}
inline const GUID GUID_AttrConverted = {0x597c1d27, 0xd6cf, 0x4e37, {0x80, 0x86, 0xc0, 0x05, 0x2f, 0xf4, 0xf4, 0x8c}};
// {D3F48B6A-560D-4ACB-91CE-F1346E5C8C77}
inline const GUID GUID_AttrFocused = {0xd3f48b6a, 0x560d, 0x4acb, {0x91, 0xce, 0xf1, 0x34, 0x6e, 0x5c, 0x8c, 0x77}};
// {04CE839D-FB9E-477A-ABB9-CEC4957AA2DD} ライブ変換で打ち間違いを直した所 (波線)
inline const GUID GUID_AttrCorrected = {0x04ce839d, 0xfb9e, 0x477a, {0xab, 0xb9, 0xce, 0xc4, 0x95, 0x7a, 0xa2, 0xdd}};

constexpr LANGID LANG_JA = MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN);
constexpr wchar_t DISPLAY_NAME[] = L"shunti IME";

extern HINSTANCE g_inst;
void dll_add_ref();
void dll_release();
LONG dll_refs();

// この DLL のあるフォルダ (辞書とモデルもここに置く)
std::filesystem::path module_dir();
// 学習とユーザー辞書を置くフォルダ (%APPDATA%\shunti IME)
std::filesystem::path user_dir();

// 変換エンジン・学習・ユーザー辞書 (プロセスに 1 つ。最初に使うときに開く)。
// 辞書とモデルのファイルはメモリに写すだけなので、同じ IME を使うアプリどうしで中身を共有する
struct Core {
    Converter conv;
    Learning learning;
    UserDict dict;
    SettingsFile settings;   // 設定画面が書く settings.json (入力を始めるたびに読み直す)
    SentenceFlow flow;       // 文の流れの予測 (flow.tsv)
    Core();
    // 変換モデルを設定の値 (light・standard・high) のものにする。今と同じなら何もしない。
    // ファイルが無ければ standard (kkc_model.bin) を使う。前のモデルはメモリから外す
    void use_model(const std::string& name);

private:
    std::filesystem::path dir_;
    const void* lex_ = nullptr;
    size_t lex_size_ = 0;
    const void* model_ = nullptr;
    std::string model_file_;
};
Core& core();

// Windows がダークモード (アプリ) か
bool apps_dark();
// タスクバーがダークか
bool system_dark();

}  // namespace win
}  // namespace shunti

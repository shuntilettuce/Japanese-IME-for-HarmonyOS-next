# 開発


### 必要環境

- [DevEco Studio](https://developer.huawei.com/consumer/cn/deveco-studio/)（HarmonyOS NEXT SDK, API 12+）
- Node.js（辞書生成・評価スクリプト用）
- 実機または HarmonyOS エミュレータ、`hdc`（HarmonyOS Device Connector、SDK同梱）

### ビルド

DevEco Studio でプロジェクトを開くか、CLI から:

```bash
# AI変換の辞書とモデルを取得 (初回と更新時。無くてもビルドはでき、AI変換が使えないだけ)
python tools/fetch_ai_assets.py

# デバッグビルド (HAP)
hvigorw assembleHap --mode module -p product=default -p buildMode=debug

# 実機へインストール (デバッグ署名済みHAP)
hdc install <出力されたhapファイル>
```

### Android 版

`android/` にあります。Android SDK と NDK 27.2.12479018 が要ります（CMake は不要。変換エンジン `entry/src/main/cpp/engine.cpp` を NDK の clang で直接ビルドする）。

```bash
# 辞書とモデル (HarmonyOS 版と共有。entry/src/main/resources/rawfile/kkc_*.bin に置かれ、ビルド時に APK に同梱される)
python tools/fetch_ai_assets.py

# デバッグビルド
cd android && ./gradlew assembleDebug

# リリースビルド (android/keystore.properties に署名の鍵を書いておく。git には入れない)
cd android && ./gradlew assembleRelease
```

### Windows 版

`desktop/` にあります。`desktop/core` は PC 版の共通部分（ローマ字・AI 変換・文節・学習・ユーザー辞書・入力の状態）、`desktop/windows` は TSF のテキストサービス（x64 / x86 の DLL）と設定画面です。Visual Studio 2022 Build Tools（C++）が要ります。

```bat
rem 辞書とモデル (entry/src/main/resources/rawfile/kkc_*.bin)
python tools/fetch_ai_assets.py

rem 試験 (打鍵を流し込んで入力の決まりを確かめる)
desktop\tests\build_test.bat

rem DLL・設定画面をビルドして desktop\build\dist に集める。install.ps1 でこのパソコンに入れる (管理者の許可が要る)
desktop\windows\build.bat
powershell -ExecutionPolicy Bypass -File desktop\build\dist\install.ps1

rem 配る zip を作る
powershell -ExecutionPolicy Bypass -File desktop\windows\package.ps1 -Version 0.1.1
```

アイコンは `desktop/windows/art/` の Zori-chan の絵から `python desktop/tools/gen_icons.py` で作る。

### Linux 版

`desktop/linux` にあります。Fcitx5 の入力メソッドのアドオンで、変換は Windows 版と同じ `desktop/core` を使います。ビルドと試験の手順は [desktop/linux/README.md](../desktop/linux/README.md) の「ソースからビルドする」へ。

HarmonyOS 版のソースを正本として、Android 版に取り込むもの:

- `node tools/gen_android_tables.mjs` — キーの配置・QWERTY・ローマ字・記号・絵文字・顔文字・決まり文句・色・差別語の除外・ライセンス表記を `android/app/src/main/assets/` に書き出す（HarmonyOS 版の表を直したら流し直す）
- `node tools/gen_android_golden.mjs` — NumberFormatter / DateTimePredictor / JapaneseConverter を Node でそのまま動かした正解を作る。Kotlin 版は `./gradlew testDebugUnitTest`（GoldenTest）で一致を確かめる

### プロジェクト構成

[README の「リポジトリの構成」](../README.md#リポジトリの構成) を見てください。

IME本体は `InputMethodExtensionAbility` として別プロセス（`:inputMethod`）で動作するため、本体アプリとはサンドボックスが分離されています（ユーザー辞書やログの読み書きが両側で別経路になっているのはこのため）。

### 変換を変えるときの確かめ方

変換はAI変換です。エンジンは `entry/src/main/cpp/engine.cpp` で、HarmonyOS・Android・Windows・Linuxの各版が同じものを使います。

- キーボードの見た目や設定画面だけを変える場合は、実機で触って確かめれば十分です。変換の試験は要りません。
- `engine.cpp` や辞書の扱いを変える場合は、Windowsで次の試験を通してください。変換の上位の候補、文節、学習の決まりを確かめます。Visual Studio 2022 Build Tools (C++) が要ります。

```bat
python tools/fetch_ai_assets.py
desktop\tests\build_test.bat
```

最後に `all ok` と出れば通っています。

`tools/ime-eval`、`tools/mozc_data/compare_engines.js`、`tools/blind-eval` は以前の変換エンジン用です。今のAI変換には使えません。

### コントリビュート

Issue・Pull Request歓迎です。変換に手を入れる変更は、上の試験が通ることを確かめてから送ってください。精度の比べ方で迷ったら、Issueで相談してください。

---

## 開発版のみの機能: 入力ログ収集

変換精度を実際の入力から測るため、**開発ビルドにのみ**入力ログ収集を入れてあります。
配布版（AppGalleryなどのリリースビルド）には含めません。

- 記録: 読み・確定した表記・選んだ候補の番号・文節の区切りと選択・変換範囲の変更・候補削除・未知語学習・削除した文字
- 記録しない: パスワード等の secure field、クリップボードの内容
- 保存先: 端末内アプリサンドボックスの 1 ファイルのみ。送信は一切しない
- 閲覧・書き出し・消去: 本体アプリのフッター「入力ログ」から

収集コードは `entry/src/main/ets/ime/InputLog.ets` に閉じており、外部の呼び出し箇所は
すべて行末に `// [DEBUG-LOG]` が付いています。リリース前の手順:

```
node tools/check_debug_log.js   # 収集地点を全部列挙（未マークの参照があれば exit 1）
```

出力に並んだファイルを削除し、`// [DEBUG-LOG]` の付いた行を消したあと、
もう一度実行して「収集コードはありません（リリース可）」になることを確認します。

> 収集したログは private リポジトリを含め、git に入れないでください。
> clone・バックアップ・コラボレータに渡り、履歴からは消せません。

# shunti Japanese IME — 高精度AIによる日本語入力

元はHarmonyOS NEXT (API 12+) 向けに唯一存在したフリック対応日本語 IME でしたが、windows,linux,Androidにも後から対応しました。
スマホやパソコン向けIMEとして最低限の機能を備え、独自AI(shuntelligence)によるgoogle日本語入力やCopilot Keyboardを超える高精度な漢字変換が完全ローカルで動作します。個人開発のオープンソースプロジェクトです。

> **開発者**: shuntilettuce
> **貢献者**: [端島](https://github.com/Bakunetsuuuuu),[HiSubway](https://github.com/HiSubway)
> **ライセンス**: [MIT](./LICENSE)（バンドル辞書データは別ライセンス、下記参照）


---
# インストール方法

## HarmonyOS版

**Huawei AppGallery** から入手できます。
中国本土版では認証の関係でリリースしていません。

AppGallery で「shunti Japanese IME」を検索してインストール後、以下の手順で有効化してください。

**有効化手順：**
1. 設定 → システム → 入力方法 
2. 一覧に表示される **shunti Japanese IME** を選択

（メニュー名は端末の HarmonyOS バージョンにより多少異なる場合があります）

---

## Android 版（shunti IME）

**Google Play Storeには掲載されていません**

Android 向けのシンプルな日本語キーボードです（Android 8.0 以上、64bit の ARM 端末）。**[GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) **もしくは**Huawei AppGallery** から APK で入手できます。インストール後にアプリを開いて出る指示に従い、有効化してください。

- 全ての処理に必要なデータは全て APK に同梱しており、**インターネット通信権限を持ちません**
- フリック / QWERTY（ローマ字）をかな・英字それぞれで選択可能、数字パッド、記号・絵文字の一覧
- 予測、日付・時刻、数字の書き換え、パーソナライジング学習、ユーザー辞書（品詞登録可）
- クリップボードの貼り付け、片手モード（記号・空白の長押し）、フローティング（⚙ の長押し）、高さ・幅・位置の調整、ライト / ダーク

入力の決まり・キーの配置・記号や絵文字の表は HarmonyOS 版と共有しています（gboardに寄せたレイアウト）。プライバシーポリシー: [日本語](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-android) / [English](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-android-en)

---

## Windows 版（shunti IME）

大手の IME を超える変換精度を、630 万パラメータの小さな AI（shuntelligence S4）で提供します。Windows 10 / 11（64 ビット版）で動く日本語入力です。[GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) から zip を入手し、展開して `install.bat` を実行します（署名していないため「Windows によって PC が保護されました」と出る場合がありますが「詳細情報」→「実行」）。

| 1 位の正解率 | AJIMEE-Bench（182 問） | 日常の文（105 文） |
|---|---|---|
| Google 日本語入力 | 53.8% | 81.0% |
| Microsoft IME | 54.9% | 79.0% |
| **shunti IME（S4）** | **67.6%** | **87.6%** |
(日常の文=オリジナルベンチマーク。普通の会話で出るような文を大量に用意しています)

どの IME にも同じ打鍵（ローマ字で打つ → スペースで変換 → Enter で確定）を TSF 越しに流し込み、確定した文を採点した（2026 年 9 月、Windows 11）。AJIMEE-Bench（azooKey、CC BY-SA 3.0）は、ローマ字で打てない字を含む 18 問を除いた 182 問。全角・半角の違いは問わない。

- 変換はパソコンの中だけで行い、**通信する機能を持ちません**。
- 変換位置調整（Shift + ← → で伸び縮み）、F6〜F10、無変換キーでカタカナ・半角カタカナ
- 学習、ユーザー辞書（品詞登録可）、候補ウィンドウのライト / ダーク
- 設定はタスクバーの キャラクター(Zori-chan) を右クリック、またはスタートメニューの「shunti IME の設定」から

---

## Linux 版（shunti IME・試用版）

Windows 版と同じ変換（shuntelligence S4・同じ入力の決まり）を、Linux の入力の仕組み **Fcitx5** の上で動かします。Ubuntu 24.04 以降・Debian 13 以降（64 ビットの Intel・AMD）向けの `.deb` を [GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) に置いています。

```bash
sudo apt install ./fcitx5-shunti_0.1.0_amd64.deb
```

入れたあと Fcitx5 に切り替えて「shunti IME」を足す。手順・設定・ユーザー辞書は [desktop/linux/README.md](./desktop/linux/README.md) にあります。通信する機能を持たないのは Windows 版と同様。

---

## 機能一覧(HarmonyOS版)

### 入力方式
| 機能 | 説明 |
|---|---|
| **フリック入力** | Gboard風の 4×5 レイアウト。上下左右フリックでかな入力 |
| **QWERTY ローマ字入力** | 全ローマ字パターン対応（っ/ん/拗音含む） |
| **モード切替** | ひらがな / カタカナ / 英数字をワンタップで切替 |
| **濁点・半濁点** | フリックキーで゛゜を即時付与・サイクル |

### 変換
| 機能 | 説明 |
|---|---|
| **スペース変換** | スペースで候補一覧表示 → 繰り返しで次候補へ |
| **文節変換モード** | 文全体を文節に分割して個別に変換先を選択 |
| **カタカナ変換** | 全文をカタカナに変換する候補を常に提供。「カタカナっぽい」単語では優先度が上がります。|
| **変換学習** | 選んだ変換先の優先度を自動で上げ、次回から上位表示 |
| **ユーザー辞書** | アプリ本体から「よみ→単語」＋品詞（名詞/動詞/形容詞/人名/地名）を登録。動詞・形容詞は活用形も自動で変換候補に |
| **括弧変換** | 「かっこ」で `()` `「」` `【】` 等を入力。閉じ括弧は保留され、► キー（QWERTY では候補バーの待機バッジをタップ）を押したところで挿入されるので、囲む範囲を自分で決められる。設定 → 入力 → 括弧の閉じ方 で「次の確定で自動的に閉じる」も選択可 |

### キーボード操作
| 機能 | 説明 |
|---|---|
| **長押し削除** | ⌫ 長押でで連続削除 |
| **カーソル移動** | ◄ ► キーでカーソル左右移動。閉じ括弧の保留中は ► が「ここで閉じる」を兼ねる |
| **記号パネル** | 約 300 種の記号・矢印・数学記号・全角文字 |
| **絵文字パネル** | 8 カテゴリ 512 種の絵文字 |
| **クリップボード** | コピーしたテキストを候補バーに表示してワンタップ貼り付け。画像・ファイルは対応アプリの貼り付け機能を呼び出し |
| **片手モード** | キーボード全体を左右どちらかに寄せて縮小表示 |
| **ダークモード** | 端末の設定に自動追従（手動切替なし） |

### 設定
キーボードパネルの ⚙ から。キーボードレイアウト（QWERTY / フリック）・片手モード・変換エンジン・クリップボード履歴・など全機能を利用できます。

---

**AI変換について**: モデルの重みと推論のコードは本リポジトリで公開していますが、学習のコードと教材は公開していません。長い文は、変換が落ち着いた前の方から自動で確定し、変換するのは末尾の十数字だけにしています（数十字を同時に変換処理に入れると重いため）。辞書とモデル（計約120MB）は GitHub Releases に置いてあり、ビルド前に `python tools/fetch_ai_assets.py` で取得します。辞書の改善は実際の入力ログ（後述）を元に継続的に行っています。

---

## 開発

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

`desktop/linux` にあります。Fcitx5 の入力メソッドのアドオンで、変換は Windows 版と同じ `desktop/core` を使います。ビルドと試験の手順は [desktop/linux/README.md](./desktop/linux/README.md) の「ソースからビルドする」へ。

HarmonyOS 版のソースを正本として、Android 版に取り込むもの:

- `node tools/gen_android_tables.mjs` — キーの配置・QWERTY・ローマ字・記号・絵文字・顔文字・決まり文句・色・差別語の除外・ライセンス表記を `android/app/src/main/assets/` に書き出す（HarmonyOS 版の表を直したら流し直す）
- `node tools/gen_android_golden.mjs` — NumberFormatter / DateTimePredictor / JapaneseConverter を Node でそのまま動かした正解を作る。Kotlin 版は `./gradlew testDebugUnitTest`（GoldenTest）で一致を確かめる

### プロジェクト構成（抜粋）

```
entry/src/main/ets/
  ime/          変換エンジン本体・IME拡張のロジック
    KanaKanjiConverter.ets   かな漢字変換のコア（辞書lookup・学習・3エンジンのブレンド）
    KeyboardController.ets   InputMethodExtensionAbility側の入力制御・状態管理
    JapaneseConverter.ets    ローマ字/フリック入力の変換・文字種処理
    ConjugationEngine.ets    活用形の自動展開（ユーザー辞書登録時など）
    InputLog.ets             デバッグ専用の入力ログ収集（下記参照）
  components/    キーボードUI（フリック/QWERTY/記号/絵文字/設定画面 等）
  pages/         本体アプリ側の画面（設定・ユーザー辞書・プライバシーポリシー）
tools/
  mozc_data/     mozc/JMdict/SudachiDictから統計データエンジンの辞書を構築するスクリプト群
  ime-eval/      変換精度の回帰テスト（corpus_test*.js）
  blind-eval/    Google日本語入力との変換結果比較用コーパス・スコアラー
  check_debug_log.js   InputLog呼び出し箇所の棚卸し（リリース前チェック）
```

IME本体は `InputMethodExtensionAbility` として別プロセス（`:inputMethod`）で動作するため、本体アプリとはサンドボックスが分離されています（ユーザー辞書やログの読み書きが両側で別経路になっているのはこのため）。

### 変換精度の回帰テスト

辞書やロジックを変更した際は、既存コーパスでのスコアを必ず確認してください。

```bash
bun tools/mozc_data/compare_engines.js corpus_test9.js
bun tools/mozc_data/compare_engines.js corpus_test10.js
bun tools/mozc_data/compare_engines.js corpus_test11.js
```


### コントリビュート

Issue・Pull Request歓迎です。辞書データや変換ロジックに手を入れる変更は、上記の回帰テストで既存スコアがなるべく下がっていないことを確認のうえ送ってください。

---

## プライバシーポリシー

- [日本語](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy)
- [English](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-en)
- [中文](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-zh)

Harmony版本体アプリ（ユーザー辞書画面）のフッターからも同じ内容を確認できます。

本アプリは入力内容・変換履歴を外部サーバーへ一切送信しません。すべての処理はデバイス内で完結します。

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

---

## 開発支援のご案内

shunti IMEは広告なし・完全無料で個人が開発しているオープンソースプロジェクトです。
現在、日々の変換精度向上のために発生するAI利用コストやAI学習用GPUを個人で負担しており、開発継続のための資金が不足している状態です。

もし shunti を気に入っていただけましたら、開発継続のために缶コーヒー1杯分だけでもご支援（寄付）をいただけますと大変励みになります。

- **Buy Me a Coffee**: **[buymeacoffee.com/shunti](https://buymeacoffee.com/shunti)**
- **GitHub Sponsors**: **[github.com/sponsors/shuntilettuce](https://github.com/sponsors/shuntilettuce)**

いただいたご支援は変換精度向上のために充てさせていただきます。

---

## ライセンス

### アプリコード

`.ets` ファイル等のアプリ独自コードは **MIT License** で提供します。

### 変換辞書 (dict.json, global_dict.json) — track A（旧辞書。現在アプリには含まれていない）

`entry/src/main/resources/rawfile/dict.json` / `global_dict.json` は以下のデータから構築した自作辞書です。詳細な出典は [`DATA_SOURCES.md`](./DATA_SOURCES.md) を参照してください。

- 常用漢字（文部科学省告示）— パブリックドメイン
- Unicode Unihan データベース — パブリックドメイン
- Wikipedia日本語版から収集した読み情報 — CC BY-SA 4.0
- Wiktionary日本語版から収集した読み情報 — CC BY-SA 4.0 / GFDL
- JMdict/EDICT（電子化辞書研究開発グループ）— CC BY-SA 4.0
- 日本郵便 郵便番号データ — 自由利用可
- 独自収録語彙（global_dict）

### AI変換 — shuntelligence（モデル）と shuntorge（辞書）

- **shuntelligence**（`kkc_model.bin`、変換モデルの重み）— **CC BY 4.0**。商用利用・改変・再配布ができますが、「shuntelligence（shuntilettuce）」の表示が必要です。
- **shuntorge**（`kkc_lex.bin`、AI変換の辞書）— 独自部分は MIT。mozc の辞書・連接コスト・記号・絵文字データ（BSD-3-Clause / IPAdic ライセンス）と、読みの一部に SudachiDict（Apache License 2.0）を含みます。独自部分は、手入れ済みの語・地名（日本郵便 郵便番号データと照合）・ネットの読み・技術用語・教材から拾った語。
- 推論エンジン（`entry/src/main/cpp/`）— MIT。

学習に使った文章と条件は [`DATA_SOURCES.md`](./DATA_SOURCES.md) を参照してください。

### 文節分割

外部データファイルには依存せず、文法ルールをアプリ内に実装した独自のViterbiアルゴリズムで分割しています（track A）。

### 統計データ変換エンジン (mozc_dict.json 等) — track B（旧辞書。現在アプリには含まれていない）

上記の独自辞書とは完全に別データの、切り替え式の第二変換エンジンです。

- mozc（Google）— BSD-3-Clause
- JMdict/EDICT（読みへの追加候補表記としてのみ利用）— CC BY-SA 4.0
- SudachiDict（Works Applications）— Apache License 2.0

ライセンス全文は [`THIRD_PARTY_NOTICES.md`](./THIRD_PARTY_NOTICES.md) を参照してください。

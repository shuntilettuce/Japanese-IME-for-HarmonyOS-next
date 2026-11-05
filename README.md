# shunti IME — zenz を使わない、自作 AI の日本語入力

Windows・Linux・Android・HarmonyOS で動く日本語 IME です。漢字変換は、モデルの設計・学習・推論エンジンまで一から作った小さな AI（**shuntelligence**）が、端末の中だけで行います。個人開発のオープンソースです。

**入手**: [Windows 版](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases/tag/windows-v0.7.9) ・ [Linux 版 (.deb)](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases/tag/linux-v0.7.7) ・ [Android 版](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases/tag/android-v1.3.4) ・ HarmonyOS 版（AppGallery で「shunti Japanese IME」）

> **開発者**: shuntilettuce
> **貢献者**: [端島](https://github.com/Bakunetsuuuuu),[HiSubway](https://github.com/HiSubway)
> **ライセンス**: [MIT](./LICENSE)（辞書とモデルの重みは別ライセンス、下記参照）

---
## zenz を使わない変換 AI

AI で変換する日本語 IME の多くは、azooKey の zenz を使っています。shunti IME の AI は、zenz の重みも、zenz を先生にした学習も使っていない別系統です（作者の調べた範囲では、ほかに見当たりません）。

- 約 640 万パラメータの小さな Transformer。重みは **8.2MB**（int8）で、スマホの CPU だけで動きます
- 辞書で読みから表記の候補の網を作り、AI がカーソルの左の文（と、固定した前の方を直すときは後ろの文）を読んで道を選びます
- 推論エンジンは C++ で自作し、4 つの OS で同じものが動きます

### 大手 IME より 20 ポイント以上高い

同じ打鍵（ローマ字で打つ → スペースで変換 → Enter で確定）を各 IME に流し込み、確定した文を同じやり方で採点しました（前の文脈つき。shunti IME は同じ問題を変換エンジンに直接渡して測った）。

| 1 位の正解率 | 動き方 | AJIMEE-Bench | 日常の文 | dev |
|---|---|---|---|---|
| **shunti IME（S8）** | **CPU だけ・端末の中（8.2MB）** | **81.3%** | **94.3%** | **85.6%** |
| Google 日本語入力 | | 53.8% | 81.0% | 70.9% |
| Microsoft IME | | 54.4% | 78.1% | 71.7% |
| Copilot Keyboard（Microsoft） | | 54.9% | 88.6% | 68.8% |
| zenz-v3.1-xsmall（azooKey） | 端末の中 | 70.3% | 91.4% | 79.5% |
| zenz-v3.1-small（azooKey） | 端末の中・より大きいモデル | 88.5% | 94.3% | 85.5% |

難しい変換を集めた [AJIMEE-Bench](https://github.com/azooKey/AJIMEE-Bench)（azooKey 作、CC BY-SA 3.0。ローマ字で打てない字を含む 18 問を除いた 182 問）では、より重いモデルの zenz-v3.1-small に及びませんが、日常の文では並びます。日常の文（105 文）は普通の会話で出るような文を集めた自作のベンチです。2026 年 10〜11 月、作者調べ。

### 完全ローカル

- 変換は端末の中で完結し、打った文字を外へ送信しません。Android 版はインターネットの権限そのものを持ちません
- モデルの重みと推論のコードは公開しています

### タイプミスは文脈を見て直す

- ローマ字の打ち違い（よろしくおねぎあします → **よろしくお願いします**、ごさしゅうくだしあ → **ご査収ください**）や、フリックの隣のキー、よくある誤読（げっきょく → 月極）を直します
- 直した形が変換候補トップに “ ” つきで出て、微妙なときは候補の中の提案として出ます。誤字のない文を勝手に直すことはほぼありません（普通の文 400 文で 0 件）
- Windows 版は直す強さと、読む文脈の字数を設定で変えられます

### 文脈による書き分け

- 絵を**描く** / 字を**書く**、庭には**二羽**鶏
- 英字の語は、ふだん書かれる形で出ます（iphone → **iPhone**、ぎっとはぶ → **GitHub**）
- 固有名詞は Wikipedia から約 39 万語、企業名や人名地名など

### 変換以外も気が利く

- 日付（きょう → 2026/11/06 …）・郵便番号から住所・絵文字・顔文字・括弧も、候補の中に提案として並びます
- 「お疲れ様です」と打つと、続けて「。」「本日の進捗報告です。」のように、よく打つ文の流れを覚えて出します
- ＠の後ろには gmail.com などのドメインを出します

元は HarmonyOS NEXT 向けに唯一存在したフリック対応の日本語 IME で、Windows・Linux・Android には後から対応しました。

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

大手の IME を超える変換精度を、約 640 万パラメータの小さな AI（shuntelligence S8）で提供します。Windows 10 / 11（64 ビット版）で動く日本語入力です。[GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) から zip を入手し、展開して `install.bat` を実行します（署名していないため「Windows によって PC が保護されました」と出る場合がありますが「詳細情報」→「実行」）。

| 1 位の正解率 | AJIMEE-Bench（182 問） | 日常の文（105 文） |
|---|---|---|
| Google 日本語入力 | 53.8% | 81.0% |
| Microsoft IME | 54.4% | 78.1% |
| **shunti IME（S8）** | **81.3%** | **94.3%** |
(日常の文=オリジナルベンチマーク。普通の会話で出るような文を大量に用意しています)

どの IME にも同じ打鍵（ローマ字で打つ → スペースで変換 → Enter で確定）を TSF 越しに流し込み、確定した文を採点した（前の文脈つき、2026 年 10 月、Windows 11。S8 は 11 月に測り直した。shunti IME は同じ問題を変換エンジンに直接渡して測った）。AJIMEE-Bench（azooKey、CC BY-SA 3.0）は、ローマ字で打てない字を含む 18 問を除いた 182 問。全角・半角の違いは問わない。

- 変換はパソコンの中だけで行い、**通信する機能を持ちません**。
- 変換位置調整（Shift + ← → で伸び縮み）、F6〜F10、無変換キーでカタカナ・半角カタカナ
- 学習、ユーザー辞書（品詞登録可）、候補ウィンドウのライト / ダーク
- 設定はタスクバーの キャラクター(Zori-chan) を右クリック、またはスタートメニューの「shunti IME の設定」から

---

## Linux 版（shunti IME・試用版）

Windows 版と同じ変換（shuntelligence S8・同じ入力の決まり）を、Linux の入力の仕組み **Fcitx5** の上で動かします。Ubuntu 24.04 以降・Debian 13 以降（64 ビットの Intel・AMD）向けの `.deb` を [GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) に置いています。

```bash
sudo apt install ./fcitx5-shunti_0.7.7_amd64.deb
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

**AI変換について**: モデルの重みと推論のコードは本リポジトリで公開していますが、学習のコードと教材は公開していません。長い文は、変換が落ち着いた前の方から自動で確定し、変換するのは末尾の十数字だけにしています（数十字を同時に変換処理に入れると重いため）。辞書とモデル（計約120MB）は GitHub Releases に置いてあり、ビルド前に `python tools/fetch_ai_assets.py` で取得します。辞書の改善は作者の実際の入力ログ（開発版のみで集めるもの。[docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md)）を元に継続的に行っています。

---

## 開発

ビルド・試験・プロジェクト構成・開発版の入力ログ収集は [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md) にまとめています。Issue・Pull Request 歓迎です。

---

## プライバシーポリシー

- [日本語](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy)
- [English](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-en)
- [中文](https://shuntilettuce.github.io/Japanese-IME-for-HarmonyOS-next/privacy-policy-zh)

Harmony版本体アプリ（ユーザー辞書画面）のフッターからも同じ内容を確認できます。

本アプリは入力内容・変換履歴を外部サーバーへ一切送信しません。すべての処理はデバイス内で完結します。

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
- **shuntorge**（`kkc_lex.bin`、AI変換の辞書）— Wikipedia日本語版の固有名詞と JMdict/EDICT のよくある誤読・読みの揺れ（どちらも CC BY-SA 4.0）を含むため、**辞書ファイル全体は CC BY-SA 4.0** です（独自部分は単独では MIT）。mozc の辞書・連接コスト・記号・絵文字データ（BSD-3-Clause / IPAdic ライセンス）と、読みの一部に SudachiDict（Apache License 2.0）を含みます。独自部分は、手入れ済みの語・地名（日本郵便 郵便番号データと照合）・ネットの読み・技術用語・教材から拾った語。
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

# 辞書データの出典一覧

`entry/src/main/resources/rawfile/dict.json` および `global_dict.json` の語彙は、
本プロジェクトの開発過程（複数セッションにわたる）で以下の外部データソースから
収集・編纂されました。個々のエントリに出典タグは付与されていないため
（28万件超の規模でエントリ単位のタグ付けは非現実的）、本ファイルが
出典追跡の唯一の記録です。git のコミット履歴と合わせて参照してください。

**免責事項**: 本ファイルは開発時点での把握内容をまとめたものであり、法的助言では
ありません。公開配布前には、特に JMdict（下記）のシェアアライク条項について
確認することを推奨します。

**関連ファイル**: `entry/src/main/resources/rawfile/mozc_dict.json` /
`mozc_costs.json` / `mozc_matrix.json`（設定でオプション有効化する第二変換
エンジン「統計データ」用、track Aとは完全に別データ）の出典は
`THIRD_PARTY_NOTICES.md` および `tools/mozc_data/README.md` を参照。JMdict
のみ両トラックで（別々の目的に）利用されているため、下記3.に両方の利用内容を
記載しています。

---

## 1. Wikipedia日本語版 (ja.wikipedia.org)

- **ライセンス**: CC BY-SA 4.0（一部旧版は3.0）
- **利用内容**: 記事本文を MeCab で形態素解析し、単語の「読み」を抽出。記事の
  文章そのものは保存・再配布していない（読み↔漢字の対応関係のみを抽出）。
  駅名・地名・専門用語カテゴリの網羅的収集に使用。
- **関連コミット**: `c38514d`〜`f1cb947`（本セッション、バッチ11〜16、駅・
  市区町村・廃駅・難語）、`db49fab`〜`021c4f1`（バッチ3〜10）、`edd5ab9`
  （Wikipedia corpus deep expansion）等
- **帰属表示**: アプリ内ライセンス画面（SymbolView.ets）・プライバシー
  ポリシー画面（PrivacyPolicy.ets）に記載済み

## 2. Wiktionary日本語版 (ja.wiktionary.org)

- **ライセンス**: CC BY-SA 4.0 + GFDL（デュアルライセンス）
- **利用内容**: 見出し語・読みの抽出（初期の dict.json 構築に使用）
- **関連コミット**: `4ff3f6e`, `48c59ba`, `a6cf6b0`, `2b45b58`, `a2cf1df`
- **帰属表示**: 記載済み — アプリ内ライセンス画面（`SymbolView.ets`）

## 3. JMdict/EDICT（電子化辞書研究開発グループ, EDRDG）

- **ライセンス**: Creative Commons Attribution-ShareAlike 4.0
- **利用内容 (track A, dict.json/global_dict.json)**: 「196K canonical
  dictionary, priority entries cross-referenced」として語彙収集に利用
- **利用内容 (track B, mozc_dict.json — 別セッション、後日追加)**:
  `tools/mozc_data/build_jmdict_augment.py` が、mozc/実ipadic統計データ
  エンジン（独自辞書とは完全に別データの、設定でオプション有効化する
  第二変換エンジン）向けに、既存の読みへの追加の候補表記（漢字表記の
  バリエーション）としてのみ利用。新しい読みキーの追加は一切行わない
  （DPのセグメンテーション挙動に影響を与えないための制約、詳細は
  `tools/mozc_data/build_jmdict_augment.py` のモジュールdocstring参照）。
- **関連コミット**: `1996469`（corpus blitz + JMdict expansion, track A）
- **帰属表示**: 記載済み — アプリ内ライセンス画面（`SymbolView.ets`
  「ⓘ ライセンス」、変換辞書セクションとtrack Bの統計データセクション
  両方）、`THIRD_PARTY_NOTICES.md`（track B分の全文条件を記載）、
  本ファイル。EDRDG公式要件（ソフトウェア／アプリでファイルを使用する
  場合、ドキュメント・宣伝資料・ウェブサイト等で使用と出典を明記する
  必要がある）を満たす。推奨URL:
  https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project
- **シェアアライク条項**: JMdict由来のデータを含む派生物（dict.json/
  global_dict.json の該当エントリ、および mozc_dict.json への追加候補）
  は CC BY-SA 4.0 の下で配布される（このリポジトリの他のデータ・コードは
  元々MIT/BSD等の互換ライセンスであり、抵触しない）。
- **定期更新要件**: EDRDG方針は「入手可能な最新版からのデータの定期的な
  更新手順」の実装を求めている。本プロジェクトは実行時取得ではなく
  ビルド時に静的アセットへ焼き込む方式のため、`tools/mozc_data/
  fetch_jmdict.py` を毎リリース前に再実行（キャッシュを消してから）
  して最新スナップショットを取り込む運用とする。

## 4. Aozora Bunko（青空文庫）

- **ライセンス**: 収録作品ごとに異なる（著作権保護期間満了＝パブリック
  ドメイン、または著作権者の許諾に基づく無料公開）
- **利用内容**: 古典文学作品から文語・難読語彙を収集
- **関連コミット**: `5172741`（Aozora+Wikipedia）, `1996469`（同上に言及）

## 5. 日本郵便 郵便番号データ (KEN_ALL.CSV)

- **ライセンス**: 日本郵便が無償公開する公共データ、自由利用可
- **利用内容**: 全国の町域名・読みの網羅的収集（区より細かい地名）。
  加えて、`tools/build_place_packs.py` がこのデータと照合して
  「どの読みが町域名か」と「どの都道府県のものか」を判定し、
  地名を8地方の語彙パック (`tools/vocab_packs/place_*.pack`) に分割している。
  接尾辞だけの判定では 拡張漢字・英数字・企業城下町 のような一般語を
  巻き込むため、表記と読みの両方が郵便データに存在するものだけを地名と
  確定している。
- **取得元**: 都道府県別ファイル
  <https://www.post.japanpost.jp/service/search/zipcode/download/kogaki-zip.html>
  （全国一括 `ken_all.zip` は 2026-08 時点で 404 だったため県別を使用。
  都道府県はファイル名ではなくレコード内の都道府県欄から取る）
- **関連コミット**: `46e6ae1`（初回収集）
- **帰属表示**: アプリ内ライセンス画面に記載済み

## 5b. Wikidata

- **ライセンス**: CC0 1.0（パブリックドメイン提供）
- **利用内容**: `tools/build_place_core.py` が2つの一覧を取得し、
  「地方パックを切っても引けるべき地名」の判定にのみ使用する。
  1つは**現行の市区町村**（廃止日 P576 を持たないもの／1,727件）、
  もう1つは**日本の鉄道駅とそのかな表記**（P1814／7,218件）。
  駅は表記から「駅」・読みから「えき」を落とした**基底の地名**だけを
  取り、駅名そのもの（「米原駅」）は登録しない。地名さえあれば
  「米原」＋「駅」で組めるうえ、駅名を単位で持つと重複が増えるため
  （以前この形の重複を7,486件削除している）。
- **取得方法**: SPARQL エンドポイント <https://query.wikidata.org/sparql>。
  問い合わせ文は `tools/build_place_core.py` の `QUERY_MUNI` /
  `QUERY_STATION` にそのまま置いてある。取得結果は
  `tools/.cache/wikidata/`（git 管理外）に残り、2回目以降は通信しない。
- **帰属表示**: CC0 のため法的な義務は無いが、出典として本ファイルに記載。

## 6. 常用漢字表（文部科学省告示）

- **ライセンス**: パブリックドメイン（政府告示）
- **帰属表示**: アプリ内ライセンス画面に記載済み

## 7. Unicode Unihan データベース

- **ライセンス**: Unicode, Inc. のライセンス（データ利用は一般的に許可、
  Unicode社の利用条件表示が求められる場合がある）
- **帰属表示**: アプリ内ライセンス画面に記載済み

## 8. SudachiDict（Works Applications Co., Ltd.）

- **ライセンス**: Apache License 2.0
- **利用内容 (track B, mozc_dict.json のみ)**: `tools/mozc_data/
  build_sudachi_augment.py` が、統計データエンジン向けに、small/core
  レキシコンCSVから既存の読みへの追加の候補表記（漢字表記のバリエー
  ション）としてのみ利用。JMdictと同じ「新しい読みキーは追加しない」
  安全モード。track Aのdict.json/global_dict.jsonでは未使用。
- **帰属表示**: 記載済み — アプリ内ライセンス画面（`SymbolView.ets`
  「ⓘ ライセンス」track Bセクション）、`THIRD_PARTY_NOTICES.md`
  （全文条件を記載）、本ファイル。
- **注記**: Apache-2.0は許諾的ライセンスで、JMdictのCC BY-SAと異なり
  シェアアライク（同一ライセンスでの再配布）義務はない。帰属表示のみ
  required。

---

## 9. AI変換（shuntelligence / shuntorge）

AI変換（設定 → 変換エンジン → AI変換）のモデル **shuntelligence**（`kkc_model.bin`）と辞書 **shuntorge**
（`kkc_lex.bin`）は GitHub Releases に置き、`tools/fetch_ai_assets.py` で取得する（`tools/ai_assets.json` に SHA-256）。
学習のコードと教材は公開していない。モデルの重みと推論のコード（`entry/src/main/cpp/`）だけを公開する。

### モデル shuntelligence — CC BY 4.0

約630万パラメータの Transformer。学習に使った文章（モデルの重みに文章そのものは含まれない。日本の著作権法第30条の4に基づく情報解析）:

| 出典 | 条件 |
|---|---|
| おーぷん2ちゃんねる（open2ch-dialogue-corpus） | サイトが「投稿は著作権を放棄しパブリックドメイン」と明記。URL・顔文字・コピペ等を掃除し、差別語を含む投稿は除外、罵詈雑言は 1/10 に間引いた |
| GitHub（codeparrot/github-code-clean） | MIT・Apache-2.0・BSD・ISC・Unlicense・CC0 のファイルのみ |
| 青空文庫（globis-university/aozorabunko-clean） | 著作権の切れた作品のうち新字新仮名のもの |
| Wikipedia日本語版 | CC BY-SA 4.0。本文の段落のみ |

- 読み付けは SudachiPy + SudachiDict-core（Apache License 2.0）。
- 書き分けの強化の参考: 文化庁「「異字同訓」の漢字の使い分け例（報告）」（平成26年、文化審議会国語分科会。
  出典: 文化庁ホームページ。用例から語の組み合わせを取り出して加工し、同じ組み合わせを含む上記の文章を探す手掛かりに使用。
  用例の文そのものは学習に入れていない）、Wikipedia「助数詞」の一覧（CC BY-SA 4.0、名詞と助数詞の組を同様に手掛かりとして使用）、
  Wikidata（CC0、生き物の和名と分類）。
- 評価だけに使ったもの（学習には入れていない）: AJIMEE-Bench（CC BY-SA 3.0）、開発者本人の入力ログ（集計値のみ）。

### 辞書 shuntorge — 独自部分は MIT

| 部分 | 出どころ | ライセンス |
|---|---|---|
| 語・品詞のつながりのコスト | mozc OSS 辞書（`src/data/dictionary_oss`） | BSD-3-Clause / IPAdic（NAIST）ライセンス / 沖縄辞書（パブリックドメイン） |
| 記号・絵文字・顔文字 | mozc `symbol.tsv`・`emoji_data.tsv`・`emoticon.tsv` | BSD-3-Clause |
| 手入れ済みの語 | shunti IME 独自辞書のネット語・バス行き先等の節（SudachiDict の読みと一致したもの） | MIT |
| 地名 | shunti IME 地名パック（日本郵便 郵便番号データの町域名と照合） | MIT |
| ネットの読み・技術用語・【速報】等の定型 | 本プロジェクトで作成（定型の読みは SudachiDict） | MIT / SudachiDict（Apache 2.0） |
| 文章から拾った名詞 | 上記の教材に30回以上出て mozc に無い名詞（読みは SudachiDict） | MIT / SudachiDict（Apache 2.0） |
| 英字の語の読み（Win・GitHub・Excel など） | mecab-ipadic-NEologd（`mecab-user-dict-seed.20200910`。はてなキーワード一覧ファイルほかの表記と読み）。FineWeb-2 に30回以上出る英字の名詞のみ。英字を先に出すかは教材の回数で決定 | Apache License 2.0 |
| 英単語の一覧（each・other・hello など、ローマ字で英字のまま打ったときの英字の語） | SCOWL（en-wl/wordlist rel-2026.02.25、Copyright 2000-2026 Kevin Atkinson）の大きさ 35 の語 | MIT に近い許諾（著作権表示と許諾の表示を残す） |

差別語は辞書と候補から除いている。

## 未対応の推奨アクション

1. ~~Wiktionary・JMdictの帰属表示をライセンス画面に追加~~ — 完了
   （両方ともアプリ内ライセンス画面に既に記載されていたことを本セッションで
   確認・修正。track B分のJMdict利用も新規に追記済み）
2. 可能であれば、今後の語彙拡充では出典を都度このファイルに追記する運用にする

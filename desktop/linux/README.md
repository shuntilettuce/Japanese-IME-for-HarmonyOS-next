# shunti IME (Linux 版) トライアル

shuntelligence でかな漢字変換をする日本語入力です。Windows 版と同じ変換 (同じ AI・同じ入力の決まり) を、Linux の入力の仕組み **Fcitx5** の上で動かします。変換はすべてこのパソコンの中で行い、打った文字をインターネットに送ることはありません (通信する機能を持っていません)。

## 入れ方 (Ubuntu・Debian など)

[GitHub の Releases](https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases) から `fcitx5-shunti_0.4.0_amd64.deb` を入手して、端末 (ターミナル) で:

```bash
sudo apt install ./fcitx5-shunti_0.4.0_amd64.deb
```

Fcitx5 が入っていなければ一緒に入ります。Ubuntu 24.04 以降・Debian 13 以降 (64 ビットの Intel・AMD。Fcitx5 5.1 以上) で動きます。

### Fcitx5 を使うようにする (初めてのときだけ)

Ubuntu の最初の入力の仕組みは IBus なので、Fcitx5 に切り替えます。

```bash
im-config -n fcitx5
```

そのあと一度ログアウトして入り直します。すでに Fcitx5 を使っている人は、代わりに `fcitx5 -r` で Fcitx5 を読み直すだけで大丈夫です。

### shunti IME を足す

1. アプリの一覧から「Fcitx5 設定」(`fcitx5-configtool`) を開く
2. 右側の一覧で「現在の言語のみ表示」のチェックを外し、「shunti IME」を探して左の一覧へ足す
3. **Ctrl + スペース** (または 半角/全角 キー) で shunti IME に切り替える

## 使い方

- ローマ字で打つと変換の候補が出ます (Tab か ↓ で候補を選び、Enter で確定)
- スペースで変換
  - スペース・↓・↑ で候補を選ぶ、1〜9 でも選べる、Page Down / Page Up でページをめくる
  - ← → で文節を選ぶ、Shift + ← → で文節を伸び縮み
  - Enter で確定、Esc でかなに戻す
- 長く打つと、前の方から自動で確定していきます (設定で切れます)
- F6〜F10: ひらがな・カタカナ・半角カタカナ・全角英字・半角英字
- 無変換キー: 押すたびに カタカナ → 半角カタカナ → ひらがな
- 大文字で打ち始めると英字のまま (Google 等)
- 括弧は 24 種類すべて打てます。`[` で「 を打ってスペースを押すと （ ( 『 【 〘 « などが並び、「かっこ」と打つと （）「」 などの組が並びます

## 設定

「Fcitx5 設定」→ 入力メソッド で、左の一覧の shunti IME を選んで「設定」から:

- 打っている間も候補を出す
- 変換を自動で確定する
- 入力中の文字も変換して表示する (ライブ変換。最初はオフ)
- 空白を全角にする (Shift を押すと逆)
- 数字を全角にする
- 句読点 (、。 / ，． / 、． / ，。)

### ユーザー辞書

`~/.local/share/shunti-ime/userdict.json` に書きます (Linux 版には登録の画面がまだありません)。書き換えると次の変換から使われます。

```json
[
  {"r": "ぞりちゃん", "w": "ゾリちゃん", "p": "person"},
  {"r": "ぐぐる", "w": "ググる", "p": "verb"},
  {"r": "えもい", "w": "エモい", "p": "adjective"}
]
```

- `r` は読み (ひらがな)、`w` は単語
- `p` は品詞: `noun` (名詞)、`person` (人名)、`place` (地名)、`verb` (動詞、終止形で)、`adjective` (形容詞、「い」で終わる形で)
- 一段の動詞 (たべる / 食べる など) は `"g": "ichidan"` も書く

### 覚えていること

変換で選び直した語は学習して、次から先に出します。学習は `~/.local/share/shunti-ime/learned.json` に保存しています (消すと学習を忘れます)。

## 外し方

```bash
sudo apt remove fcitx5-shunti
```

## ソースからビルドする

Fcitx5 の開発用ファイルと CMake が要ります (Ubuntu なら `sudo apt install fcitx5-modules-dev libfcitx5core-dev cmake g++`)。リポジトリの一番上で:

```bash
# 辞書とモデル (entry/src/main/resources/rawfile/kkc_*.bin)
python3 tools/fetch_ai_assets.py

cmake -S desktop/linux -B build-linux -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
sudo cmake --install build-linux

# 配る .deb を作る (dpkg-dev が要る)
cd build-linux && cpack -G DEB

# 試験: Fcitx5 を画面なしで起動して、アプリと同じやり方 (DBus) でキーを送る
dbus-run-session -- python3 desktop/linux/tests/dbus_test.py
```

## ライセンス

アプリのコードは MIT、AI のモデル (shuntelligence) は CC BY 4.0、辞書 (shuntorge) は Wikipedia 日本語版と JMdict 由来の語を含むためファイル全体が CC BY-SA 4.0 です (mozc (BSD-3-Clause) なども含みます)。入れたあとは `/usr/share/doc/fcitx5-shunti/copyright` にあります。

不具合や要望は https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/issues へ。

# strings.json の番号と場所

desktop/windows の画面に出る日本語の文言。番号は strings.json と同じ (ファイルに出てくる順)。
文言を直すときは strings.json だけを書き換える。ここは「どの文言がどこで使われているか」の手引き。

| 番号 | 文言 | どこ |
| --- | --- | --- |
| 1 | 名詞 | settings_app.cpp (POS 品詞の一覧) |
| 2 | 短縮よみ | settings_app.cpp (POS) |
| 3 | 顔文字 | settings_app.cpp (POS) |
| 4 | 人名 | settings_app.cpp (POS) |
| 5 | 地名 | settings_app.cpp (POS) |
| 6 | 固有名詞・組織 | settings_app.cpp (POS) |
| 7 | 動詞 (五段: 書く・ググる) | settings_app.cpp (POS) |
| 8 | 動詞 (一段: 食べる) | settings_app.cpp (POS) |
| 9 | 形容詞 (高い・エモい) | settings_app.cpp (POS) |
| 10 | 副詞 | settings_app.cpp (POS) |
| 11 | 感動詞 | settings_app.cpp (POS) |
| 12 | 設定を保存できませんでした。 | settings_app.cpp (save・adv_save) |
| 13 | ユーザー辞書 | settings_app.cpp (メッセージの題・欄の題) |
| 14 | ファイルを開けませんでした。 | settings_app.cpp (import_words) |
| 15 | 語を登録しました。 | settings_app.cpp (import_words) |
| 16 | 登録できなかった行 ( | settings_app.cpp (import_words) |
| 17 | ことえり | settings_app.cpp (KEYMAPS) |
| 18 | QWERTY ローマ字 | settings_app.cpp (load_layouts) |
| 19 | 作: | settings_app.cpp (load_layouts) |
| 20 | 、 | settings_app.cpp (sym_shown) |
| 21 | 。 | settings_app.cpp (sym_shown) |
| 22 | ・ | settings_app.cpp (sym_shown) |
| 23 | ー | settings_app.cpp (sym_shown) |
| 24 | 「 | settings_app.cpp (sym_shown) |
| 25 | 」 | settings_app.cpp (sym_shown) |
| 26 | ￥ | settings_app.cpp (sym_shown) |
| 27 | 半 (2 字分の空白つき) | settings_app.cpp (sym_label) |
| 28 | 全 (2 字分の空白つき) | settings_app.cpp (sym_label) |
| 29 | オン | settings_app.cpp (apps_refresh) |
| 30 | オフ | settings_app.cpp (apps_refresh) |
| 31 | 無変換 | settings_app.cpp (shortcut_line) |
| 32 | 変換 | settings_app.cpp (shortcut_line・build) |
| 33 | ひらがな | settings_app.cpp (shortcut_line) |
| 34 | カタカナひらがな | settings_app.cpp (shortcut_line) |
| 35 | キー割り当てを初期状態 (選んだ型) に戻します。よろしいですか? | settings_app.cpp (shortcut_reset) |
| 36 | 書き出せませんでした (書き込めない場所かもしれません)。 | settings_app.cpp (export_words) |
| 37 | 語を書き出しました。 | settings_app.cpp (export_words) |
| 38 | 更新を確かめられませんでした。 | settings_app.cpp (check_update) |
| 39 | 最新版です ( | settings_app.cpp (check_update) |
| 40 | )。 | settings_app.cpp (check_update) |
| 41 | 新しい版 | settings_app.cpp (check_update) |
| 42 | があります (今は | settings_app.cpp (check_update) |
| 43 | )。 | settings_app.cpp (check_update) |
| 44 | ダウンロードして入れますか？ 管理者の確認が出ます。… | settings_app.cpp (check_update) |
| 45 | 「いいえ」で変更点のページを開きます。 | settings_app.cpp (check_update) |
| 46 | shunti IME の更新 | settings_app.cpp (check_update) |
| 47 | 更新できませんでした: | settings_app.cpp (check_update) |
| 48 | shunti IME の詳細設定 | settings_app.cpp (open_advanced 題) |
| 49 | 日本語入力時の記号 (クリックで変更) | settings_app.cpp (open_advanced) |
| 50 | 入力 | settings_app.cpp (open_advanced・build) |
| 51 | テンキー: | settings_app.cpp (open_advanced) |
| 52 | 常に半角 | settings_app.cpp (open_advanced) |
| 53 | 数字の全角/半角の設定に合わせる | settings_app.cpp (open_advanced) |
| 54 | 常に全角 | settings_app.cpp (open_advanced) |
| 55 | 候補の窓の数: | settings_app.cpp (open_advanced) |
| 56 | 初めて使うソフト: | settings_app.cpp (open_advanced) |
| 57 | 日本語入力をオン | settings_app.cpp (open_advanced)・langbar.cpp |
| 58 | 日本語入力をオフ | settings_app.cpp (open_advanced)・langbar.cpp |
| 59 | その他 | settings_app.cpp (open_advanced) |
| 60 | プライベート (学習機能を一時的にオフ。…) | settings_app.cpp (open_advanced) |
| 61 | ユーザー辞書を書き出す… | settings_app.cpp (open_advanced) |
| 62 | キー配列 (日本語入力がオンのとき) | settings_app.cpp (open_advanced) |
| 63 | 親指キー: | settings_app.cpp (open_advanced) |
| 64 | 無変換・変換 | settings_app.cpp (open_advanced) |
| 65 | 無変換・空白 | settings_app.cpp (open_advanced) |
| 66 | 空白・変換 | settings_app.cpp (open_advanced) |
| 67 | 連続シフト (親指を押したまま続けて打つ) | settings_app.cpp (open_advanced) |
| 68 | 親指キーを単独で押したら 本来のキー (空白・変換・無変換) | settings_app.cpp (open_advanced) |
| 69 | 押しっぱなしで繰り返す | settings_app.cpp (open_advanced) |
| 70 | その他の配列は 設定のフォルダに layout_名前.tsv を置くと ここに出ます | settings_app.cpp (open_advanced) |
| 71 | 閉じる | settings_app.cpp (open_advanced・build) |
| 72 | キー割り当て | settings_app.cpp (open_advanced) |
| 73 | 型: | settings_app.cpp (open_advanced) |
| 74 | 初期状態に戻す | settings_app.cpp (open_advanced) |
| 75 | キー: | settings_app.cpp (open_advanced) |
| 76 | 機能: | settings_app.cpp (open_advanced) |
| 77 | 追加 | settings_app.cpp (open_advanced・build) |
| 78 | 変更 | settings_app.cpp (open_advanced) |
| 79 | 削除 | settings_app.cpp (open_advanced・build) |
| 80 | 上へ | settings_app.cpp (open_advanced) |
| 81 | 下へ | settings_app.cpp (open_advanced) |
| 82 | ローマ字の表 (足した綴りは元の表より優先) | settings_app.cpp (open_advanced) |
| 83 | 綴り | settings_app.cpp (open_advanced 列) |
| 84 | かな | settings_app.cpp (open_advanced 列) |
| 85 | 綴り: | settings_app.cpp (open_advanced) |
| 86 | かな: | settings_app.cpp (open_advanced) |
| 87 | ソフトごとの日本語入力 (閉じたときの状態を覚えます) | settings_app.cpp (open_advanced) |
| 88 | ソフト | settings_app.cpp (open_advanced 列) |
| 89 | 日本語入力 | settings_app.cpp (open_advanced 列) |
| 90 | オン・オフを切り替え | settings_app.cpp (open_advanced) |
| 91 | 一覧から消す | settings_app.cpp (open_advanced) |
| 92 | 変換で選び直して覚えた語を、すべて忘れます。よろしいですか? | settings_app.cpp (reset_learning) |
| 93 | 学習をリセット | settings_app.cpp (reset_learning・build) |
| 94 | 学習をリセットしました。 | settings_app.cpp (reset_learning) |
| 95 | 表示 | settings_app.cpp (build) |
| 96 | 候補ウィンドウの色: | settings_app.cpp (build) |
| 97 | Windows の設定に合わせる | settings_app.cpp (build) |
| 98 | ライト | settings_app.cpp (build) |
| 99 | ダーク | settings_app.cpp (build) |
| 100 | アイコンの色をタスクバーに合わせる | settings_app.cpp (build) |
| 101 | 変換モデル: | settings_app.cpp (build) |
| 102 | 軽量 (XS5) | settings_app.cpp (build) |
| 103 | 標準 (S7)　推奨 | settings_app.cpp (build) |
| 104 | 句読点: | settings_app.cpp (build) |
| 105 | 、。 | settings_app.cpp (build) |
| 106 | ，． | settings_app.cpp (build) |
| 107 | 、． | settings_app.cpp (build) |
| 108 | ，。 | settings_app.cpp (build) |
| 109 | 何も入力していないときのスペースを全角にする | settings_app.cpp (build) |
| 110 | 数字を全角で入力する | settings_app.cpp (build) |
| 111 | 入力中も変換候補を表示する | settings_app.cpp (build) |
| 112 | 入力中の文字も変換して表示する (ライブ変換) | settings_app.cpp (build) |
| 113 | 変換演算を直近の文字にのみ適用 (より軽量) | settings_app.cpp (build) |
| 114 | 提案 (打ち間違いの直し・日付・絵文字など) を候補と分けて表示する | settings_app.cpp (build) |
| 115 | 読み | settings_app.cpp (build 列) |
| 116 | 単語 | settings_app.cpp (build 列) |
| 117 | 品詞 | settings_app.cpp (build 列) |
| 118 | 読み: | settings_app.cpp (build) |
| 119 | 単語: | settings_app.cpp (build) |
| 120 | 品詞: | settings_app.cpp (build) |
| 121 | ファイルから辞書を登録… | settings_app.cpp (build) |
| 122 | 動詞・形容詞は終止形で MS-IME の一覧 (.txt) も登録可能 | settings_app.cpp (build) |
| 123 | 学習 | settings_app.cpp (build) |
| 124 | 詳細設定… | settings_app.cpp (build) |
| 125 | 更新を確認 | settings_app.cpp (build) |
| 126 | (デバッグ版: 入力を記録しています) | settings_app.cpp (build・デバッグ版のみ) |
| 127 | ライセンス | settings_app.cpp (build) |
| 128 | アイコンの色を合わせました。… | settings_app.cpp (proc ID_ICON) |
| 129 | shunti IME の設定 | settings_app.cpp (wWinMain 題) |
| 130 | 続き Tab | text_service.cpp (update_candidates) |
| 131 | 入力モード | langbar.cpp (GetInfo) |
| 132 | ひらがな (shunti IME) | langbar.cpp (GetTooltipString) |
| 133 | 半角英数 (shunti IME) | langbar.cpp (GetTooltipString) |
| 134 | 設定・ユーザー辞書... | langbar.cpp (OnClick・InitMenu) |
| 135 | あ | langbar.cpp (GetText) |
| 136 | 提案 Shift+↓ | cand_window.cpp (sugg_label) |
| 137 | 提案 | cand_window.cpp (sugg_label) |
| 138 | ←→ で選ぶ  Enter で確定  Esc で戻る | cand_window.cpp (foot_text) |
| 139 | Tab で選ぶ | cand_window.cpp (foot_text) |
| 140 | 件 | cand_window.cpp (foot_text) |
| 141 | shunti IME のインストール | setup.cpp (wWinMain 題) |
| 142 | 最新版を調べられませんでした。 | setup.cpp (wWinMain) |
| 143 | ネットにつながっているか確かめてください。… | setup.cpp (wWinMain) |
| 144 | (Windows 版) を入れます。 | setup.cpp (wWinMain) |
| 145 | ダウンロード (約 40MB) のあと、管理者の確認が出ます。… | setup.cpp (wWinMain) |
| 146 | 続けますか？ | setup.cpp (wWinMain) |
| 147 | 入れられませんでした: | setup.cpp (wWinMain) |
| 148 | URL を読めません | update.cpp (http_get) |
| 149 | ネットにつなげません | update.cpp (http_get) |
| 150 | サーバーの返事が | update.cpp (http_get) |
| 151 | でした | update.cpp (http_get) |
| 152 | ネットにつなげません ( | update.cpp (http_get) |
| 153 | Windows 版の Release が見つかりません | update.cpp (find_latest) |
| 154 | 一時ファイルを書けません | update.cpp (download_and_install) |
| 155 | 展開できません | update.cpp (download_and_install) |
| 156 | zip の中に install.ps1 がありません | update.cpp (download_and_install) |
| 157 | 入れる処理を起動できません | update.cpp (download_and_install) |

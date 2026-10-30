// 端末で動くかな漢字変換エンジン (網 + 採点器 + 上位 k)。外部ライブラリなしの C++17。
// shuntelligence の推論エンジン。学習側 (非公開) と同じ計算をし、候補の並びが学習側と一致することを確かめてある。
// Android では JNI、HarmonyOS では NAPI (napi_kkc.cpp) から呼ぶ。
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define KKC_API extern "C" __declspec(dllexport)
#else
#define KKC_API extern "C" __attribute__((visibility("default")))
#endif

struct kkc_engine;

// 辞書 (shuntorge, kkc_lex.bin) とモデル (shuntelligence, kkc_model.bin) の中身。呼び出し側が mmap したものをそのまま渡す (エンジンは複製しない)。
KKC_API kkc_engine* kkc_open(const void* lex, size_t lex_size, const void* model, size_t model_size);
// PC 用: ファイルを読み込んで開く
KKC_API kkc_engine* kkc_open_files(const char* lex_path, const char* model_path);
KKC_API void kkc_close(kkc_engine* e);
KKC_API void kkc_set_threads(kkc_engine* e, int n);
KKC_API void kkc_set_typo(kkc_engine* e, int on, float bias);
// 提案の種類 (kkc_last_suggest)
enum { KKC_SUGGEST_TYPO = 1, KKC_SUGGEST_DATE = 2, KKC_SUGGEST_ADDRESS = 3, KKC_SUGGEST_ENGLISH = 4, KKC_SUGGEST_ALT = 5, KKC_SUGGEST_MOOD = 6, KKC_SUGGEST_SYMBOL = 7, KKC_SUGGEST_TYPO_TOP = 8, KKC_SUGGEST_AFTER = 9 };
// 提案: オンにすると、変換の域を超えるもの (打ち間違いの直し・日付・郵便番号の住所) を候補に混ぜず kkc_last_suggest に出す
KKC_API void kkc_set_suggest(kkc_engine* e, int on);
// 豆腐になる字を含む候補を除外する (1 = 変体仮名、2 = ヒエログリフ、4 = 麻雀牌など。既定 7)。端末側で字形の有無を調べるなら 0 に
KKC_API void kkc_set_char_filter(kkc_engine* e, int mask);
KKC_API void kkc_set_latin(kkc_engine* e, int on);
KKC_API int kkc_latin_word(kkc_engine* e, const uint16_t* key, int nk, uint16_t* out, int cap);   // 綴り -> 辞書の英字の語の表記   // 英字の打ち分け (ローマ字で打った英単語の辺)。既定はオン
KKC_API void kkc_set_typo_promote(kkc_engine* e, double per_char);   // 打ち間違いの直しを 1 位にする差 (1 字あたり)
KKC_API double kkc_last_typo_margin(kkc_engine* e);
KKC_API int kkc_last_suggest(kkc_engine* e, uint16_t* out, int cap, int32_t* kinds, int kcap);   // 打ち間違いの直し (S7〜): on/off と控えめにする値

// 変換。ctx = 左の文脈 (確定済みの文)、kana = 読み (UTF-16)。上位 maxout 個の表記を 0 区切りの UTF-16 で out に書く。
// 返り値は候補の数 (out が足りなければ -1)。use_model = 0 なら辞書のコストだけ (採点器を通さない)。
// use_model = 2 なら段階式: まず辞書だけで変換し、辞書が迷っている (1 位と 2 位のコストの差が小さい) ときだけ採点器を通す。
KKC_API int kkc_convert(kkc_engine* e, const uint16_t* ctx, int nctx, const uint16_t* kana, int nk,
                        int maxout, int use_model, uint16_t* out, int cap);

// 打ったキーの並び (ローマ字の英字) を渡す。英字のまま打った語 (github) を候補にできるようにする。
// keys = 打ったキー (英字・記号そのまま)。span_end[i] = 読みの i 文字目がキーの何文字目までで決まったか (単調増加)。
// 直後の 1 回の kkc_convert にだけ効く (その変換の読みの長さが nk と合わなければ使わない)
KKC_API void kkc_set_keys(kkc_engine* e, const uint16_t* keys, int nkeys, const int32_t* span_end, int nk);
// カーソルの後ろの文 (後ろの文脈、80 字ほど)。直後の 1 回の kkc_convert にだけ効く。後ろの文脈を学んだモデル (S8 から) だけが使う
KKC_API void kkc_set_right_ctx(kkc_engine* e, const uint16_t* text, int n);
// モデルが後ろの文脈を学んでいるか (S8 から 1)。使わないモデルでは後ろの文脈を作る手間を省ける
KKC_API int kkc_has_right_ctx(kkc_engine* e);

// 直前の kkc_convert の 1 位と 2 位 (表記が違う最初の 2 つ) の経路のコストの差 (大きいほど迷いが少ない)。
// 段階式 (辞書だけで決まるときはモデルを呼ばない) の判定に使う。2 位が無ければとても大きな値
KKC_API double kkc_last_margin(kkc_engine* e);
// 直前の kkc_convert の時間の内訳 (ミリ秒): [網, 下書き, エンコーダ, 区間と語, 上位 k]
KKC_API void kkc_last_times(kkc_engine* e, double* t5);

// 検証用: 直前の kkc_convert の網の辺ごとの点数 u (辺の順は学習側と同じ)。返り値は辺の数
KKC_API int kkc_last_scores(kkc_engine* e, float* u, int cap);

// 直前の kkc_convert の 1 位の候補の語の区切り。ends[i] = i 語目の読みの終わりの位置 (読みの UTF-16 の字数)、
// lens[i] = i 語目の表記の長さ (UTF-16)。返り値は語の数。長い入力で「前の方を固定して後ろだけ変換する」ために使う
KKC_API int kkc_last_segments(kkc_engine* e, int32_t* ends, int32_t* lens, int cap);

// 直前の kkc_convert の上位の経路 (候補の木の窓用)。経路 p のコスト costs[p]・語の数 counts[p]、語ごとに読みの終わり ends と
// 表記の長さ lens (UTF-16)、表記は text に続けて書く。返り値は経路の数。kkc_convert のすぐ後に同じスレッドで呼ぶ
KKC_API int kkc_last_paths(kkc_engine* e, int pmax, double* costs, int32_t* counts, int32_t* ends, int32_t* lens, int wcap,
                           uint16_t* text, int tcap);

// 直前の kkc_convert の 1 位の候補の語ごとの品詞の左右 ID (辞書の ID。辞書に無い語は名詞の ID)。返り値は語の数。
// PC 版が文節 (自立語 + 付属語) の区切りを決めるのに使う
KKC_API int kkc_last_segment_pos(kkc_engine* e, int32_t* lids, int32_t* rids, int cap);

// ユーザー辞書。登録した語を辞書の語と同じく網に入れ、モデルが文脈で採点する (読みが文の途中に出てきても候補になる)。
// 品詞の左右 ID とコストは、辞書にある「同じ品詞・同じ活用の形の代表語」(読み tr・表記 ts) から写す
// (例: 登録語 ぐぐら/ググら の代表に はしら/走ら)。コストは代表語より bonus だけ下げる (選ばれやすくする)。
// 代表語が辞書に無ければ足さずに 0 を返す。kkc_convert と同じスレッドで呼ぶ
KKC_API void kkc_user_clear(kkc_engine* e);
// M5 の個人化 (版 6 のモデルのとき効く)。書き方の対の表 (UTF-16 の TSV: かな 	 漢字 	 見出し 	 種類)。返り値は表記の数
KKC_API int kkc_set_variants(kkc_engine* e, const uint16_t* tsv, int n);
// モデルが M5 の個人化 (書き方の印) を知っているか
KKC_API int kkc_personal(kkc_engine* e);
// 直前の変換で、打ち間違いの直しの道の直した辺の読みの区間 (始め, 終わり) × 返り値の数
KKC_API int kkc_typo_spans(kkc_engine* e, int32_t* out, int cap);
// 確定した文を読みの網の上でたどり、通った語のうち書き方の対の表にある表記を 0 区切りで out に (書き方の記録に数える語)
KKC_API int kkc_path_variants(kkc_engine* e, const uint16_t* reading, int nr, const uint16_t* text, int nt, uint16_t* out, int cap);
// 表記が書き方の対の表にあれば out3 = (見出しの番号, 種類, 漢字か) で 1、無ければ 0
KKC_API int kkc_variant_info(kkc_engine* e, const uint16_t* s, int n, int32_t* out3);
// 表記ごとの使う人の印 (1〜24 = 回数の段階 × 時期。表記を 0 区切りで並べたもの と それぞれの印)
KKC_API void kkc_set_marks(kkc_engine* e, const uint16_t* surfs, int n, const int32_t* marks, int nm);
// 書き方の記録: fam = (見出しの番号, 漢字か, 回数) × nf、cls = 種類 0〜7 ごとの (かな, 漢字) の回数 (16 個)
KKC_API void kkc_set_style(kkc_engine* e, const int32_t* fam, int nf, const int32_t* cls);
// kkc_user_add_like と同じ。flags & 1 = 読み全体が一致したときだけ置く (短縮よみ)
KKC_API int kkc_user_add_like2(kkc_engine* e, const uint16_t* r, int nr, const uint16_t* s, int ns, const uint16_t* tr, int ntr,
                               const uint16_t* ts, int nts, int bonus, int flags);
KKC_API int kkc_user_add_like(kkc_engine* e, const uint16_t* r, int nr, const uint16_t* s, int ns, const uint16_t* tr, int ntr,
                              const uint16_t* ts, int nts, int bonus);

// 予測: 読みが prefix で始まり、prefix より長い (長くても max_extra 字まで) 辞書の語の表記を、コストの低い (よく使う) 順に
// 最大 maxout 個、0 区切りの UTF-16 で out に書く (こん → こんにちは・こんばんは・今度…)。返り値は数 (out が足りなければ -1)。
// 辞書を読むだけなので、変換中の別のスレッドから呼んでもよい
KKC_API int kkc_complete(kkc_engine* e, const uint16_t* prefix, int np, int max_extra, int maxout, uint16_t* out, int cap);

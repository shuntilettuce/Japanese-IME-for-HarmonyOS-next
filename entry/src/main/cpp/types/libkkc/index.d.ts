// libkkc.so (napi_kkc.cpp) の型。entry/src/main/cpp/types/libkkc/index.d.ts に置く
import resourceManager from '@ohos.resourceManager';

/** 辞書とモデルを開く (裏のスレッドで)。rawfile の kkc_lex.bin / kkc_model.bin を HAP の中のまま mmap する。
 * dir は 1.6.1 までの写し (filesDir に写していたもの) を消すためだけに使う */
export const loadAsync: (resMgr: resourceManager.ResourceManager, dir: string, threads: number) => Promise<boolean>;
/** 変換の結果: 候補と、1 位の候補の語の区切り (ends[i] = i 語目の読みの終わり、lens[i] = 表記の長さ) */
export interface KkcResult {
  cands: string[];
  ends: number[];
  lens: number[];
  /** 上位の経路 (表示しない。確定した語を学習するのに使う)。1 本ごとに "読みの終わり\u0001表記\u0001…"。convertAsync のときだけ */
  paths?: string[];
  /** 待ち時間の内訳 (ms): 裏のスレッドの順番待ち / 変換 / 変換が終わってから JS に届くまで */
  waitMs: number;
  runMs: number;
  deliverMs: number;
  /** 提案 (変換の域を超えるもの: 打ち間違いの直し・日付・郵便番号の住所)。convertAsync のときだけ。候補には入らない */
  sugg?: string[];
  /** 提案の種類: 1 = 打ち間違いの直し、2 = 日付・時刻、3 = 住所 */
  suggKinds?: number[];
}
/** 変換 (呼んだスレッドで。時間の内訳は 0)。文節を選ぶとき・語を作れるかを確かめるときに */
export const convert: (ctx: string, kana: string, max: number) => KkcResult;
/** 変換 (裏のスレッドで。UI を止めない) */
export const convertAsync: (ctx: string, kana: string, max: number) => Promise<KkcResult>;
/** 予測: 読みが prefix で始まり、あと maxExtra 字までの辞書の語を、よく使う順に最大 maxOut 個 */
export const complete: (prefix: string, maxExtra: number, maxOut: number) => string[];
/** ユーザー辞書を入れ直す。forms は [読み, 表記, 代表語の読み, 代表語の表記] を 4 つずつ並べたもの。入った形の数を返す */
export const setUserWords: (forms: string[], bonus: number) => number;
/** 直前の変換の時間の内訳 (ms): [網, 下書き, エンコーダ, 区間と語, 上位k] */
export const times: () => number[];
/** 提案を分けて出すか (オフなら日付・住所・打ち間違いの直しを候補に混ぜる) */
export const setSuggest: (on: boolean) => boolean;
/** AI学習 (設定、既定オフ) のオン・オフ。path = 育てた中身の保存先 (空ならオフ)。同じ保存先なら、学習のリセットで消されたかだけを見る */
export const setAiLearn: (path: string) => boolean;
/** 確定した文 (左の文脈・読み・表記) から 1 歩学んで保存する (裏のスレッドで)。学ぶ前の損失 (学ばなかったら負) */
export const aiLearnAsync: (ctx: string, reading: string, text: string) => Promise<number>;
/** AI学習で育てた中身を消す (学習のリセット。オフでも消す)。使っていれば初めの値に戻す */
export const aiForget: (path: string) => boolean;
export const close: () => boolean;

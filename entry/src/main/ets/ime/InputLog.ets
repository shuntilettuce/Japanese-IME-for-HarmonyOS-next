// ===========================================================================
//  ★ デバッグビルド専用・入力ログ収集 / DEBUG-BUILD-ONLY INPUT LOG ★
// ===========================================================================
//
// 変換の精度・学習の効き具合を、この端末の持ち主が実際に打った入力から測る
// ための収集機構。実文コーパス(Tatoeba)は「どこかの誰かの日本語」で、個人最適化
// (短期記憶・未知語学習・次単語予測) の効果は本人の繰り返しにしか現れないので、
// 実入力の記録でしか測れない。
//
// ---------------------------------------------------------------------------
//  リリースビルドでは動かない
// ---------------------------------------------------------------------------
//  スイッチは手で書き換える定数ではなく、hvigor がビルドのたびに生成する
//  BuildProfile.DEBUG をそのまま使う。
//    - デバッグビルド (assembleHap -p buildMode=debug) … 記録する
//    - リリースビルド (assembleApp -p buildMode=release) … 全 record*() が即 return
//  1.5.3 までは定数を手で false にするか、`// [DEBUG-LOG]` を付けた行を
//  リリース前に全部剥がす運用だった。剥がすたびに収集ごと失ううえ、戻すのも
//  手作業になるので、ビルド種別に連動させてコードは残す形に変えた。
//
// ---------------------------------------------------------------------------
//  記録されるもの / されないもの
// ---------------------------------------------------------------------------
//  記録する: 打鍵 (ローマ字キー・フリックのかな・濁点等の切替) / 変換中の候補の
//            移り変わり / 確定 (候補・かな・文節・予測) と選んだ番号・候補一覧 /
//            矢印キーでの変換範囲の伸縮・文節の移動 / Space・Enter / 文字の削除・
//            取消 / 候補の削除 / 未知語学習・準候補 / 入力モード・片手・
//            フローティングの切替 / 実際にホストへ挿入された文字 / 入力欄の切替 /
//            アプリバージョンとエンジン (起動毎)
//            候補の出どころ (どの辞書の何番目か: KanaKanjiConverter.explainSources) /
//            変換中の自動の文節区切り / フリックの方向と指の移動量 / 候補バーの
//            スクロール / 長押しメニュー / パネル切替 / 設定の変更
//  記録しない: パスワード等の secure field の中身 (SECURE_PATTERNS)。
//            クリップボードの中身 (貼り付けた事実と文字数だけ残す)。
//
//  保存先は端末内の拡張プロセスのサンドボックス 1 ファイルのみ。送信は一切
//  しない。取り出しは USB デバッグ経由 (tools/pull_input_log.js)。
//
//  ★ 収集したログを git に入れないこと (リポジトリは public)。
//     取り出し先 tools/.inputlog/ と *.jsonl は .gitignore 済み。
// ===========================================================================

import fs from '@ohos.file.fs';
import util from '@ohos.util';
import zlib from '@ohos.zlib';
import BuildProfile from 'BuildProfile';

export const DEBUG_INPUT_LOG: boolean = BuildProfile.DEBUG;

// アプリサンドボックス内のログ本体。1行1レコードの JSONL にしているのは、
// 追記だけで書けて壊れにくく、途中で切れても前半が読めるため。
export const INPUT_LOG_FILE: string = 'debug_input_log.jsonl';

// レコード種別。取り出した JSONL をあとから分類するためのキーなので、
// 一度決めたら変えない (古いログと混ざると集計できなくなる)。
// ---- 1.5.3 以前から ----
export const LK_COMMIT: string = 'commit';      // 候補を確定した
export const LK_RAW: string = 'raw';            // 変換せずかなのまま確定した
export const LK_RANGE: string = 'range';        // 変換範囲 (◀▶) を変えた
export const LK_BUNSETSU: string = 'bunsetsu';  // 文節モードに入った (区切り位置)
export const LK_BSEG: string = 'bseg';          // 文節ごとに候補を選んだ
export const LK_DELCAND: string = 'delcand';    // 候補を削除した (長押し→削除)
export const LK_UNKNOWN: string = 'unknown';    // 未知語として学習された
export const LK_DELCHAR: string = 'delchar';    // 文字を消した (⌫)
export const LK_UNDO: string = 'undo';          // 取消で消した文字を戻した
export const LK_FIELD: string = 'field';        // 入力欄が変わった (本文なし)
export const LK_DIAG: string = 'diag';          // 拡張プロセスの自己申告 (本文なし)
export const LK_BSEG_FINAL: string = 'bsegfinal'; // 文節モードの最終確定
export const LK_QUASI_PICK: string = 'quasipick'; // 準候補をタップして確定した
export const LK_KNOWN_ASSEMBLY: string = 'knownassembly'; // 組み立てた語が実は辞書に埋もれていた
// ---- 1.5.4 で追加 (「ありとあらゆる記録」) ----
export const LK_KEY: string = 'key';            // ローマ字キーを押した (r=キー, s=確定したかな, x=保留ローマ字)
export const LK_KANA: string = 'kana';          // フリック等でかなを直接入れた
export const LK_VARIANT: string = 'variant';    // 濁点/半濁点/小文字の切替 (x=種別)
export const LK_UPDATE: string = 'upd';         // 変換中に候補が更新された (r=読み, c=候補, x=変換範囲)
export const LK_BFOCUS: string = 'bfocus';      // 文節モードで注目する文節を移した (x=from->to)
export const LK_BCANCEL: string = 'bcancel';    // 文節モードを⌫で抜けて入力中に戻った
export const LK_CURSOR: string = 'cursor';      // 入力中でないときの矢印キー (ホストのカーソル移動)
export const LK_SPACE: string = 'space';        // Space (x=状態, 候補送りなら i/n)
export const LK_ENTER: string = 'enter';        // Enter (x=状態)
export const LK_INSERT: string = 'ins';         // ホストへ実際に挿入した文字列 (x=経路)
export const LK_PRED: string = 'pred';          // 次単語予測のストリップが出た (c=予測, x=文脈)
export const LK_PRED_PICK: string = 'predpick'; // 予測をタップした
export const LK_PASTE: string = 'paste';        // 貼り付けた (本文は記録しない。i=文字数, x=経路)
export const LK_MODE: string = 'mode';          // 入力モード等の切替 (x=内容)
export const LK_BOUNDARY: string = 'boundary';  // 文の区切り (全角スペース・Enter)
export const LK_ABANDON: string = 'abandon';    // 入力中の読みを確定せず全部消した
export const LK_EXPAND: string = 'expand';      // 候補一覧 (▼) を開いた/選ばずに閉じた (x=open/close)
// ---- 独自辞書を育てるための詳細 (2026-09-17 追加) ----
export const LK_FLICK: string = 'flick';        // フリックの詳細 (r=キーの中央, s=出た文字, x=方向 dx,dy)
export const LK_SCROLL: string = 'scroll';      // 候補バーを横にスクロールした (i=止まった位置vp, n=候補数)
export const LK_MENU: string = 'menu';          // 候補の長押しメニュー (x=open/cancel, i=候補番号)
export const LK_SUBMODE: string = 'submode';    // 記号・絵文字・設定などのパネル切替 (x=パネル名)
export const LK_SETTING: string = 'setting';    // 設定の変更 (r=項目, s=値)
export const LK_PERF: string = 'perf';       // 速度の集計 (r=対象, x=件数と ms)
export const LK_BRACKET: string = 'bracket';    // 閉じ括弧の保留を解いた/捨てた (s=閉じ括弧, x=きっかけ)

// 確定の性質を表す補助タグ (entry.x に入る)。
export const LF_KATAKANA: string = 'kata';      // 読みのカタカナ形をそのまま確定
export const LF_ALNUM: string = 'alnum';        // 英数に変換して確定
export const LF_KANA: string = 'kana';          // 読みと同じ (かな) を候補から確定

// EditorAttribute.inputPattern のうち収集対象外にするもの。パスワード系は
// 変換の研究データとして何の価値もない一方、取り出せるファイルに平文で残ると
// 事故にしかならないので常に除外する。
// (値は ArkUI の InputType と同じ並び。未知の値なら通常フィールド扱い=記録する)
const SECURE_PATTERNS: number[] = [7, 8, 9, 11];

// メモリに溜めてからまとめて書く。打鍵のたびに open/write/close すると
// 応答に乗ってくるため。どちらか早い方で flush する。打鍵単位の記録が入って
// 件数が増えたので、1.5.3 以前 (4件/800ms) より溜める件数を増やしてある。
// 間隔を短めにしているのは、拡張プロセスが onDestroy を経ずに落とされたときの
// 取りこぼしを抑えるため。
const FLUSH_AT: number = 48;
const FLUSH_MS: number = 2500;

// 上限チェック (statSync) を flush のたびに走らせない。1回の flush で増えるのは
// 数KBなので、この回数ごとに見れば十分。
const TRIM_CHECK_EVERY: number = 64;

// 速度の集計をログへ吐く間隔 (サンプル数)。1件ずつ残すと件数が倍増するので
// まとめて 1 行にする。
const PERF_EVERY: number = 100;

// 属性が届くまでの保留の上限。
const MAX_PENDING: number = 128;

// 同じフィールド属性の記録を抑える間隔 (onInputStart の二重発火対策)。
const FIELD_DEDUPE_MS: number = 1000;

// ログ本体の上限。超えたら古い方を捨てる (消えるのは古い行だけで、収集は
// 止まらない)。候補の全件・出どころまで残すようにして1打鍵あたりが重く
// なったぶん上限を上げてある。書き出しは zlib で圧縮してから流すので
// (テキストなので数分の1になる)、この大きさでも USB への書き出しは
// 1分前後で済む。
const MAX_BYTES: number = 24 * 1024 * 1024;

// 確定 (commit/bseg) に添える候補一覧の上限。「20番目にあった」を見るには
// 全件が要るので実質無制限にしてある (異常に長い一覧への保険だけ)。
const CAND_LIMIT_PICK: number = 300;
// 変換中の候補更新 (打鍵ごと) に添える上限。こちらは打鍵のたびに書くので
// 重くしすぎない。
const CAND_LIMIT_UPDATE: number = 40;

// ---- USB への書き出し (hilog) -------------------------------------------
// 拡張プロセスからは、クリップボードの setData は常に失敗し (SELinux で
// pasteboard サービスへのアクセスが拒否される)、hdc からサンドボックスも
// 読めない。サンドボックスの外から確実に読めるのは hilog だけなので、
// ファイルの中身を Base64 にして hilog へ流し、PC 側で拾って復元する。
export const DUMP_MARKER: string = 'SHUNTILOGDUMP';
// hilog の1行に収まる長さ。Base64 なので区切り位置は文字数だけの問題になる。
const DUMP_CHUNK: number = 1500;
// まとめて書くと hilog 側のバースト制御で静かに落ちる (1.5.3 以前に実測:
// 一度に書いた733行中100行前後が毎回ほぼ同じ範囲で欠けた)。少数ずつ間隔を
// 空けて書く。欠けた場合は PC 側が連番の抜けとして検出する。
const DUMP_BATCH: number = 6;
const DUMP_INTERVAL_MS: number = 100;

// 1レコード。キーを1文字にしてあるのは、JSONL ではキー名がレコード数ぶん
// 繰り返されてファイルの大半がキー名になるため。
export interface InputLogEntry {
  t: number;   // Date.now()
  k: string;   // 種別 (LK_*)
  r: string;   // 読み / 対象のかな
  s: string;   // 確定した表記 / 対象の文字列
  i: number;   // 選んだ候補の番号 (0始まり, 該当なしは -1)
  n: number;   // そのとき出ていた候補の総数 (該当なしは 0)
  x: string;   // 補助情報
  c?: string;  // その場に出ていた候補 (',' 区切り)
  // 候補の出どころ (',' 区切り、c と同じ並び)。確定では「選んだ候補と、
  // それより上に並んでいた候補」の分だけ入る (= 何に負けて下にいたか)。
  p?: string;
  g?: string;  // 変換中の自動の文節区切り ('|' 区切り)
  // 候補をどの仕組みが列に入れたか (',' 区切り、c と同じ並び)。p が「どの
  // 辞書に載っているか」なのに対し、こちらは「どの段が入れたか」:
  //   look=読み全体の辞書引き / whole=複数文節だが全体で辞書に当たった /
  //   join=文節ごとに変換して連結 / alt=文節ごとの同音語差し替え /
  //   reseg=区切りを変えた別解 / phrase=定型句補完 / unk=未知語学習 /
  //   date=日付時刻 / pred=予測補完 / num=数字整形 / quasi=準候補
  // 末尾の ^C は文脈学習、^L は選択学習で先頭に引き上げられた印。
  o?: string;
  // 変換中の文節分割の内訳 (KanaKanjiConverter.segDiagFor の形式)。
  // 分割の自信 (次善との差) と、辞書に無い1文字断片 (u) の有無を見る。
  q?: string;
}

export class InputLog {
  private static buffer: InputLogEntry[] = [];
  private static filesDir: string = '';
  private static timer: number = -1;
  // 現在のテキストフィールドが secure か。
  private static secureField: boolean = false;
  // フィールドの属性がまだ届いていない (getEditorAttribute は非同期)。その間の
  // 記録は捨てずに保留し、属性が届いた時点で secure なら破棄・そうでなければ
  // 採用する (1.5.3 以前の最初の実装は捨てていて、属性が解決しない欄では
  // 「1件も記録されない」のに画面上は「空」としか見えなかった)。
  private static fieldPending: boolean = false;
  private static pending: InputLogEntry[] = [];
  private static pendingDropped: number = 0;
  private static lastFieldPattern: number = -9999;
  private static lastFieldAt: number = 0;
  // 直前の候補更新。同じ読み・同じ候補の更新が続くと (保留ローマ字だけが
  // 変わった打鍵など) 同じ行が並ぶだけなので記録しない。
  private static lastUpdateKey: string = '';
  // 直前に出した候補 (recordUpdate)。消し始め・全部消したときに残す
  private static lastReading: string = '';
  private static lastCands: string[] = [];
  private static inDeleteRun: boolean = false;
  private static flushCount: number = 0;
  // 速度の集計 (notePerf)。対象ごとに件数・合計・最大を持つだけ。
  private static perfName: string = '';
  private static perfCount: number = 0;
  private static perfSum: number = 0;
  private static perfMax: number = 0;
  private static perfSlow: number = 0;
  // 直近の候補更新で、各候補を入れた段 (o 欄) と文節分割の内訳 (q 欄)。
  // 確定の記録にも同じものを付けるために持っておく。
  private static lastOrigins: Map<string, string> = new Map<string, string>();
  private static lastOriginsReading: string = '';
  private static lastSegDiag: string = '';
  // 書き出し中か (二重押しで同じ内容を2回流さない)。
  private static dumping: boolean = false;

  // 候補を順に「実際に開いて書けるか」試し、最初に成功したものを使う。
  // パス文字列が取れることと、拡張プロセスのサンドボックスがそこに書けることは
  // 別 (flush() の失敗は握り潰すので、仮定すると「0件」としか見えなくなる)。
  static setWritableDir(dirs: string[]): string {
    if (!DEBUG_INPUT_LOG) { return ''; }
    for (const d of dirs) {
      if (d === undefined || d === null || d.length === 0) { continue; }
      try {
        const file = fs.openSync(d + '/' + INPUT_LOG_FILE,
          fs.OpenMode.WRITE_ONLY | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
        fs.closeSync(file);
        InputLog.filesDir = d;
        return d;
      } catch (_e) {
        // 次の候補へ
      }
    }
    InputLog.filesDir = '';
    return '';
  }

  static path(): string {
    return InputLog.filesDir + '/' + INPUT_LOG_FILE;
  }

  // フィールドが変わった。属性が届くまでの記録は保留にする。
  static beginField(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    // 前のフィールドの属性が最後まで来なかった場合、保留分は判断できないので
    // 捨てる (secure かもしれないため)。黙って捨てず件数を残す。
    if (InputLog.pending.length > 0) {
      InputLog.pendingDropped += InputLog.pending.length;
      InputLog.pending = [];
      InputLog.write(LK_FIELD, '', '', -1, 0, `属性が届かず破棄 ${InputLog.pendingDropped}件`);
    }
    InputLog.flush();
    InputLog.fieldPending = true;
    InputLog.secureField = false;
    InputLog.lastUpdateKey = '';
  }

  // フィールドの属性が届いた。保留分をここで採否する。
  static setFieldPattern(pattern: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    const prevPattern = InputLog.lastFieldPattern;
    const prevAt = InputLog.lastFieldAt;
    InputLog.lastFieldPattern = pattern;
    InputLog.lastFieldAt = Date.now();
    InputLog.secureField = SECURE_PATTERNS.indexOf(pattern) >= 0;
    InputLog.fieldPending = false;
    const held = InputLog.pending;
    InputLog.pending = [];
    if (!InputLog.secureField) {
      for (const e of held) { InputLog.buffer.push(e); }
    }
    if (pattern === prevPattern && InputLog.lastFieldAt - prevAt < FIELD_DEDUPE_MS) { return; }
    InputLog.write(LK_FIELD, '', '', -1, 0,
      `pattern=${pattern}${InputLog.secureField ? ' (secure: 記録しない)' : ''}`);
  }

  // 拡張プロセスが「自分はどこに書いているつもりか」を1件残す。
  static recordPaths(source: string, logDir: string, filesDir: string, prefsDir: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.write(LK_DIAG, '', '', -1, 0,
      `log=${logDir} 起動元=${source} files=${filesDir} prefs=${prefsDir}`);
  }

  // 拡張プロセス起動のたびに1件。どの記録がどのバージョン/エンジンの下で
  // 残ったものかを後から付き合わせられるようにする。
  static recordVersion(appVersion: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.write(LK_DIAG, '', '', -1, 0,
      `ver=${appVersion} engine=ai`);
  }

  // パネル (KeyboardPage) からの保険。本来の初期化は setContext 経由だが、
  // それが走らない状態が実機で出たことがある。既に初期化済みなら何もしない。
  static ensureFromPanel(dirs: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (InputLog.filesDir.length > 0) { return; }
    const used = InputLog.setWritableDir(dirs);
    InputLog.recordPaths('panel(setContext未到達)',
      used.length > 0 ? used : '書き込めるディレクトリ無し', dirs.join(' / '), '');
  }

  // 永続化ストアの初期化/保存に失敗した。これが起きるとキーボードは普通に
  // 動くのに保存だけが全部死ぬので、理由を残す (preferences に依存しない経路)。
  static recordInit(store: string, e: ESObject): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.write(LK_DIAG, '', store, -1, 0, '初期化に失敗: ' + JSON.stringify(e));
  }

  // ---- 記録 API -----------------------------------------------------------

  // via: どこから選んだか。bar=候補バーをタップ / list=▼で開いた一覧をタップ /
  // key=キー操作での確定 (Enter・次の打鍵での先頭確定など)。基本はタップで
  // 選ぶ使い方なので、「バーに出ていたか、一覧を開かないと無かったか」を
  // 区別できないと、候補の並びの良し悪しが測れない。
  // x は "分類 via=..." (分類が無ければ " via=..." で始まる)。分類を先頭に
  // 残しているのは 1.5.3 以前のログと集計を揃えるため。
  static recordCommit(reading: string, surface: string, index: number, total: number,
      candidates: string[], via: string, provenance: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    const fromUpdate = reading === InputLog.lastOriginsReading;
    InputLog.push(LK_COMMIT, reading, surface, index, total,
      `${InputLog.classify(reading, surface)} via=${via}`, candidates, CAND_LIMIT_PICK, provenance,
      undefined, fromUpdate ? InputLog.originsOf(candidates, CAND_LIMIT_PICK) : undefined,
      fromUpdate && InputLog.lastSegDiag.length > 0 ? InputLog.lastSegDiag : undefined);
  }

  // 候補更新の直前に呼ぶ。reading は変換対象 (範囲を縮めていればその部分)。
  static noteCandidateOrigins(reading: string, origins: Map<string, string>, segDiag: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.lastOrigins = origins;
    InputLog.lastOriginsReading = reading;
    InputLog.lastSegDiag = segDiag;
  }

  private static originsOf(candidates: string[], limit: number): string {
    const out: string[] = [];
    const n = Math.min(candidates.length, limit);
    for (let i = 0; i < n; i++) { out.push(InputLog.lastOrigins.get(candidates[i]) ?? '?'); }
    return out.join(',');
  }

  static recordRaw(reading: string, surface: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (reading.length === 0 && surface.length === 0) { return; }
    InputLog.push(LK_RAW, reading, surface, -1, 0, '');
  }

  // 変換範囲を ◀▶ で伸縮した。x は "変換する長さ/全体の長さ"。
  static recordRange(reading: string, end: number, dir: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_RANGE, reading, reading.slice(0, end), -1, 0,
      `${end}/${reading.length} ${dir}`);
  }

  // 文節モードに入った。x は変換器が切った区切り位置 ("わたしは|がっこうへ")。
  static recordBunsetsu(reading: string, segs: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BUNSETSU, reading, '', -1, 0, segs.join('|'));
  }

  static recordBunsetsuPick(seg: string, surface: string, index: number, total: number,
      candidates: string[], via: string, provenance: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BSEG, seg, surface, index, total,
      `${InputLog.classify(seg, surface)} via=${via}`, candidates, CAND_LIMIT_PICK, provenance);
  }

  static recordBunsetsuFinal(reading: string, surface: string, segs: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BSEG_FINAL, reading, surface, -1, 0, segs.join('|'));
  }

  // 文節モードで注目する文節を移した (矢印キー/タップ)。
  static recordBunsetsuFocus(segs: string[], chosen: string[], from: number, to: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BFOCUS, segs.join('|'), chosen.join('|'), to, segs.length, `${from}->${to}`);
  }

  static recordBunsetsuCancel(reading: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BCANCEL, reading, '', -1, 0, '');
  }

  static recordDeleteCandidate(reading: string, surface: string, index: number, total: number,
      via: string, provenance: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_DELCAND, reading, surface, index, total, `via=${via}`,
      undefined, 0, [provenance]);
  }

  // 候補一覧 (▼) を開いた / 選ばずに閉じた。一覧から選んで閉じた場合は
  // commit の via=list で分かるので、ここでは記録しない。
  static recordExpand(open: boolean, reading: string, candidates: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_EXPAND, reading, '', -1, candidates.length, open ? 'open' : 'close',
      open ? candidates : undefined, CAND_LIMIT_PICK);
  }

  // フリック1回の詳細。dx/dy は指を離した位置の移動量 (vp)。方向判定の
  // しきい値 (FlickKeyboardView.THRESHOLD) 付近の誤判定を後から見るため。
  static recordFlick(center: string, out: string, dir: string, dx: number, dy: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_FLICK, center, out, -1, 0, `${dir} ${Math.round(dx)},${Math.round(dy)}`);
  }

  // 候補バーを横にスクロールして止めた。offset はスクロール位置 (vp)。
  // 「タップした候補がスクロールしないと見えない位置だったか」の手がかり。
  static recordScroll(reading: string, offsetVp: number, total: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_SCROLL, reading, '', Math.round(offsetVp), total, '');
  }

  static recordMenu(action: string, reading: string, surface: string, index: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_MENU, reading, surface, index, 0, action);
  }

  static recordSubMode(mode: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_SUBMODE, '', '', -1, 0, mode);
  }

  // 変換1回などの所要時間を溜めて、PERF_EVERY 件ごとに 1 行だけ残す。
  // 実機 (とくに安い端末) で本当に何 ms かかっているかを、あとからログで
  // 見るためのもの。記録そのものが重くならないよう、1件ずつは残さない。
  static notePerf(name: string, ms: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (name !== InputLog.perfName) {
      InputLog.perfName = name;
      InputLog.perfCount = 0;
      InputLog.perfSum = 0;
      InputLog.perfMax = 0;
      InputLog.perfSlow = 0;
    }
    InputLog.perfCount++;
    InputLog.perfSum += ms;
    if (ms > InputLog.perfMax) { InputLog.perfMax = ms; }
    if (ms >= 16) { InputLog.perfSlow++; }
    if (InputLog.perfCount >= PERF_EVERY) {
      InputLog.push(LK_PERF, name, '', -1, InputLog.perfCount,
        `avg=${(InputLog.perfSum / InputLog.perfCount).toFixed(1)} max=${InputLog.perfMax} slow16=${InputLog.perfSlow}`);
      InputLog.perfCount = 0;
      InputLog.perfSum = 0;
      InputLog.perfMax = 0;
      InputLog.perfSlow = 0;
    }
  }

  static recordSetting(name: string, value: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_SETTING, name, value, -1, 0, '');
  }

  // 閉じ括弧の保留をどう解いたか。trigger は flush (►・バッジのタップ・
  // 送信直前) / cancel (バッジの長押し) / auto (自動モードで確定に付いた)。
  // 手動モードで「括弧をどこで閉じているか・閉じ忘れていないか」を見るため。
  static recordBracket(close: string, trigger: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BRACKET, '', close, -1, 0, trigger);
  }

  static recordUnknownWord(reading: string, surface: string, confirmed: boolean): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_UNKNOWN, reading, surface, -1, 0, confirmed ? 'confirmed' : '');
  }

  static recordQuasiPick(reading: string, surface: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_QUASI_PICK, reading, surface, -1, 0, '');
  }

  static recordKnownAssembly(reading: string, surface: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_KNOWN_ASSEMBLY, reading, surface, -1, 0, '');
  }

  // 文字を消した。x は消した時の状態。確定後すぐの idle 削除は「確定した変換が
  // 間違っていた」の合図になるので、状態を分けて残す。
  // 打っている途中で消し始めたとき (続けて消す最初の 1 字) は、消す前に出ていた読みと候補も残す。
  // 「出てほしい語が候補に無くて打ち直した」を後から確かめるため (ユーザー 2026-10-20: ある活用形で出ない動詞を、
  // 別の形で打って確定し後ろを消して打ち直していた。そのとき何が出ていたかがログに無かった)
  static recordDeleteChar(ch: string, state: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (state === 'COMPOSING' && !InputLog.inDeleteRun && InputLog.lastCands.length > 0) {
      InputLog.push(LK_DELCHAR, InputLog.lastReading, ch, -1, InputLog.lastCands.length, state,
        InputLog.lastCands, CAND_LIMIT_UPDATE);
    } else {
      InputLog.push(LK_DELCHAR, '', ch, -1, 0, state);
    }
    InputLog.inDeleteRun = true;
  }

  static recordUndo(ch: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_UNDO, '', ch, -1, 0, '');
  }

  // ローマ字キー。committed は この打鍵で確定したかな (無ければ '')、
  // pending はこの打鍵の後に残っている未確定ローマ字。
  static recordKey(key: string, committed: string, pending: string, mode: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_KEY, key, committed, -1, 0, `${mode} pend=${pending}`);
  }

  static recordKana(kana: string, mode: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_KANA, '', kana, -1, 0, mode);
  }

  // 濁点/半濁点/小文字の切替。before はかな列の末尾 (切替前)、after は切替後。
  static recordVariant(kind: string, before: string, after: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_VARIANT, before, after, -1, 0, kind);
  }

  // 変換中に候補が更新された。「正しい候補が一度は出ていたのに打ち続けた」
  // 「確定直前まで出ていなかった」を後から見分けるための記録。
  static recordUpdate(reading: string, pending: string, convEnd: number, candidates: string[],
      segs: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    // 以前は候補40件を join して比べていたが、打鍵ごとに走るので
    // 先頭候補と件数だけで十分 (同じ読みで先頭も件数も同じなら同じ列)。
    InputLog.lastReading = reading;
    InputLog.lastCands = candidates;
    const key = `${reading}\u0001${convEnd}\u0001${candidates.length}${candidates.length > 0 ? candidates[0] : ''}`;
    if (key === InputLog.lastUpdateKey) { return; }
    InputLog.lastUpdateKey = key;
    InputLog.push(LK_UPDATE, reading, pending, -1, candidates.length,
      convEnd > 0 && convEnd < reading.length ? `range=${convEnd}/${reading.length}` : '',
      candidates, CAND_LIMIT_UPDATE, undefined, segs.length > 1 ? segs.join('|') : undefined,
      InputLog.originsOf(candidates, CAND_LIMIT_UPDATE),
      InputLog.lastSegDiag.length > 0 ? InputLog.lastSegDiag : undefined);
  }

  static recordCursor(dir: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_CURSOR, '', '', -1, 0, dir);
  }

  static recordSpace(state: string, reading: string, index: number, total: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_SPACE, reading, '', index, total, state);
  }

  static recordEnter(state: string, reading: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_ENTER, reading, '', -1, 0, state);
  }

  // ホストへ実際に挿入した文字列。確定の記録とは別に、括弧の自動閉じや
  // 記号パネル・絵文字など insertText に直接来る入力もここで全部拾う。
  // 貼り付けは本文を残さない (recordPaste を使い、呼び出し側でこちらを抑止する)。
  static recordInsert(text: string, route: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_INSERT, '', text, -1, 0, route);
  }

  static recordPaste(route: string, length: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_PASTE, '', '', length, 0, route);
  }

  static recordPredictions(predictions: string[], context: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (predictions.length === 0) { return; }
    InputLog.push(LK_PRED, '', '', -1, predictions.length, context.join('|'), predictions,
      CAND_LIMIT_PICK);
  }

  static recordPredictionPick(text: string, index: number, strip: string[]): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_PRED_PICK, '', text, index, strip.length, '', strip, CAND_LIMIT_PICK);
  }

  static recordMode(what: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_MODE, '', '', -1, 0, what);
  }

  static recordBoundary(reason: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.push(LK_BOUNDARY, '', '', -1, 0, reason);
  }

  static recordAbandon(reading: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    if (reading.length === 0) { return; }
    // 全部消す前に出ていた候補も (何が無くて諦めたかを見るため)
    InputLog.push(LK_ABANDON, reading, '', -1, InputLog.lastCands.length, '', InputLog.lastCands, CAND_LIMIT_UPDATE);
  }

  // ---- 書き込み ---------------------------------------------------------

  private static push(k: string, r: string, s: string, i: number, n: number, x: string,
      candidates?: string[], candLimit?: number, provenance?: string[], segs?: string,
      origins?: string, segDiag?: string): void {
    if (InputLog.secureField) { return; }
    if (InputLog.filesDir.length === 0) { return; }
    if (k !== LK_DELCHAR) { InputLog.inDeleteRun = false; }
    if (k === LK_COMMIT || k === LK_RAW || k === LK_ABANDON) { InputLog.lastCands = []; InputLog.lastReading = ''; }
    const e: InputLogEntry = { t: Date.now(), k: k, r: r, s: s, i: i, n: n, x: x };
    if (candidates !== undefined && candidates.length > 0) {
      e.c = candidates.slice(0, candLimit !== undefined && candLimit > 0 ? candLimit : CAND_LIMIT_UPDATE).join(',');
    }
    if (provenance !== undefined && provenance.length > 0) { e.p = provenance.join(','); }
    if (segs !== undefined && segs.length > 0) { e.g = segs; }
    if (origins !== undefined && origins.length > 0) { e.o = origins; }
    if (segDiag !== undefined && segDiag.length > 0) { e.q = segDiag; }
    if (InputLog.fieldPending) {
      InputLog.pending.push(e);
      if (InputLog.pending.length > MAX_PENDING) { InputLog.pending.shift(); }
      return;
    }
    InputLog.buffer.push(e);
    if (InputLog.buffer.length >= FLUSH_AT) {
      InputLog.flush();
      return;
    }
    clearTimeout(InputLog.timer);
    InputLog.timer = setTimeout(() => { InputLog.flush(); }, FLUSH_MS);
  }

  // 保留を経由せず必ず書く。本文を含まない診断用の記録だけに使う。
  private static write(k: string, r: string, s: string, i: number, n: number, x: string): void {
    if (InputLog.filesDir.length === 0) { return; }
    InputLog.buffer.push({ t: Date.now(), k: k, r: r, s: s, i: i, n: n, x: x });
    InputLog.flush();
  }

  // 溜めた分を追記する。フィールドを離れるときにも呼ぶ (取りこぼし防止)。
  static flush(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    clearTimeout(InputLog.timer);
    const rows = InputLog.buffer;
    if (rows.length === 0 || InputLog.filesDir.length === 0) { return; }
    InputLog.buffer = [];
    let text = '';
    for (const e of rows) { text += JSON.stringify(e) + '\n'; }
    try {
      const path = InputLog.path();
      const file = fs.openSync(path,
        fs.OpenMode.WRITE_ONLY | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      fs.writeSync(file.fd, text);
      fs.closeSync(file);
      InputLog.flushCount++;
      if (InputLog.flushCount % TRIM_CHECK_EVERY === 0) { InputLog.trim(path); }
    } catch (_e) {
      // 収集が失敗しても入力そのものは止めない。
    }
  }

  // 上限を超えたら、末尾から上限の半分ぶんだけ残す (半分にするだけだと1回で
  // 上限を下回らず、以後 flush のたびに数MBを読み直すことになるため)。
  private static trim(path: string): void {
    try {
      if (fs.statSync(path).size <= MAX_BYTES) { return; }
      const lines = fs.readTextSync(path).split('\n');
      let kept = 0;
      let start = lines.length;
      while (start > 0 && kept < MAX_BYTES / 2) {
        start--;
        kept += lines[start].length + 1;
      }
      const text = lines.slice(start).join('\n');
      const file = fs.openSync(path,
        fs.OpenMode.WRITE_ONLY | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, text);
      fs.closeSync(file);
    } catch (_e) {}
  }

  // ---- 状態表示・書き出し・消去 ------------------------------------------

  static readRaw(): string {
    if (!DEBUG_INPUT_LOG || InputLog.filesDir.length === 0) { return ''; }
    try {
      const path = InputLog.path();
      if (!fs.accessSync(path)) { return ''; }
      return fs.readTextSync(path);
    } catch (_e) {
      return '';
    }
  }

  static sizeBytes(): number {
    if (!DEBUG_INPUT_LOG || InputLog.filesDir.length === 0) { return 0; }
    try {
      const path = InputLog.path();
      if (!fs.accessSync(path)) { return 0; }
      return fs.statSync(path).size;
    } catch (_e) {
      return 0;
    }
  }

  // 設定画面に出す一行。件数を数えるのに全文を読むが、上限 6MB なので
  // 画面を開いたときに1回読む程度なら問題ない。
  static summary(): string {
    if (!DEBUG_INPUT_LOG) { return ''; }
    if (InputLog.filesDir.length === 0) { return '収集先が未設定(初期化に失敗)'; }
    InputLog.flush();
    const raw = InputLog.readRaw();
    let lines = 0;
    for (let i = 0; i < raw.length; i++) {
      if (raw.charCodeAt(i) === 0x0a) { lines++; }
    }
    const kb = Math.round(InputLog.sizeBytes() / 1024);
    return `${lines}件 / ${kb}KB`;
  }

  // 全文を hilog へ流す。PC 側 (tools/pull_input_log.js) が DUMP_MARKER の行を
  // 拾い、連番順に並べて Base64 を戻す。onDone は流し終わった (または流すものが
  // 無かった) ときに呼ばれ、引数は書き出したチャンク数 (失敗時は -1)。
  // 本文は zlib (zlib 形式 = PC 側の zlib.inflateSync で戻せる) で圧縮して
  // から流す。圧縮に失敗した・結果が返ってこない場合は無圧縮で流す
  // (BEGIN 行の enc= で PC 側が見分ける)。
  static dumpToHilog(onDone: (chunks: number) => void): void {
    if (!DEBUG_INPUT_LOG) { onDone(-1); return; }
    if (InputLog.dumping) { return; }
    InputLog.flush();
    const raw = InputLog.readRaw();
    if (raw.length === 0) {
      console.info(`shuntiIME ${DUMP_MARKER}_BEGIN total=0 rawLen=0 b64Len=0 enc=raw`);
      console.info(`shuntiIME ${DUMP_MARKER}_END`);
      onDone(0);
      return;
    }
    InputLog.dumping = true;
    let bytes: Uint8Array;
    try {
      bytes = new util.TextEncoder().encodeInto(raw);
    } catch (e) {
      InputLog.dumping = false;
      console.error('shuntiIME InputLog dump encode failed', JSON.stringify(e));
      onDone(-1);
      return;
    }
    let started = false;
    const start = (payload: Uint8Array, enc: string): void => {
      if (started) { return; }
      started = true;
      InputLog.streamToHilog(payload, enc, raw.length, onDone);
    };
    // 圧縮の Promise が返ってこない端末でも止まらないよう、一定時間で無圧縮に切り替える。
    const fallback = setTimeout(() => { start(bytes, 'raw'); }, 8000);
    try {
      const zip = zlib.createZipSync();
      zip.compressBound(bytes.length).then((bound: number) => {
        const dest = new ArrayBuffer(bound);
        const src = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        return zip.compress(dest, src, bytes.byteLength).then((info: zlib.ZipOutputInfo) => {
          clearTimeout(fallback);
          start(new Uint8Array(dest, 0, info.destLen), 'zlib');
        });
      }).catch((e: Error) => {
        clearTimeout(fallback);
        console.error('shuntiIME InputLog compress failed, sending raw', JSON.stringify(e));
        start(bytes, 'raw');
      });
    } catch (e) {
      clearTimeout(fallback);
      console.error('shuntiIME InputLog compress unavailable, sending raw', JSON.stringify(e));
      start(bytes, 'raw');
    }
  }

  private static streamToHilog(payload: Uint8Array, enc: string, rawLen: number,
      onDone: (chunks: number) => void): void {
    let b64 = '';
    try {
      b64 = new util.Base64Helper().encodeToStringSync(payload);
    } catch (e) {
      InputLog.dumping = false;
      console.error('shuntiIME InputLog dump base64 failed', JSON.stringify(e));
      onDone(-1);
      return;
    }
    const total = Math.ceil(b64.length / DUMP_CHUNK);
    console.info(`shuntiIME ${DUMP_MARKER}_BEGIN total=${total} rawLen=${rawLen} b64Len=${b64.length} enc=${enc}`);
    let i = 0;
    const writeBatch = (): void => {
      const end = Math.min(i + DUMP_BATCH, total);
      for (; i < end; i++) {
        const chunk = b64.slice(i * DUMP_CHUNK, (i + 1) * DUMP_CHUNK);
        console.info(`shuntiIME ${DUMP_MARKER} ${i}/${total} ${chunk}`);
      }
      if (i < total) {
        setTimeout(writeBatch, DUMP_INTERVAL_MS);
      } else {
        console.info(`shuntiIME ${DUMP_MARKER}_END`);
        InputLog.dumping = false;
        onDone(total);
      }
    };
    writeBatch();
  }

  static clear(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.buffer = [];
    InputLog.pending = [];
    InputLog.lastUpdateKey = '';
    if (InputLog.filesDir.length === 0) { return; }
    try {
      const path = InputLog.path();
      if (fs.accessSync(path)) { fs.unlinkSync(path); }
    } catch (_e) {}
  }

  // ---- 補助 -------------------------------------------------------------

  // 確定の性質。カタカナ変換・英数変換・かな確定は「候補の順番の問題」ではない
  // ので、精度の集計から外せるように印を付けておく。
  private static classify(reading: string, surface: string): string {
    if (surface === reading) { return LF_KANA; }
    if (InputLog.isAllKatakana(surface) && InputLog.toKatakana(reading) === surface) {
      return LF_KATAKANA;
    }
    if (InputLog.isAllAscii(surface)) { return LF_ALNUM; }
    return '';
  }

  private static isAllKatakana(s: string): boolean {
    if (s.length === 0) { return false; }
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (!(c >= 0x30a1 && c <= 0x30fc)) { return false; }
    }
    return true;
  }

  private static isAllAscii(s: string): boolean {
    if (s.length === 0) { return false; }
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c > 0x7e || c < 0x20) { return false; }
    }
    return true;
  }

  private static toKatakana(s: string): string {
    let out = '';
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      out += (c >= 0x3041 && c <= 0x3096) ? String.fromCharCode(c + 0x60) : s.charAt(i);
    }
    return out;
  }
}

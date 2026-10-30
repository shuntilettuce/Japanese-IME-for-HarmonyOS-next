import inputMethodEngine from '@ohos.inputMethodEngine';
import InputMethodExtensionContext from '@ohos.InputMethodExtensionContext';
import type { Want } from '@kit.AbilityKit';
import display from '@ohos.display';
import dataPreferences from '@ohos.data.preferences';
import pasteboard from '@ohos.pasteboard';
import vibrator from '@ohos.vibrator';
import bundleManager from '@ohos.bundle.bundleManager';

import { JapaneseConverter, toKatakana, toHalfKatakana } from './JapaneseConverter';
import { KanaKanjiConverter } from './KanaKanjiConverter';
import { UserDictStore } from './UserDictStore';
import { UserPosStore } from './UserPosStore';
import type { UserPosEntry, PosKind, VerbGroup } from './ConjugationEngine';
import { SettingsStore, KeyboardSettings, isDarkFromConfiguration, isDarkFromResourceManager, resolveIsDark } from './SettingsStore';
import { findPredictiveCompletion } from './PredictivePhrases';
import { NextWordPredictor } from './NextWordPredictor';
import { PredictiveConversion } from './PredictiveConversion';
import { UnknownWordLearner } from './UnknownWordLearner';
import { ShortTermMemory } from './ShortTermMemory';
// デバッグビルドでだけ記録する (BuildProfile.DEBUG に連動、InputLog.ets 冒頭参照)。
import { InputLog, DEBUG_INPUT_LOG } from './InputLog';
import { SuppressedCandidates, SUPPRESSED_STORE, SUPPRESSED_KEY } from './SuppressedCandidates';
import { DateTimePredictor } from './DateTimePredictor';
import { NumberFormatter } from './NumberFormatter';
import { bracketVariants } from './Brackets';
import { EmojiSuggest } from './EmojiSuggest';
import { AppMaintenance } from './AppMaintenance';
import { AiConverter } from './AiConverter';
import { SentenceFlow } from './SentenceFlow';
import { ClipboardPins } from './ClipboardPins';

interface InputMethodSubtype {
  id: string;
  label?: string;
  locale?: string;
  mode?: string;
}

// Segmentation of the current composing text plus each non-final segment's
// auto-converted surface, captured while updateCandidates runs (where the
// segmentation and prefix conversions are already computed). A later
// whole-sentence commit uses this to learn each 文節 individually: it peels
// the known prefix off the committed text to recover the final segment's
// accepted surface, so homophone corrections generalize word-by-word --
// see learnCommit / KanaKanjiConverter.recordSegmentedChoice.
interface ComposeAlignment {
  segs: string[];            // per-segment readings
  prefixSurfaces: string[];  // auto-converted surface of segs[0..n-2]
  prefixText: string;        // prefixSurfaces joined
}

// ホスト側(確定済み)テキストへの1回の操作。hostUndoStack の要素。
// 詳しい設計意図は hostUndoStack のフィールドコメント参照。
interface InsertTextOptions {
  raw: boolean;
}

interface HostEditOp {
  kind: 'insert' | 'delete' | 'move';   // move: ◄► でカーソルを動かした (at = 動かす前の位置)
  t?: number;                           // 記録した時刻 (続けて消した分をまとめるため)
  text: string;
  at: number;   // IME自己完結の相対座標(ホストの絶対indexではない)
}

// 開き括弧 -> 閉じ括弧。pendingBracketClose の元ネタ。
//
// 以前は「（）」のように開き・閉じを1つの候補として丸ごと確定し、確定後に
// カーソルだけ内側へ戻す実装だったが、moveCursor/selectByRange のどちらも
// 実機で信用できないことが分かり撤去した (コミット 7328e504 参照)。
//
// 今回は発想を変え、カーソルを一切動かさない: 開きだけを確定し、閉じは
// 保留しておいて、あとから「その時点のカーソル位置」へ挿入する。カーソル
// 移動プリミティブを一切使わないので、上記の実機不具合と無縁になる。
//
// 括弧の入れ方は設定 bracketCloseMode で選べる (SettingsStore 参照)。
//   'pair' (既定): 開きと閉じを一緒に入れて、カーソルを中へ (HarmonyOS 7 から moveCursor が使える)
//   'manual'      : 上の保留式。► キー / 待機バッジのタップ / 送信直前に閉じが出る
const BRACKET_PAIRS: Record<string, string> = {
  '（': '）', '「': '」', '『': '』', '【': '】',
  '〖': '〗', '〔': '〕', '〈': '〉', '《': '》',
  '(': ')', '[': ']', '{': '}',
  '［': '］', '｛': '｝', '〝': '〟',
  // Brackets.ets の全種類のうち、括弧以外にも使う < ＜ (不等号) と ‘ (アポストロフィ) は自動で閉じない
  '〘': '〙', '〚': '〛', '｢': '｣', '«': '»', '‹': '›', '“': '”', '｟': '｠',
};

// Cap for tablet/foldable content width (see computeKeyWidths / applyKeyboardWidthsForVp).
const MAX_KB_VP = 620;

// 打鍵の間隔がこれ未満なら「まだ打っている」とみなし、候補列のおまけ
// (同音語の差し替え・区切り違いの別解) を後回しにする。指が止まってから
// この時間後に計算し直す。人の連打はおおむね 100〜200ms 間隔。
const ENRICH_DELAY_MS = 160;
// 短い読みではおまけの計算自体が軽いので、後回しにせずそのまま出す。
const EXTRAS_MIN_KANA = 4;

// フローティングキーボードの位置・大きさ (vp)。KeyboardController.floatingRectVp
// 参照。
interface FloatingRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

// KeyboardController.displayForFloating の戻り値。
interface DisplayInfo {
  dis: display.Display;
  density: number;
}

// Pure width math shared by the display-metrics-based initial guess (computeKeyWidths)
// and the live layout correction (KeyboardPage.onAreaChange). display.getDefaultDisplaySync()
// can under- or over-report the actually drawable panel width on some devices (e.g. a
// landscape cutout/nav-bar inset it doesn't account for), which made keys sized from the
// raw display metrics alone overflow past the real edge on one side while leaving a dead
// band on the other. Recomputing from the *measured* on-screen width — whatever the true
// source of the discrepancy — self-corrects regardless of cause.
export function applyKeyboardWidthsForVp(rawScreenVp: number, isTabletClass: boolean, isLandscape: boolean): void {
  // タブレット/フォルダブルの展開画面 (isTabletClass) だけに効く、最大幅の
  // 上限そのものの倍率。実機を持たない開発側では画面比率ごとの最適値が
  // 分からないため、設定 → ビジュアル調整 で利用者本人が追い込めるように
  // してある (既定 1.0 = 従来どおり MAX_KB_VP=620vp)。通常のスマホでは
  // isTabletClass が false になるのでこの値自体が一切効かない。
  const widthScale = isTabletClass ? Math.min(1.6, Math.max(0.5, AppStorage.get<number>('keyboardWidthScale') ?? 1.0)) : 1.0;
  const screenVp = isTabletClass ? Math.min(rawScreenVp, MAX_KB_VP * widthScale) : rawScreenVp;
  AppStorage.setOrCreate('keyboardContentVp', screenVp);
  // キーボード幅の上限スライダーのライブ反映 (applyKeyboardWidthScaleLive) が
  // 上限を引き上げたときに実際に広がれるよう、キャップ適用前の生の値も
  // 別キーで残しておく (keyboardContentVp は既にキャップ後の値なので、
  // それを入力に再計算すると小さいほうに固定されて戻れなくなる)。
  AppStorage.setOrCreate('keyboardRawContentVp', rawScreenVp);
  // 10-key rows (KeyboardView's number row / qwerty row / romaji asdfghjklー
  // row): 4vp total H-padding + 9 gaps × 4vp (KeyboardView's Row({space:4}),
  // not the 2vp this used to assume before the design-token pass bumped row
  // spacing -- that mismatch was the actual bug: keys came out ~2.7vp too
  // wide each, ~27vp of overflow past the right edge on any 10-key row) =
  // 40vp overhead. The middle letter row (asdfghjkl + ー) and bottom row
  // (shift + zxcvbnm + backspace) also reuse this width; they aren't a
  // fixed-width fit like this row, just centered, so a different key count
  // is harmless.
  const kwVp = Math.max(Math.floor((screenVp - 40) / 10), 24);
  const skwVp = Math.round(kwVp * 1.2);
  // Space bar = remaining after mode + 、 + ◄ + ► + 文節 + 。 + enter + 7 gaps × 4vp
  // (◄ ► sit right after space at the letter-key width, then 文節 at the
  // same special-key width as mode/enter — the row would overflow if it
  // were sized on top of them instead of space giving up the room). Same space:4 as kwVp above.
  const spaceVp = Math.max(Math.floor(screenVp - 4 - 3 * skwVp - 4 * kwVp - 28), 60);
  // Flick keys: 5 columns (左特殊・中央3列・右特殊) + 4 gaps × 4vp
  // (FlickKeyboardView's Row({space:4})) + 0vp H-padding (its Column has no
  // left/right padding) = 16vp overhead -- same total as the old "4 gaps ×
  // 3vp + 4vp H-padding" comment assumed, by coincidence, so this one was
  // never actually broken.
  //
  // flickSideColumnRatio (設定 → レイアウト調整) は「左右の特殊キー1列の
  // 幅 / 中央の かなキー1列の幅」。既定 1.0 で今までどおり5列均等になる。
  // available = 2*side + 3*mid, side = ratio*mid という連立から解く。
  const available = screenVp - 16;
  const sideRatio = Math.min(2.0, Math.max(0.4, AppStorage.get<number>(
    isLandscape ? 'flickSideColumnRatioLandscape' : 'flickSideColumnRatioPortrait') ?? 1.0));
  const flickKwVp = Math.max(Math.floor(available / (3 + 2 * sideRatio)), 40);
  const flickSideKwVp = Math.max(Math.floor(flickKwVp * sideRatio), 32);
  AppStorage.setOrCreate('keyWidthVp', kwVp);
  AppStorage.setOrCreate('specialKeyWidthVp', skwVp);
  AppStorage.setOrCreate('spaceKeyWidthVp', spaceVp);
  AppStorage.setOrCreate('flickKeyWidthVp', flickKwVp);
  AppStorage.setOrCreate('flickSideKeyWidthVp', flickSideKwVp);
  // Symbol panel: 8 columns + 7 gaps × 4vp (SymbolView's Row({space:4})) +
  // 8vp H-padding (left:4 + right:4) = 36vp overhead -- was assuming 2vp
  // gaps + 6vp padding (20vp) here too, same class of bug as kwVp above.
  const symKwVp = Math.max(Math.floor((screenVp - 36) / 8), 36);
  AppStorage.setOrCreate('symKeyWidthVp', symKwVp);
}

export const enum InputState {
  IDLE = 'IDLE',
  COMPOSING = 'COMPOSING',
  SELECTING = 'SELECTING',
  BUNSETSU = 'BUNSETSU',
}

export const enum InputMode {
  HIRAGANA = 'HIRAGANA',
  KATAKANA = 'KATAKANA',
  ALPHANUMERIC = 'ALPHANUMERIC',
}

const IMEAbility = inputMethodEngine.getInputMethodAbility();

// Cycles through all applicable variants of a kana character on repeated taps.
// Order: small (if applicable) → dakuten → handakuten (if applicable) → original
const VARIANT_CYCLE: Record<string, string> = {
  // ka行: dakuten toggle
  'か':'が','が':'か','き':'ぎ','ぎ':'き','く':'ぐ','ぐ':'く','け':'げ','げ':'け','こ':'ご','ご':'こ',
  // sa行: dakuten toggle
  'さ':'ざ','ざ':'さ','し':'じ','じ':'し','す':'ず','ず':'す','せ':'ぜ','ぜ':'せ','そ':'ぞ','ぞ':'そ',
  // ta行: ち/て/と dakuten only; つ→っ→づ→つ (small first)
  'た':'だ','だ':'た','ち':'ぢ','ぢ':'ち','て':'で','で':'て','と':'ど','ど':'と',
  'つ':'っ','っ':'づ','づ':'つ',
  // ha行: dakuten → handakuten → original
  'は':'ば','ば':'ぱ','ぱ':'は','ひ':'び','び':'ぴ','ぴ':'ひ',
  'ふ':'ぶ','ぶ':'ぷ','ぷ':'ふ','へ':'べ','べ':'ぺ','ぺ':'へ','ほ':'ぼ','ぼ':'ぽ','ぽ':'ほ',
  // う: small → dakuten → original
  'う':'ぅ','ぅ':'ゔ','ゔ':'う',
  // small vowels (no dakuten)
  'あ':'ぁ','ぁ':'あ','い':'ぃ','ぃ':'い','え':'ぇ','ぇ':'え','お':'ぉ','ぉ':'お',
  'や':'ゃ','ゃ':'や','ゆ':'ゅ','ゅ':'ゆ','よ':'ょ','ょ':'よ','わ':'ゎ','ゎ':'わ',
  // katakana — same pattern
  'カ':'ガ','ガ':'カ','キ':'ギ','ギ':'キ','ク':'グ','グ':'ク','ケ':'ゲ','ゲ':'ケ','コ':'ゴ','ゴ':'コ',
  'サ':'ザ','ザ':'サ','シ':'ジ','ジ':'シ','ス':'ズ','ズ':'ス','セ':'ゼ','ゼ':'セ','ソ':'ゾ','ゾ':'ソ',
  'タ':'ダ','ダ':'タ','チ':'ヂ','ヂ':'チ','テ':'デ','デ':'テ','ト':'ド','ド':'ト',
  'ツ':'ッ','ッ':'ヅ','ヅ':'ツ',
  'ハ':'バ','バ':'パ','パ':'ハ','ヒ':'ビ','ビ':'ピ','ピ':'ヒ',
  'フ':'ブ','ブ':'プ','プ':'フ','ヘ':'ベ','ベ':'ペ','ペ':'ヘ','ホ':'ボ','ボ':'ポ','ポ':'ホ',
  'ウ':'ゥ','ゥ':'ヴ','ヴ':'ウ',
  'ア':'ァ','ァ':'ア','イ':'ィ','ィ':'イ','エ':'ェ','ェ':'エ','オ':'ォ','ォ':'オ',
  'ヤ':'ャ','ャ':'ヤ','ユ':'ュ','ュ':'ユ','ヨ':'ョ','ョ':'ヨ','ワ':'ヮ','ヮ':'ワ',
};

// ---------- InputHandler ----------

// メールのドメイン。順は教材 (FineWeb・おーぷん2ちゃんねる) に「@ドメイン」が書かれた回数の多い順 (2026-10-13 に数えた。
// gmail.com 253・yahoo.co.jp 128・docomo.ne.jp 56・i.softbank.jp 38・hotmail.com 30・ezweb.ne.jp 21・live.jp 20 …)。
// 教材は古めの文が多く icloud.com が少ない (7) ので、今よく使われる icloud.com・outlook.jp は後ろに足した
const EMAIL_DOMAINS: string[] = ['gmail.com', 'yahoo.co.jp', 'docomo.ne.jp', 'i.softbank.jp', 'hotmail.com', 'ezweb.ne.jp',
  'live.jp', 'hotmail.co.jp', 'ybb.ne.jp', 'softbank.ne.jp', 'nifty.com', 'me.com', 'yahoo.com', 'icloud.com', 'outlook.jp',
  'outlook.com', 'au.com', 'rakuten.jp'];

function emailCompletions(composing: string): string[] {
  const m = /^([A-Za-z0-9._%+\-]+)@([A-Za-z0-9.\-]*)$/.exec(composing);
  if (m === null) { return []; }
  const typed = m[2].toLowerCase();
  return EMAIL_DOMAINS.filter((d: string) => d.startsWith(typed) && d !== typed).slice(0, 5)
    .map((d: string) => `${m[1]}@${d}`);
}

export class InputHandler {
  private static _instance: InputHandler | undefined = undefined;
  private kbController: inputMethodEngine.KeyboardController | undefined;
  private inputClient: inputMethodEngine.InputClient | undefined;
  private converter: JapaneseConverter = new JapaneseConverter();
  // bunsetsu state
  private bSegs: string[] = [];
  private bChosen: string[] = [];
  private bCur: number = 0;
  // 打鍵が続いている間は、候補列の「おまけ」(同音語の差し替え・区切り違いの
  // 別解) の計算を飛ばす。どちらも候補0を変えず、位置1以降を足すだけなのに
  // 1打鍵あたりの計算時間の7割を占めていた (実機ログの読みで実測)。指が
  // 止まってから ENRICH_DELAY_MS 後に計算し直して足す。安い端末ほど効く。
  private enrichTimer: number = -1;
  private enrichPending: boolean = false;
  private lastKeystrokeAt: number = 0;

  // Segmentation + prefix surfaces of the most recent updateCandidates()
  // composing text, or undefined when it was a single segment / had a symbol
  // prefix. Lets a whole-sentence commit learn each 文節 individually (see
  // learnCommit / KanaKanjiConverter.recordSegmentedChoice).
  private lastAlignment: ComposeAlignment | undefined = undefined;
  private insertedBuffer: string[] = [];
  // 変換前のかな組み立て中(COMPOSING)にbackspaceで消した文字のLIFO。この
  // バッファは常に末尾でしか編集できない(カーソルが途中へ移動する経路が
  // 無い)ので、位置のズレが起きようがない。確定するとその回の組み立ては
  // 終わりなので resetComposition で空にする(handleDirectKana 参照)。
  private composingUndoStack: string[] = [];
  private conversionEnd: number = 0;
  // composingText の「今回の入力で一番長く伸びた時点」の値。updateCandidates
  // のたびに更新し (打っている間は常に候補が見えているので、これが伸びる=
  // 「変換候補を見た」の代わり)、消して空に戻ったときに
  // UnknownWordLearner.noteAbandonedReading へ渡す。確定すれば
  // resetComposition で捨てる (諦めたのではなく使われたため)。
  private composingWatermark: string = '';
  // この入力欄が setPreviewTextSync に対応しているか (onInputStart で判定)。
  private previewSupported: boolean = false;
  // 対応可否がまだ分からない (getEditorAttribute の応答待ち) か。
  // onInputStart は「まず false にして publishPreviewMode → 非同期で本当の値が
  // 来たらもう一度」の順で走るので、対応している欄でも一瞬だけ
  // previewInField=false が配られる。UI 側がこれを「非対応の欄に入った」と
  // 受け取って、どんな入力欄でも毎回フォールバックのバナーを出してしまって
  // いた。判定が確定するまでバナーを抑止するためのフラグ。
  private previewSupportPending: boolean = false;
  // 貼り付けの間だけ立てる。insertText は挿入した文字列を入力ログに残すが、
  // クリップボードの中身は残さない方針なので、貼り付け経路からの挿入だけ
  // 記録を止める (貼り付けた事実と文字数は InputLog.recordPaste で別に残す)。
  private suppressInsertLog: boolean = false;
  // 実際に入力欄へプレビューを出している最中か。出していないのに
  // finishTextPreviewSync を呼ぶと無駄な IPC になるので状態を持つ。
  private previewActive: boolean = false;
  // いまプレビューとして出している文字数 (取り消し時の削除長に使う)。
  private previewLen: number = 0;
  // 入力中のかなをどこに出すかの設定 (SettingsStore、独立した2つのトグル)。
  private composingInPanel: boolean = true;
  private composingInField: boolean = true;
  // ホスト側(確定済み、IDLE状態)テキストに対する操作の履歴。挿入・削除
  // どちらも1本のスタックに積み、取消キーは最新のものから本当のctrl+zと
  // 同じ順序で戻す。insertText() を経由するものは全部ここに乗るので、
  // 変換確定・生かな確定・記号/絵文字タップ・予測変換タップ・
  // クリップボード貼り付け...どの経路で入っても等しく取り消せる。
  //
  // at は「このIME自身が動かした分」だけを積算した自己完結の座標で、
  // ホストの絶対カーソル位置とは対応しない。inputClient.getTextIndexAtCursor
  // は実機で誤った値(カーソル後方にテキストがあると文末寄りの値)を返すこと
  // が分かっている(括弧カーソル自動移動機能で実証・撤去済み、コミット
  // 7328e504 参照)ため、ホストの絶対座標には一切頼らない。矢印キー(◄►)
  // による移動は hostCursor に反映して追従できるが、ユーザーが入力欄を
  // 直接タップしてカーソルを動かした場合は検知できずズレる。それでも
  // 「常に今のカーソル位置で実行する」だった以前より、追跡できる範囲では
  // 確実に正しくなる。
  // → HarmonyOS 7 では入力欄の位置が正しく取れる (2026-10-14 実機確認) ので、今は selectionChange で
  //   本当の位置に合わせている (onHostSelection)。指でタップして動かした後も正しい。
  private hostUndoStack: HostEditOp[] = [];
  private hostCursor: number = 0;
  private static readonly MAX_HOST_UNDO = 100;
  private static readonly UNDO_MERGE_MS = 1500;
  private static readonly SELF_EDIT_MS = 400;

  // 入力欄のカーソルが動いた (指でタップ・◄►・文字を入れた)。HarmonyOS 7 では入力欄が知らせる位置が実際と合う
  // (2026-10-14 実機で確認) ので、取消キーの位置 (hostCursor) を入力欄の本当の位置に合わせる。
  // これで指でタップしてカーソルを動かした後でも、取消キーが正しい場所を戻す
  //
  // ただし自分で入れた・消した・動かした直後の知らせは使わない: 自分で数えた分と二重になって、取消キーが
  // 「あえええきききくけこ」のように違う所へ戻した (ユーザー 2026-10-14)。指でタップして動かしたときだけ合わせる
  onHostSelection(nb: number): void {
    if (Date.now() < this.selfEditUntil) { return; }
    if (nb >= 0) { this.hostCursor = nb; }
  }

  // 入力欄の文字が変わった。打っている途中なのに欄が空になった (アプリの送信ボタンで送って欄を空にした) ときは、
  // 打っている途中の状態と候補を捨てる (確定せずに送ると候補が残った。ユーザー 2026-10-16)。
  // 入力欄の文字はアプリが消しているので、こちらでは消し直さない (送った文字やほかの文字を消さないため)
  onHostText(text: string): void {
    if (Date.now() < this.selfEditUntil) { return; }
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state === InputState.IDLE || text.length > 0) { return; }
    this.previewActive = false;   // 入力欄のプレビューは、もうアプリが消している
    this.previewLen = 0;
    this.resetComposition();
  }

  private selfEditUntil: number = 0;
  private markSelfEdit(): void {
    this.selfEditUntil = Date.now() + InputHandler.SELF_EDIT_MS;
  }

  private syncHostCursor(): void {
    try {
      const i = this.inputClient?.getTextIndexAtCursorSync() ?? -1;
      if (i >= 0) { this.hostCursor = i; }
    } catch (_e) { /* 古い OS: 自分で動かした分だけ数える */ }
  }

  // ◄► でカーソルを動かした: 取消キーで元の位置へ戻れるように積む。続けて動かした分は 1 つに (動かす前の位置を残す)
  private pushHostMove(from: number): void {
    const last = this.hostUndoStack.length > 0 ? this.hostUndoStack[this.hostUndoStack.length - 1] : undefined;
    if (last !== undefined && last.kind === 'move') { return; }
    this.hostUndoStack.push({ kind: 'move', text: '', at: from, t: Date.now() });
    if (this.hostUndoStack.length > InputHandler.MAX_HOST_UNDO) {
      this.hostUndoStack = this.hostUndoStack.slice(-InputHandler.MAX_HOST_UNDO);
    }
  }
  // 出す機会を待っている閉じ括弧。空なら保留無し。いつ出るかは
  // bracketCloseMode 次第で、詳しい設計意図は BRACKET_PAIRS のコメント参照。
  // AppStorage にも流しているのは、保留中であることを画面に出すため
  // (KeyboardPage の候補バー行のバッジ参照) -- 見た目に何も変化が無いと、
  // 閉じ括弧がいつ出るのか利用者から分からず不安になる。手動モードでは
  // このバッジ自体が「► で閉じられる」ことの案内も兼ねている。
  private pendingBracketClose: string = '';

  // 括弧の入れ方 ('pair' | 'manual')。設定の実体は
  // SettingsStore 側にあり、ここは反映済みの値を読むだけ。設定画面
  // (パネル内 SettingsBar) と拡張本体は同じプロセスの AppStorage を
  // 共有するので、都度読めば必ず最新になる。
  private bracketCloseMode(): string {
    // 選べるのは 組で (pair)・手動 (manual) の 2 つ。前に保存した 自動 (auto) は 組で に (ユーザー 2026-10-13)
    const m = AppStorage.get<string>('bracketCloseMode') ?? 'pair';
    return m === 'manual' ? 'manual' : 'pair';
  }

  private cursorIntoPair: boolean = false;   // 括弧を組で入れた: 入れ終わったらカーソルを中へ

  // 保留中の閉じ括弧を今この位置へ出す。出したら true、保留が無ければ false。
  // 先に保留を空にしてから insertText を呼ぶのは、insertText 側の
  // 「利用者が自分で閉じ括弧を打った」判定と二重に噛み合わせないため。
  flushPendingBracketClose(): boolean {
    if (this.pendingBracketClose.length === 0) { return false; }
    const close = this.pendingBracketClose;
    InputLog.recordBracket(close, 'flush');
    this.setPendingBracketClose('');
    this.insertText(close, { raw: true } as InsertTextOptions);
    return true;
  }

  // 保留中の閉じ括弧を出さずに捨てる (待機バッジの長押し)。開き括弧だけを
  // 使いたかった場合に、わざわざ出してから消す手間を省くための逃げ道。
  cancelPendingBracketClose(): void {
    InputLog.recordBracket(this.pendingBracketClose, 'cancel');
    this.setPendingBracketClose('');
  }

  setBracketCloseMode(mode: string): void {
    AppStorage.setOrCreate('bracketCloseMode', mode);
    if (this.imeContext) {
      SettingsStore.saveBracketCloseMode(this.imeContext, mode);
    }
  }

  private setPendingBracketClose(v: string): void {
    this.pendingBracketClose = v;
    AppStorage.setOrCreate('pendingBracketClose', v);
  }
  private imeContext: InputMethodExtensionContext | undefined = undefined;
  private learningPrefs: dataPreferences.Preferences | undefined = undefined;
  private learningSaveTimer: number = -1;
  private nextWordPrefs: dataPreferences.Preferences | undefined = undefined;
  private nextWordSaveTimer: number = -1;
  private flowPrefs: dataPreferences.Preferences | undefined = undefined;
  private flowSaveTimer: number = -1;
  private unknownWordPrefs: dataPreferences.Preferences | undefined = undefined;
  private unknownWordSaveTimer: number = -1;
  private suppressedPrefs: dataPreferences.Preferences | undefined = undefined;
  // 前回の書き込み以降に中身が変わったか。変化が無いのに閉じるたび4本
  // 書きに行かないため (flushPendingSaves のコメント参照)。
  private flowDirty: boolean = false;
  private learningDirty: boolean = false;
  private nextWordDirty: boolean = false;
  private unknownWordDirty: boolean = false;
  // 登録したそのままのユーザー辞書 (活用展開する前)。設定パネルの一覧用。
  private userDictRaw: Record<string, string[]> = {};
  // 辞書の自由入力中に、確定文字の送り先を切り替えるための印。
  // '' = ホストアプリ / 'yomi' | 'word' = パネル内のその欄。
  private captureTarget: string = '';
  private suppressedDirty: boolean = false;
  private suppressedSaveTimer: number = -1;
  // The last (up to) two committed words, at word granularity, used as the
  // 1- and 2-word context for next-word prediction/learning (see
  // NextWordPredictor / pushPredictions). Newest last.
  private recentWords: string[] = [];
  // Parallel to the 'predictions' AppStorage strip: the individual word(s) each
  // displayed chip represents (a phrase chip's own words, or [text] for a
  // plain single-word/emoji chip) -- see pushPredictions/commitPrediction.
  private predictionWordGroups: string[][] = [];
  private symbolPrefs: dataPreferences.Preferences | undefined = undefined;
  private symbolSaveTimer: number = -1;
  // Per-symbol tap counts, used to rank the symbol panel's "recent" tab by
  // actual usage instead of a fixed guess. Static so SymbolView's AppStorage
  // read and InputHandler's own updates always see the same data.
  private static symbolFreq: Record<string, number> = {};

  static getInstance(): InputHandler {
    if (!InputHandler._instance) {
      InputHandler._instance = new InputHandler();
    }
    return InputHandler._instance;
  }

  // 拡張プロセスの永続化の初期化。ここが1つの同期ブロックなので、途中で例外が
  // 出ると以降の getPreferences が全部走らず、learningPrefs 等が undefined の
  // まま残る。そうなると saveLearning/saveSuppressed 等は `if (!prefs) return`
  // で黙って何もしなくなり、キーボード自体は動くのに保存だけが全部死ぬ ──
  // 画面からは「全部空」としか見えない。
  // だから、ここに本質的でない処理を足さないこと。
  setContext(ctx: InputMethodExtensionContext): void {
    if (this.imeContext !== undefined) { return; }
    this.imeContext = ctx;
    dataPreferences.getPreferences(this.imeContext, 'shunti_learning')
      .then((prefs: dataPreferences.Preferences) => {
        this.learningPrefs = prefs;
        return prefs.get('v1', '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          const data = JSON.parse(String(val)) as Record<string, Record<string, number>>;
          KanaKanjiConverter.setLearned(data);
        } catch (_e) {
          InputHandler.keepBroken(this.learningPrefs, val);
        }
        this.learningPrefs?.get('day', '{}').then((v: dataPreferences.ValueType) => {
          try { KanaKanjiConverter.setLearnedDay(JSON.parse(String(v)) as Record<string, number>); } catch (_e) {}
        });
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_learning', e); });
    // 文の流れの予測 (SentenceFlow) の覚えた流れ
    dataPreferences.getPreferences(this.imeContext, 'shunti_flow')
      .then((prefs: dataPreferences.Preferences) => {
        this.flowPrefs = prefs;
        return prefs.get('v1', '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          SentenceFlow.setLearned(JSON.parse(String(val)) as Record<string, Record<string, number>>);
        } catch (_e) {
          InputHandler.keepBroken(this.flowPrefs, val);
        }
        this.flowPrefs?.get('sugg', '{}').then((v: dataPreferences.ValueType) => {
          try { AiConverter.suggUse = JSON.parse(String(v)) as Record<string, number>; } catch (_e) {}
        });
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_flow', e); });
    AiConverter.onSuggUse = () => {
      const prefs = this.flowPrefs;
      if (!prefs) { return; }
      prefs.put('sugg', JSON.stringify(AiConverter.suggUse)).then(() => prefs.flush()).catch((_e: Error) => {});
    };
    SentenceFlow.onChange = () => {
      this.flowDirty = true;
      clearTimeout(this.flowSaveTimer);
      this.flowSaveTimer = setTimeout(() => { this.writeFlow(); }, 1000);
    };
    dataPreferences.getPreferences(this.imeContext, 'shunti_nextword')
      .then((prefs: dataPreferences.Preferences) => {
        this.nextWordPrefs = prefs;
        return prefs.get('v1', '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          // Either the v2 {v,uni,bi} store or a legacy flat unigram map; the
          // cast satisfies setLearned's union parameter and setLearned itself
          // decides which shape it is (by checking `.v`) and migrates the old one.
          const data = JSON.parse(String(val)) as Record<string, Record<string, number>>;
          NextWordPredictor.setLearned(data);
        } catch (_e) {
          InputHandler.keepBroken(this.nextWordPrefs, val);
        }
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_nextword', e); });
    dataPreferences.getPreferences(this.imeContext, 'shunti_unknownword')
      .then((prefs: dataPreferences.Preferences) => {
        this.unknownWordPrefs = prefs;
        return prefs.get('v1', '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          const data = JSON.parse(String(val)) as Record<string, Record<string, number>>;
          UnknownWordLearner.setLearned(data);
        } catch (_e) {
          InputHandler.keepBroken(this.unknownWordPrefs, val);
        }
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_unknownword', e); });
    dataPreferences.getPreferences(this.imeContext, SUPPRESSED_STORE)
      .then((prefs: dataPreferences.Preferences) => {
        this.suppressedPrefs = prefs;
        return prefs.get(SUPPRESSED_KEY, '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          const data = JSON.parse(String(val)) as Record<string, string[]>;
          SuppressedCandidates.setSuppressed(data);
        } catch (_e) {
          InputHandler.keepBroken(this.suppressedPrefs, val);
        }
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_suppressed', e); });
    dataPreferences.getPreferences(this.imeContext, 'shunti_symbol_freq')
      .then((prefs: dataPreferences.Preferences) => {
        this.symbolPrefs = prefs;
        return prefs.get('v1', '{}');
      })
      .then((val: dataPreferences.ValueType) => {
        try {
          InputHandler.symbolFreq = JSON.parse(String(val)) as Record<string, number>;
          InputHandler.publishFrequentSymbols();
        } catch (_e) {}
      })
      .catch((e: Error) => { InputLog.recordInit('shunti_symbol_freq', e); });
    this.reloadUserDict();
    this.reloadSettings();
    // 単漢字ずつ組み立て直した語が、実は辞書のどこかに(埋もれて)もう存在した
    // ときの合図。「志岐」のように読み全体では一発で出てこないくらい埋もれて
    // いる語は、UnknownWordLearner からは isKnown()=true で毎回捨てられる一方、
    // 読み全体をひとつの文字列として打つことが無いので recordChoice も一切
    // 呼ばれず、いつまでも埋もれたままになる穴があった。通常の学習ブーストに
    // 回すことでこの穴を塞ぐ。
    UnknownWordLearner.onKnownAssembly = (r: string, s: string) => {
      KanaKanjiConverter.recordChoice(r, s);
      this.saveLearning();
      InputLog.recordKnownAssembly(r, s);
    };
    // 必ず最後。収集の初期化で例外が出ても、上の本来の初期化を巻き込まない。
    this.setupInputLog(ctx);
  }

  // 入力ログの初期化 (デバッグビルドのみ。リリースでは何もしない)。
  //
  // 書き先はアプリ単位の filesDir を先に試し、書けなければモジュール単位に
  // 落とす。どちらに書けたかは diag レコードに残るので、取り出した記録を見れば
  // 分かる。パス文字列が取れても拡張のサンドボックスから書けるとは限らない
  // ので、setWritableDir が実際に開いて確かめる。
  private setupInputLog(ctx: InputMethodExtensionContext): void {
    if (!DEBUG_INPUT_LOG) { return; }
    try {
      let appDir = '';
      try { appDir = ctx.getApplicationContext().filesDir; } catch (_e) { appDir = ''; }
      const used = InputLog.setWritableDir([appDir, ctx.filesDir]);
      console.info('shuntiIME InputLog dir=' + used + ' (module=' + ctx.filesDir + ' app=' + appDir + ')');
      InputLog.recordPaths('setContext',
        used.length > 0 ? used : '書き込めるディレクトリ無し',
        ctx.filesDir, ctx.preferencesDir);
      let appVersion = '';
      try {
        appVersion = bundleManager.getBundleInfoForSelfSync(
          bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT).versionName;
      } catch (_e) { appVersion = ''; }
      InputLog.recordVersion(appVersion);
      UnknownWordLearner.onLearn = (r: string, s: string, confirmed: boolean) => {
        InputLog.recordUnknownWord(r, s, confirmed);
      };
    } catch (e) {
      console.error('shuntiIME InputLog setup failed', JSON.stringify(e));
    }
  }


  // Called whenever the user taps a symbol in SymbolView, so its "recent" tab
  // can be ranked by what this user actually reaches for instead of a fixed
  // guess at common punctuation.
  recordSymbolUse(sym: string): void {
    InputHandler.symbolFreq[sym] = (InputHandler.symbolFreq[sym] ?? 0) + 1;
    // Deliberately does NOT republish the "recent" ordering here: doing so on
    // every tap re-sorted the 🕐 tab in real time, so a key slid out from
    // under the user's finger as they used it. The visible order is snapshotted
    // when the symbol panel opens instead (SymbolView.aboutToAppear →
    // refreshFrequentSymbols), so positions stay put during a tapping session
    // and only settle into their new frequency order the next time it opens.
    clearTimeout(this.symbolSaveTimer);
    this.symbolSaveTimer = setTimeout(() => {
      const prefs = this.symbolPrefs;
      if (!prefs) { return; }
      prefs.put('v1', JSON.stringify(InputHandler.symbolFreq))
        .then(() => prefs.flush())
        .catch((_e: Error) => {});
    }, 2000);
  }

  // Re-sort the "recent" symbol tab by current tap frequency. Called on
  // symbol-panel open (not per tap) so the ordering the user sees is stable
  // while they tap within it -- see recordSymbolUse.
  refreshFrequentSymbols(): void {
    InputHandler.publishFrequentSymbols();
  }

  private static publishFrequentSymbols(): void {
    const syms = Object.keys(InputHandler.symbolFreq);
    syms.sort((a: string, b: string) =>
      (InputHandler.symbolFreq[b] ?? 0) - (InputHandler.symbolFreq[a] ?? 0));
    AppStorage.setOrCreate('frequentSymbols', syms.slice(0, 24));
  }

  // Reload the user dictionary from disk (picks up words added in the main
  // app), plus its 品詞 metadata (DictEditBar's POS chips) --
  // expanded into conjugated forms + connection-matrix class overrides via
  // ConjugationEngine before handing off to KanaKanjiConverter, so a verb/
  // adjective/person/place entry gets real segmentation behaviour instead of
  // the flat noun default (see UserPosStore/ConjugationEngine.ets).
  private reloadUserDict(): void {
    if (!this.imeContext) { return; }
    const ctx = this.imeContext;
    Promise.all([
      UserDictStore.load(ctx, true),
      UserPosStore.load(ctx, true),
    ]).then((results: [Record<string, string[]>, Record<string, UserPosEntry>]) => {
      // Guarded: corrupt/unexpected persisted user data must NEVER take the
      // whole keyboard extension down. buildUserExpansion runs the
      // conjugation engine over on-disk data every launch, so a single bad
      // entry throwing here used to become an unhandled rejection that
      // recurred on every restart until the store was wiped (a
      // "reinstall-only" crash). On any failure, fall back to no user
      // dictionary rather than crashing — the keyboard still works.
      console.info('shuntiIME reloadUserDict loaded');
      // 設定パネルの「登録済み」一覧が出す元データ (展開前の生の登録内容)。
      this.userDictRaw = results[0];
      // 登録した語を AI 変換の網に入れる (AiConverter.setUserDict)
      try {
        AiConverter.setUserDict(results[0], results[1]);
      } catch (e) {
        console.error('shuntiIME reloadUserDict apply failed', JSON.stringify(e));
        AiConverter.setUserDict({}, {});
      }
    }).catch((err: Error) => {
      console.error('shuntiIME reloadUserDict load failed', JSON.stringify(err));
      AiConverter.setUserDict({}, {});
    });
  }

  // Reload keyboard-wide settings from disk (picks up the layout choice made
  // in the main app's settings screen, since that's a separate process).
  private reloadSettings(): void {
    if (!this.imeContext) {
      console.error('shuntiIME reloadSettings: no imeContext');
      return;
    }
    SettingsStore.load(this.imeContext, true)
      .then((s: KeyboardSettings) => {
        console.info('shuntiIME reloadSettings applied', JSON.stringify(s));
        AppStorage.setOrCreate('kanaKeyboardLayout', s.kanaKeyboardLayout);
        AppStorage.setOrCreate('alphabetKeyboardLayout', s.alphabetKeyboardLayout);
        AppStorage.setOrCreate('oneHandedMode', s.oneHandedMode);
        AppStorage.setOrCreate('oneHandedSide', s.oneHandedSide);
        AppStorage.setOrCreate('conversionEngine', s.conversionEngine);
        AppStorage.setOrCreate('numericPadEnabled', s.numericPadEnabled);
        // 提案を分ける・英数字キーボードの変換・キーボード内の表示は設定を廃止して決め打ち
        // (ユーザー 2026-10-14「もういらない」。前に保存した値は無視)
        s.separateSuggest = true;
        s.alphaCompose = true;
        s.composingInPanel = false;  // ひらがなの行は入力欄に出せない欄だけ (常に出していたのは誤り、2026-10-15)
        s.composingInField = true;   // 入力欄に表示も決め打ち (設定は廃止、2026-10-14)
        AppStorage.setOrCreate('separateSuggest', s.separateSuggest);
        AppStorage.setOrCreate('alphaCompose', s.alphaCompose);
        AiConverter.setSuggestMode(s.separateSuggest);
        AppStorage.setOrCreate('hapticEnabled', s.hapticEnabled);
        AppStorage.setOrCreate('themeMode', s.themeMode);
        AppStorage.setOrCreate('keyChromeEnabled', s.keyChromeEnabled);
        AppStorage.setOrCreate('flickHintsEnabled', s.flickHintsEnabled);
        AppStorage.setOrCreate('bracketCloseMode', s.bracketCloseMode);
        this.applyComposingDisplay(s.composingInPanel, s.composingInField);
        this.hapticEnabled = s.hapticEnabled;
        this.probeHapticSupportSilently();
        this.loadAiConverter();
        this.applyThemeMode(s.themeMode);
        // computeKeyHeight/applyKeyboardWidthsForVp は AppStorage の
        // keyboardHeightScale*/flickSideColumnRatio* (縦横別) を読むが、
        // 起動直後の initPanel はこの読み込みより先に (既定値のまま) 一度
        // 走っていることがある。ここで両方向ぶん値を入れたうえで
        // resizePanel を呼び直し、保存済みの調整を確実に反映させる。
        AppStorage.setOrCreate('keyboardHeightScalePortrait', s.keyboardHeightScalePortrait);
        AppStorage.setOrCreate('keyboardHeightScaleLandscape', s.keyboardHeightScaleLandscape);
        AppStorage.setOrCreate('flickSideColumnRatioPortrait', s.flickSideColumnRatioPortrait);
        AppStorage.setOrCreate('flickSideColumnRatioLandscape', s.flickSideColumnRatioLandscape);
        AppStorage.setOrCreate('keyboardWidthScale', s.keyboardWidthScale);
        AppStorage.setOrCreate('keyboardHorizontalOffset', s.keyboardHorizontalOffset);
        this.syncFloatingFromSettings(s);
      });
  }

  // フローティングの位置・大きさ・ON/OFF を設定値と揃える。
  // KeyboardController.reloadSettings (起動・inputStart毎) と
  // KeyboardPage.reloadSettingsIntoStorage (パネルUI再構築毎) の両方から
  // 呼ぶための共通処理 -- 別プロセスの本体アプリ側で設定が変わった場合の
  // 反映経路が2つあるので、ロジックの二重実装を避けてここに集約した。
  syncFloatingFromSettings(s: KeyboardSettings): void {
    AppStorage.setOrCreate('floatingXFrac', s.floatingXFrac);
    AppStorage.setOrCreate('floatingYFrac', s.floatingYFrac);
    AppStorage.setOrCreate('floatingWFrac', s.floatingWFrac);
    AppStorage.setOrCreate('floatingHFrac', s.floatingHFrac);
    // 前回の起動/表示から floatingMode が変わっているときだけ changeFlag を
    // 伴う setFloatingMode を呼ぶ (それ自体が内部で resizePanel 相当の
    // 再配置もする)。変わっていなければ通常の resizePanel だけで足りる。
    if ((AppStorage.get<boolean>('floatingMode') ?? false) !== s.floatingMode) {
      this.kbCtrlRef?.setFloatingMode(s.floatingMode);
    } else {
      this.kbCtrlRef?.resizePanel();
    }
  }

  // AI 変換を読み込む (何度呼んでもよい。読み込みは 1 回だけ、裏のスレッドで)。読み込みが済むまでは候補は読みのまま
  private loadAiConverter(): void {
    if (this.imeContext) {
      AiConverter.ensureLoaded(this.imeContext, () => {
        console.error('shuntiIME AiConverter unavailable: candidates stay as typed');
      });
    }
  }

  // One-handed mode is toggled from the keyboard's own settings panel, so it
  // can persist AppStorage's current value directly rather than re-reading it.
  setOneHandedMode(enabled: boolean, side: string): void {
    InputLog.recordMode(`onehand=${enabled ? side : 'off'}`);
    AppStorage.setOrCreate('oneHandedMode', enabled);
    AppStorage.setOrCreate('oneHandedSide', side);
    if (this.imeContext) {
      SettingsStore.saveOneHanded(this.imeContext, enabled, side);
    }
  }

  // Toggle one-handed mode for a given side, driven by the same key-swipe both
  // to enter and to leave: an up-swipe engages that side, and repeating it while
  // already leaning that way returns to full width. Swiping the other side's key
  // switches sides directly.
  toggleOneHanded(side: string): void {
    const on = AppStorage.get<boolean>('oneHandedMode') ?? false;
    const cur = AppStorage.get<string>('oneHandedSide') ?? 'right';
    if (on && cur === side) {
      this.setOneHandedMode(false, side);
    } else {
      this.setOneHandedMode(true, side);
    }
  }

  // Switching the keyboard layout from the keyboard's own settings panel
  // takes effect immediately (same process, no reload round-trip needed) —
  // more reliable than the main-app toggle, which depends on the extension
  // picking the change back up on its own next reload. かな/アルファベット are
  // independent settings (each can be QWERTY or FLICK on its own).
  setKanaKeyboardLayout(layout: string): void {
    AppStorage.setOrCreate('kanaKeyboardLayout', layout);
    if (this.imeContext) {
      SettingsStore.saveKanaKeyboardLayout(this.imeContext, layout);
    }
  }

  setAlphabetKeyboardLayout(layout: string): void {
    AppStorage.setOrCreate('alphabetKeyboardLayout', layout);
    if (this.imeContext) {
      SettingsStore.saveAlphabetKeyboardLayout(this.imeContext, layout);
    }
  }

  // Same immediate-switch pattern as setOneHandedMode/setKanaKeyboardLayout
  // above. Toggling this off while the 数字パッド (numeric subMode) happens
  // to be open would strand the user on a panel cycleInputMode's あA key can
  // no longer reach -- fall back to kana so the setting and the panel
  // actually on screen never disagree.
  // ---- 打鍵時の振動 -------------------------------------------------------
  //
  // 設定はキーボード側で持つ (本体アプリとはデータを共有できないため)。
  // 実際に振動させるかは毎回 AppStorage を読まず、この場に控えた値で判断する。
  // 打鍵ごとに走る経路なので、余計な読み出しを挟まない。
  private hapticEnabled: boolean = false;

  setHapticEnabled(enabled: boolean): void {
    // 使えないと分かっている端末ではオンにしない (トグルはグレーアウトしてあるが、念のためここでも止める)
    if (enabled && (AppStorage.get<boolean>('hapticUnavailable') ?? false)) {
      AppStorage.setOrCreate('hapticEnabled', false);
      return;
    }
    this.hapticEnabled = enabled;
    AppStorage.setOrCreate('hapticEnabled', enabled);
    if (this.imeContext) {
      SettingsStore.saveHapticEnabled(this.imeContext, enabled);
    }
    // 切り替えた手応えとして一度鳴らす。オンにしたことがその場で分かる。
    // ここで方式の判定も走る。
    if (enabled) { this.hapticMode = 0; this.hapticTick(); }
  }

  // 振動の鳴らし方。最初の1回で試して決める。
  //   0 = 未判定 / 1 = プリセット効果 / 2 = 時間指定 / 3 = 旧FA系 / -1 = 鳴らせない
  //
  // 現状のHarmonyOSでは、この経路はまず通らない。入力方式の拡張プロセスは
  // SELinux上 input_isolate_hap (デバッグ版は input_isolate_debug_hap)
  // というドメインで動き、そこから振動サービスへの接続が明示的に禁じられている。
  //
  //   allow { normal_hap_attr -input_isolate_attr }
  //         sa_miscdevice_service:samgr_class { get };
  //
  // 実機のログにも avc: denied { get } ... sa_miscdevice_service が出て、
  // startVibration は 14600101 (Device operation failed) で返る。
  // 権限 (VIBRATE) は付与済みで、同じ端末でも本体アプリからは鳴る。つまり
  // 権限でも端末差でもなく、プロセスの種別による遮断であり、アプリ側では
  // 解決できない (リリース版でも同じドメイン属性なので配信しても変わらない)。
  //
  // OSが方針を変えれば以下がそのまま動くため、実装は残してある。
  private hapticMode: number = 0;

  // 実際には鳴らさずに、端末が振動そのものに対応しているかだけを見る。
  // 設定画面を開いた時点で「この端末では使えない」と分かるようにして、
  // トグル自体をグレーアウトするために使う (「非対応機種は自動で検出して
  // 設定自体をグレーアウトしてほしい」)。OS が IME 拡張プロセスからの振動を
  // 許可していない場合はここでは分からないので、実際に鳴らそうとして初めて
  // 判明する従来の経路 (probeHaptic / markHapticUnavailable) も残してある。
  //
  // 効果の有無だけでは足りない: 振動できる端末でも、上に書いたとおり OS が
  // キーボードのプロセスから振動サービスへの接続を禁じているので、この判定を
  // 通ってしまい、効かないトグルをオンにできていた。そこで振動を「止める」
  // 命令も送ってみる。止めるだけなので鳴らないが、振動サービスへの接続は
  // 鳴らすときと同じなので、禁じられていればここで失敗して分かる
  // (鳴っていないときに止めても、接続できれば成功で返る)。
  probeHapticSupportSilently(): void {
    try {
      if (!vibrator.isSupportEffectSync('haptic.clock.timer')) {
        this.markHapticUnavailable();
        return;
      }
    } catch (_e) {
      // 判定APIそのものが無い/失敗した端末では、下の接続の確認に任せる。
    }
    try {
      vibrator.stopVibration()
        .then(() => {
          console.info('shuntiIME haptic probe: vibrator reachable');
        })
        .catch((e: ESObject) => {
          console.info('shuntiIME haptic probe: unavailable ' + JSON.stringify(e));
          this.markHapticUnavailable();
        });
    } catch (e) {
      console.info('shuntiIME haptic probe: unavailable (sync) ' + JSON.stringify(e));
      this.markHapticUnavailable();
    }
  }

  // startVibration は Promise で失敗を返すだけでなく、その場で例外を投げる
  // 端末もある。以前は例外を受けていなかったので、投げられるとオフに戻す処理
  // (markHapticUnavailable) まで届かず、効かないのにオンのまま残っていた。
  private probeHaptic(): void {
    // 2) 時間指定 + usage:'touch'。物理フィードバック扱いを避ける。
    const tryTime = (): void => {
      try {
        vibrator.startVibration({ type: 'time', duration: 30 }, { id: 0, usage: 'touch' })
          .then(() => { this.hapticMode = 2; })
          .catch((_e2: ESObject) => { this.markHapticUnavailable(); });
      } catch (_e) {
        this.markHapticUnavailable();
      }
    };
    // 1) プリセット効果。端末が用意している「カチッ」を借りる方式。
    try {
      vibrator.startVibration({ type: 'preset', effectId: 'haptic.clock.timer', count: 1 },
        { id: 0, usage: 'physicalFeedback' })
        .then(() => { this.hapticMode = 1; })
        .catch((_e1: ESObject) => { tryTime(); });
    } catch (_e) {
      tryTime();
    }
  }

  // どの方式でも鳴らせなかった。
  //
  // 効かない設定を残しても混乱させるだけなので、自動でオフに戻して
  // 「この端末では使えない」と示せるようにする。
  private markHapticUnavailable(): void {
    this.hapticMode = -1;
    this.hapticEnabled = false;
    AppStorage.setOrCreate('hapticEnabled', false);
    AppStorage.setOrCreate('hapticUnavailable', true);
    if (this.imeContext) { SettingsStore.saveHapticEnabled(this.imeContext, false); }
  }

  // キーを押した瞬間に呼ぶ。判定済みの方式で鳴らす。
  // 鳴らせないと分かっている端末では何もしない (毎回失敗を投げても無駄なので)。
  hapticTick(): void {
    if (!this.hapticEnabled) { return; }
    if (this.hapticMode === -1) { return; }
    if (this.hapticMode === 0) { this.probeHaptic(); return; }
    const effect: ESObject = this.hapticMode === 1
      ? { type: 'preset', effectId: 'haptic.clock.timer', count: 1 }
      : { type: 'time', duration: 30 };
    const usage: string = this.hapticMode === 1 ? 'physicalFeedback' : 'touch';
    try {
      vibrator.startVibration(effect as ESObject, { id: 0, usage: usage } as ESObject)
        .catch((_e: ESObject) => {});
    } catch (_e) {
      // 非対応端末。鳴らないだけで害は無い。
    }
  }

  // キーボードのライト／ダーク。即時反映し、OS のカラーモード変更は
  // 'system' のときだけ追従する (applyColorMode / applyColorModeFrom)。
  setThemeMode(mode: string): void {
    AppStorage.setOrCreate('themeMode', mode);
    this.applyThemeMode(mode);
    if (this.imeContext) {
      SettingsStore.saveThemeMode(this.imeContext, mode);
    }
  }

  private applyThemeMode(mode: string): void {
    if (!this.imeContext) {
      AppStorage.setOrCreate('isDark', resolveIsDark(mode, AppStorage.get<boolean>('isDark') ?? false));
      return;
    }
    AppStorage.setOrCreate('isDark', resolveIsDark(mode, isDarkFromResourceManager(this.imeContext as ESObject)));
  }

  setKeyChromeEnabled(enabled: boolean): void {
    AppStorage.setOrCreate('keyChromeEnabled', enabled);
    if (this.imeContext) {
      SettingsStore.saveKeyChromeEnabled(this.imeContext, enabled);
    }
  }

  // フリックキーの十字ヒント表示。フリック自体はタッチのジェスチャで判定
  // しており見た目だけの設定なので、切り替えても即座に反映すればよい。
  setFlickHintsEnabled(enabled: boolean): void {
    AppStorage.setOrCreate('flickHintsEnabled', enabled);
    if (this.imeContext) {
      SettingsStore.saveFlickHintsEnabled(this.imeContext, enabled);
    }
  }

  setNumericPadEnabled(enabled: boolean): void {
    AppStorage.setOrCreate('numericPadEnabled', enabled);
    if (!enabled && AppStorage.get<string>('keyboardSubMode') === 'numeric') {
      AppStorage.setOrCreate('keyboardSubMode', 'kana');
    }
    if (this.imeContext) {
      SettingsStore.saveNumericPadEnabled(this.imeContext, enabled);
    }
  }

  // Shared "あA"/"Aあ" mode-switch key handler for both FlickKeyboardView and
  // KeyboardView. With 数字パッド enabled this cycles 日本語→アルファベット→数字→
  // (back to 日本語 via NumericPadView's exit key); with it disabled the key
  // only toggles 日本語⇔アルファベット, matching the pre-numeric-pad behavior.
  cycleInputMode(): void {
    const inputMode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
    const numericPadEnabled = AppStorage.get<boolean>('numericPadEnabled') ?? false;
    if (inputMode === InputMode.ALPHANUMERIC) {
      if (numericPadEnabled) {
        AppStorage.setOrCreate('keyboardSubMode', 'numeric');
        InputLog.recordMode('input=numeric');
      } else {
        AppStorage.setOrCreate('inputMode', InputMode.HIRAGANA);
        InputLog.recordMode(`input=${InputMode.HIRAGANA}`);
      }
      return;
    }
    AppStorage.setOrCreate('inputMode', InputMode.ALPHANUMERIC);
    InputLog.recordMode(`input=${InputMode.ALPHANUMERIC}`);
  }

  // 変換方式は AI だけ。設定の読み直し (KeyboardPage) から呼ばれたら、AI 変換の読み込みを確かめるだけ
  setConversionEngine(_engine: string): void {
    AppStorage.setOrCreate('conversionEngine', 'ai');
    this.loadAiConverter();
  }

  // 保存せずに反映だけする。設定の読み直しは拡張側 (reloadSettings) と
  // パネル側 (KeyboardPage.reloadSettingsIntoStorage) の両方から走るので、
  // 読み直しのたびに書き戻さないよう保存と分けてある。
  applyComposingDisplay(inPanel: boolean, inField: boolean): void {
    this.composingInPanel = inPanel;
    this.composingInField = inField;
    AppStorage.setOrCreate('composingInPanel', inPanel);
    AppStorage.setOrCreate('composingInField', inField);
    // 入力欄側をOFFにした瞬間、出したままのプレビューを消す。
    if (!inField) { this.cancelPreviewText(); }
    this.publishPreviewMode();
  }

  // レイアウト調整モード (設定 → レイアウト調整) からの参照。パネルの
  // 実サイズ変更 (panel.resize) はこのクラスからはできず、それを持っている
  // KeyboardController (拡張本体) へ委譲する必要があるので、生成時に
  // 向こうから自分を渡してもらう (KeyboardController.onCreate 参照)。
  private kbCtrlRef: KeyboardController | undefined = undefined;

  setKeyboardControllerRef(ctrl: KeyboardController): void {
    this.kbCtrlRef = ctrl;
  }

  // 縦の長さの調整ハンドル (LayoutAdjustBar) からドラッグ中に毎フレーム
  // 呼ばれる想定。保存は別 (saveKeyboardHeightScale) にして、ここは
  // AppStorage の書き換えとパネルの実リサイズだけをその場でやる。
  // 連打の間引きは resizePanel 側 (KeyboardController、setTimeoutベースで
  // 詰まらないよう組んである) が引き受けるので、ここでは毎回呼んでよい。
  // 縦向き/横向きで別の値を持つ (画面が横長で背が低くなりがち、特殊キー列
  // の見え方も違うため)。呼び出し元 (LayoutAdjustBar/FlickKeyboardView) は
  // 自分の isLandscape を @Prop/@StorageLink で既に知っているので渡す。
  applyKeyboardHeightScaleLive(scale: number, isLandscape: boolean): void {
    AppStorage.setOrCreate(isLandscape ? 'keyboardHeightScaleLandscape' : 'keyboardHeightScalePortrait', scale);
    this.kbCtrlRef?.resizePanel();
  }

  saveKeyboardHeightScale(scale: number, isLandscape: boolean): void {
    AppStorage.setOrCreate(isLandscape ? 'keyboardHeightScaleLandscape' : 'keyboardHeightScalePortrait', scale);
    this.kbCtrlRef?.resizePanel();
    if (this.imeContext) {
      SettingsStore.saveKeyboardHeightScale(this.imeContext, scale, isLandscape);
    }
  }

  // 中央3列 : 左右特殊キー1列 の比率スライダー。パネル自体のサイズは
  // 変わらないので、幅の再計算 (applyKeyboardWidthsForVp) だけで足りる。
  applyFlickSideRatioLive(ratio: number, isLandscape: boolean): void {
    AppStorage.setOrCreate(isLandscape ? 'flickSideColumnRatioLandscape' : 'flickSideColumnRatioPortrait', ratio);
    const vp = AppStorage.get<number>('keyboardContentVp') ?? 0;
    if (vp <= 0) { return; }
    const isTabletClass = AppStorage.get<boolean>('isTabletClass') ?? false;
    applyKeyboardWidthsForVp(vp, isTabletClass, isLandscape);
  }

  saveFlickSideRatio(ratio: number, isLandscape: boolean): void {
    this.applyFlickSideRatioLive(ratio, isLandscape);
    if (this.imeContext) {
      SettingsStore.saveFlickSideRatio(this.imeContext, ratio, isLandscape);
    }
  }

  // タブレット/フォルダブルの展開画面だけに効く、キーボード最大幅の倍率。
  // isTabletClass でない (普通のスマホの) 端末では applyKeyboardWidthsForVp
  // 側で無視されるので、ここで弾かず常に書き込んでよい。
  applyKeyboardWidthScaleLive(scale: number): void {
    AppStorage.setOrCreate('keyboardWidthScale', scale);
    const rawVp = AppStorage.get<number>('keyboardRawContentVp') ?? 0;
    if (rawVp <= 0) { return; }
    const isTabletClass = AppStorage.get<boolean>('isTabletClass') ?? false;
    const isLandscape = AppStorage.get<boolean>('isLandscape') ?? false;
    applyKeyboardWidthsForVp(rawVp, isTabletClass, isLandscape);
  }

  saveKeyboardWidthScale(scale: number): void {
    this.applyKeyboardWidthScaleLive(scale);
    if (this.imeContext) {
      SettingsStore.saveKeyboardWidthScale(this.imeContext, scale);
    }
  }

  // キーボード本体の左右寄せ (-1 = 左端、0 = 中央、+1 = 右端)。パネルの
  // 大きさもキーの幅も変わらず、余白のどちら側に寄せるかだけの話なので、
  // 実リサイズも幅の再計算も要らない -- AppStorage を書き換えれば
  // KeyboardPage 側が自分で位置を付け替える。
  applyKeyboardHorizontalOffsetLive(offset: number): void {
    AppStorage.setOrCreate('keyboardHorizontalOffset', Math.min(1, Math.max(-1, offset)));
  }

  saveKeyboardHorizontalOffset(offset: number): void {
    this.applyKeyboardHorizontalOffsetLive(offset);
    if (this.imeContext) {
      SettingsStore.saveKeyboardHorizontalOffset(this.imeContext, offset);
    }
  }

  // 設定画面のリセットボタン。縦横どちらの調整帯からでも呼べるが、
  // 「リセット」に縦横の区別を出していないので両方向とも既定 (1.0) へ戻す。
  resetLayoutAdjust(): void {
    this.saveKeyboardHeightScale(1.0, false);
    this.saveKeyboardHeightScale(1.0, true);
    this.saveFlickSideRatio(1.0, false);
    this.saveFlickSideRatio(1.0, true);
    this.saveKeyboardWidthScale(1.0);
    this.saveKeyboardHorizontalOffset(0);
  }

  // フローティングキーボード。固定⇔フローティングの切替そのものは
  // panel.changeFlag を持っている KeyboardController 側でしかできない
  // (このクラスは拡張本体の参照を kbCtrlRef に持っているだけ)。
  // 設定画面のトグルは isOn を明示的に渡す (setKeyChromeEnabled 等の他の
  // トグルと同じ形)。設定ボタン長押し→上スライドのジェスチャは現在値を
  // 反転させるだけでよいので toggleFloatingMode を使う。
  setFloatingModeExplicit(enabled: boolean): void {
    InputLog.recordMode(`floating=${enabled ? 'on' : 'off'}`);
    this.kbCtrlRef?.setFloatingMode(enabled);
    if (this.imeContext) {
      SettingsStore.saveFloatingMode(this.imeContext, enabled);
    }
  }

  toggleFloatingMode(): void {
    this.setFloatingModeExplicit(!(AppStorage.get<boolean>('floatingMode') ?? false));
  }

  // 上際ハンドルのドラッグ中に毎フレーム呼ばれる。保存は onActionEnd から
  // saveFloatingRect で別に行う (他のライブ系ハンドルと同じ分担)。
  moveFloatingPanelLive(startXFrac: number, startYFrac: number, offsetXVp: number, offsetYVp: number): void {
    this.kbCtrlRef?.moveFloatingPanelLive(startXFrac, startYFrac, offsetXVp, offsetYVp);
  }

  // 上際ハンドルを離した瞬間。画面下へ大きくはみ出す位置まで引っ張って
  // いたら通常のドッキング状態に戻し、そうでなければ普通に保存する
  // (KeyboardController.endFloatingMoveDrag 参照)。
  endFloatingMoveDrag(startYFrac: number, offsetYVp: number): void {
    this.kbCtrlRef?.endFloatingMoveDrag(startYFrac, offsetYVp);
  }

  // ドラッグ中、いま離したらドックに戻るかを 0〜1 の連続値でライブに
  // 問い合わせる。KeyboardPage の「下から這い出てくる」プレビュー帯の
  // 高さにそのまま使う。
  checkFloatingDockBack(startYFrac: number, offsetYVp: number): number {
    return this.kbCtrlRef?.checkFloatingDockBack(startYFrac, offsetYVp) ?? 0;
  }

  // 四隅のリサイズハンドルのドラッグ中に毎フレーム呼ばれる。
  resizeFloatingPanelLive(corner: 'tl' | 'tr' | 'bl' | 'br',
      startXFrac: number, startYFrac: number, startWFrac: number, startHFrac: number,
      offsetXVp: number, offsetYVp: number): void {
    this.kbCtrlRef?.resizeFloatingPanelLive(corner, startXFrac, startYFrac, startWFrac, startHFrac, offsetXVp, offsetYVp);
  }

  // ドラッグ/リサイズが終わった時点の (クランプ済みの) 現在値を保存する。
  saveFloatingRect(): void {
    if (!this.imeContext) { return; }
    const xFrac = AppStorage.get<number>('floatingXFrac') ?? 0.2;
    const yFrac = AppStorage.get<number>('floatingYFrac') ?? 0.12;
    const wFrac = AppStorage.get<number>('floatingWFrac') ?? 0.6;
    const hFrac = AppStorage.get<number>('floatingHFrac') ?? 0.35;
    SettingsStore.saveFloatingRect(this.imeContext, xFrac, yFrac, wFrac, hFrac);
  }

  // ドラッグ/リサイズの開始・終了。KeyboardController.beginFloatingDrag の
  // コメント参照 -- 「反復横跳び」対策として、ドラッグ中は画面サイズの
  // 読み直しを止めて固定するための区間マーカー。
  beginFloatingDrag(): void {
    this.kbCtrlRef?.beginFloatingDrag();
  }

  endFloatingDrag(): void {
    this.kbCtrlRef?.endFloatingDrag();
  }

  // 学習系の保存はどれもデバウンスされている。打鍵のたびに数万件の JSON を
  // 書き出すわけにいかないので、それ自体は正しい。問題は「予約したまま消える」
  // 経路が塞がれていなかったこと。
  //
  //   入力 -> 2秒後に書く予約 -> キーボードが閉じる/拡張が落とされる
  //        -> タイマーごと消える -> 一度も書かれない
  //
  // IME拡張は入力欄を離れるたびに落とされうるので、これは例外ケースではなく
  // 通常の経路だった。学習も削除した候補もメモリ上では効くため、使っている間は
  // 正常に見えて、次の起動には何も残っていない ── 「削除した候補」がずっと空
  // だったのも、5つのストアが揃って空だったのもこれ。
  // 対処は flushPendingSaves()。セッション終了時と onDestroy で必ず呼ぶ。
  //
  // ただし flushPendingSaves() は onInputStop/onDestroy が実際に呼ばれる
  // 正常終了の経路にしか効かない。OS がプロセスを強制終了する経路
  // (メモリ逼迫など、onInputStop を経由しない) では、デバウンス待ちの間隔
  // そのものが「学習が失われる窓」として残り続ける。
  // 実データで確認済み: 「し」を選んで「志」を学習した13日後、同じ「志」を
  // 再度選んだ記録が学習前の並び順 (18/40) から始まっていた ── その1回目の
  // 保存がどこかで消えていたことになる。候補削除 (suppressedSaveTimer) は
  // 元々 500ms だったのに、学習系だけ 2000ms のままだったのは単なる
  // 不揃いで、学習が候補削除より軽視していい理由は無い。窓を揃えて縮める。
  //
  // dirty フラグは、変化が無いのに閉じるたび4本書きに行かないためのもの。

  private writeLearning(): void {
    if (!this.learningDirty) { return; }
    const prefs = this.learningPrefs;
    if (!prefs) { return; }
    this.learningDirty = false;
    prefs.put('v1', JSON.stringify(KanaKanjiConverter.getLearned()))
      .then(() => prefs.put('day', JSON.stringify(KanaKanjiConverter.getLearnedDay())))
      .then(() => prefs.flush())
      // 書けなかったら次の機会に書き直す (前は dirty を先に落として学習が消えた。2026-10-25)
      .catch((_e: Error) => { this.learningDirty = true; });
  }

  // 読めなかった保存 (壊れた JSON) を、次の保存で上書きする前に別のキーへ写して残す (PC 版の .broken と同じ。2026-10-25)
  private static keepBroken(prefs: dataPreferences.Preferences | undefined, val: dataPreferences.ValueType): void {
    if (!prefs) { return; }
    const raw = String(val);
    if (raw.length === 0 || raw === '{}') { return; }
    prefs.put('broken-' + Math.floor(Date.now() / 1000), raw).then(() => prefs.flush()).catch((_e: Error) => {});
  }

  // 文の流れ (SentenceFlow) も同じ形で。前は flushPendingSaves の外で、閉じると 1 秒待ちの分が消えた (2026-10-25)
  private writeFlow(): void {
    if (!this.flowDirty) { return; }
    const prefs = this.flowPrefs;
    if (!prefs) { return; }
    this.flowDirty = false;
    prefs.put('v1', JSON.stringify(SentenceFlow.getLearned()))
      .then(() => prefs.flush())
      .catch((_e: Error) => { this.flowDirty = true; });
  }

  private saveLearning(): void {
    this.learningDirty = true;
    clearTimeout(this.learningSaveTimer);
    this.learningSaveTimer = setTimeout(() => { this.writeLearning(); }, 500);
  }

  private writeNextWord(): void {
    if (!this.nextWordDirty) { return; }
    const prefs = this.nextWordPrefs;
    if (!prefs) { return; }
    this.nextWordDirty = false;
    prefs.put('v1', JSON.stringify(NextWordPredictor.getLearned()))
      .then(() => prefs.flush())
      // 書けなかったら次の機会に書き直す (前は dirty を先に落として学習が消えた。2026-10-25)
      .catch((_e: Error) => { this.nextWordDirty = true; });
  }

  private saveNextWord(): void {
    this.nextWordDirty = true;
    clearTimeout(this.nextWordSaveTimer);
    this.nextWordSaveTimer = setTimeout(() => { this.writeNextWord(); }, 500);
  }

  private writeUnknownWords(): void {
    if (!this.unknownWordDirty) { return; }
    const prefs = this.unknownWordPrefs;
    if (!prefs) { return; }
    this.unknownWordDirty = false;
    prefs.put('v1', JSON.stringify(UnknownWordLearner.getLearned()))
      .then(() => prefs.flush())
      // 書けなかったら次の機会に書き直す (前は dirty を先に落として学習が消えた。2026-10-25)
      .catch((_e: Error) => { this.unknownWordDirty = true; });
  }

  private saveUnknownWords(): void {
    this.unknownWordDirty = true;
    clearTimeout(this.unknownWordSaveTimer);
    this.unknownWordSaveTimer = setTimeout(() => { this.writeUnknownWords(); }, 500);
  }

  // ---- 辞書登録 (キーボード内で完結させるための API) ----------------------
  //
  // 本体アプリの辞書画面から登録した語は、キーボードには一切届かない。
  // 入力方式の拡張は別プロセスかつ別サンドボックスで動いており、本体アプリが
  // 書いた preferences を拡張から読むと空になる (実機で確認済み: 本体側には
  // 160B の shunti_userdict があるのに、拡張の reloadUserDict は 0 件)。
  // プラットフォーム側の共有手段 (dataGroupIds + getGroupDir) は応用市場での
  // 発行が前提で、このアカウントでは開放されていない。
  // よって登録も一覧も、変換を行っているこの拡張プロセス側で完結させる。
  //
  // 登録候補は UnknownWordLearner が既に持っている「ユーザーが組み立てたが
  // 辞書に無い語」をそのまま使う。読みと表記を手で打ち直させる必要が無く、
  // パネル内に自由入力欄を作らずに済む。

  // 登録候補 (組み立てられた未知語) を出現回数の多い順に。
  // 文字列1本 "よみ\t表記\t回数" で返すのは、ArkTS の @StorageLink に
  // 構造体配列を載せると更新検知が効きにくいため。
  // 1回しか組み立てられていないものは出さない。
  //
  // 連続して打った2語は、1語を組み立てた場合と構造上まったく区別できない。
  // 実際「丸一日」と「本山」を続けて打っただけで「丸一日本山」が候補に上がって
  // いた。判定で弾くことはできない (春+知 のような本当に登録したい組み立ても
  // 同じ形になる) ので、回数で見る。偶然の隣接は繰り返されないが、実際に使う
  // 語は何度も組み立て直すことになる。
  private static readonly SUGGEST_MIN_COUNT = 2;

  getUnknownWordSuggestions(): string[] {
    const counts = UnknownWordLearner.getLearned();
    const rows: string[] = [];
    for (const reading of Object.keys(counts)) {
      const entry = counts[reading];
      for (const surface of Object.keys(entry)) {
        if (entry[surface] < InputHandler.SUGGEST_MIN_COUNT) { continue; }
        rows.push(`${reading}\t${surface}\t${entry[surface]}`);
      }
    }
    rows.sort((a: string, b: string) => {
      const ca = Number.parseInt(a.split('\t')[2], 10);
      const cb = Number.parseInt(b.split('\t')[2], 10);
      return cb - ca;
    });
    return rows;
  }

  // 登録済みユーザー辞書を "よみ\t表記" で。変換器に渡すのは活用展開後の辞書
  // なので、一覧には reloadUserDict が控えておいた「登録したそのままの内容」を使う。
  getUserDictRows(): string[] {
    const dict = this.userDictRaw;
    const rows: string[] = [];
    for (const reading of Object.keys(dict)) {
      for (const surface of dict[reading]) { rows.push(`${reading}\t${surface}`); }
    }
    rows.sort();
    return rows;
  }

  // AppStorage 経由で UI に配る。パネルは @StorageLink でこれを見る。
  publishDictRows(): void {
    AppStorage.setOrCreate('dictSuggestions', this.getUnknownWordSuggestions());
    AppStorage.setOrCreate('dictRegistered', this.getUserDictRows());
  }

  // 1件を本登録する。拡張自身の preferences に書くので、次の reloadUserDict で
  // そのまま変換に反映される。登録済みの語は未知語リストから外す。
  // 品詞つきで登録する。品詞は活用展開 (ConjugationEngine.buildUserExpansion) を
  // 駆動するので、動詞や形容詞は指定しないと「〜ます」「〜た」等で引けない。
  // pos が 'noun' のときは UserPosStore 側で保存しない (既定扱い)。
  registerUserWordWithPos(reading: string, surface: string, pos: string, group: string): void {
    if (!this.imeContext) { return; }
    if (pos === 'noun') {
      this.registerUserWord(reading, surface);
      return;
    }
    const ctx = this.imeContext;
    const entry: UserPosEntry = group.length > 0
      ? { pos: pos as PosKind, group: group as VerbGroup }
      : { pos: pos as PosKind };
    UserPosStore.load(ctx, true)
      .then((map: Record<string, UserPosEntry>) => {
        const next = UserPosStore.set(map, reading, surface, entry);
        return UserPosStore.save(ctx, next);
      })
      .then(() => { this.registerUserWord(reading, surface); })
      .catch((e: ESObject) => {
        console.error('shuntiIME registerUserWordWithPos failed', JSON.stringify(e));
        // 品詞が保存できなくても、語そのものは登録しておく。
        this.registerUserWord(reading, surface);
      });
  }

  registerUserWord(reading: string, surface: string): void {
    if (!this.imeContext) {
      console.error('shuntiIME registerUserWord: imeContext なし');
      return;
    }
    if (reading.length === 0 || surface.length === 0) { return; }
    const ctx = this.imeContext;
    UserDictStore.load(ctx, true)
      .then((dict: Record<string, string[]>) => {
        const next: Record<string, string[]> = {};
        for (const k of Object.keys(dict)) { next[k] = dict[k]; }
        const cur = next[reading];
        if (cur === undefined) {
          next[reading] = [surface];
        } else if (cur.indexOf(surface) < 0) {
          const merged: string[] = [surface];
          for (const w of cur) { merged.push(w); }
          next[reading] = merged;
        }
        return UserDictStore.save(ctx, next).then(() => next);
      })
      .then((next: Record<string, string[]>) => {
        // 一覧は保存できた内容から直接作る。reloadUserDict の完了を待って
        // タイマーで配り直す形にしていたが、間に合わないと登録直後の一覧が
        // 古いままになり「登録できていない」ように見える。
        this.userDictRaw = next;
        UnknownWordLearner.forget(reading, surface);
        this.saveUnknownWords();
        this.publishDictRows();
        // 変換への反映 (活用展開) は別途。こちらは表示に影響しない。
        this.reloadUserDict();
        console.info('shuntiIME registerUserWord ok', reading + '→' + surface,
          '登録数=' + String(Object.keys(next).length));
      })
      .catch((e: ESObject) => { console.error('shuntiIME registerUserWord failed', JSON.stringify(e)); });
  }

  // ---- 自由入力 (任意の読み・単語を手で入れる) --------------------------
  //
  // '' = 通常 (確定文字はホストアプリへ) / 'yomi' | 'word' = その欄へ。
  // 実際の振り分けは insertText と handleBackspace で行う。

  // 設定パネルはキーボードの代わりに全面を占有するため、その中に入力欄を置いても
  // 打つキーが無い。登録欄はキーボードを残したまま候補バーの位置に出す
  // (DictEditBar)。ここではそのモードに入り、設定パネルを畳んで、最初の欄を
  // 掴むところまでを行う。
  beginDictEdit(): void {
    this.clearDictEdit();
    AppStorage.setOrCreate('keyboardSubMode', 'kana');
    AppStorage.setOrCreate('dictEditMode', true);
    this.startCapture('yomi');
  }

  endDictEdit(): void {
    this.stopCapture();
    AppStorage.setOrCreate('dictEditMode', false);
    this.clearDictEdit();
  }

  startCapture(target: string): void {
    // 入力途中の変換状態を持ち越すと、欄を切り替えた瞬間に前の欄の続きが
    // 入ってしまう。切り替えのたびに畳む。
    this.resetComposition();
    this.captureTarget = target;
    AppStorage.setOrCreate('dictCaptureTarget', target);
  }

  stopCapture(): void {
    this.resetComposition();
    this.captureTarget = '';
    AppStorage.setOrCreate('dictCaptureTarget', '');
  }

  private appendCaptured(text: string): void {
    const key = this.captureTarget === 'yomi' ? 'dictEditYomi' : 'dictEditWord';
    const cur = AppStorage.get<string>(key) ?? '';
    AppStorage.setOrCreate(key, cur + text);
  }

  private backspaceCaptured(): void {
    const key = this.captureTarget === 'yomi' ? 'dictEditYomi' : 'dictEditWord';
    const cur = AppStorage.get<string>(key) ?? '';
    if (cur.length > 0) { AppStorage.setOrCreate(key, cur.slice(0, cur.length - 1)); }
  }

  clearDictEdit(): void {
    AppStorage.setOrCreate('dictEditYomi', '');
    AppStorage.setOrCreate('dictEditWord', '');
    AppStorage.setOrCreate('dictEditPos', 'noun');
    AppStorage.setOrCreate('dictEditVerbGroup', '');
  }

  // 自由入力の内容を登録する。読みがひらがな以外だと変換で引けないので弾く。
  // 戻り値は画面に出すメッセージ (空なら成功)。
  commitDictEdit(): string {
    const yomi = AppStorage.get<string>('dictEditYomi') ?? '';
    const word = AppStorage.get<string>('dictEditWord') ?? '';
    if (yomi.length === 0 || word.length === 0) { return 'よみと単語の両方を入れてください'; }
    for (let i = 0; i < yomi.length; i++) {
      const c = yomi.charCodeAt(i);
      const hira = c >= 0x3041 && c <= 0x309f;
      const choon = c === 0x30fc;
      if (!hira && !choon) { return 'よみはひらがなで入れてください'; }
    }
    const pos = AppStorage.get<string>('dictEditPos') ?? 'noun';
    // 動詞で読みが「る」で終わる場合、五段と一段が字面で区別できない
    // (detectGodanRow のコメント参照)。活用を出し分けるため種別が要る。
    const group = AppStorage.get<string>('dictEditVerbGroup') ?? '';
    if (pos === 'verb' && yomi.endsWith('る') && group.length === 0) {
      return '動詞の種類(五段/一段)を選んでください';
    }
    this.registerUserWordWithPos(yomi, word, pos, pos === 'verb' ? group : '');
    this.clearDictEdit();
    return '';
  }

  // ---- 削除した候補の一覧・復元 -----------------------------------------
  //
  // 候補の長押し削除は拡張プロセス側で完結しているのに、一覧と復元は本体アプリの
  // 画面にしか無かった。両者はサンドボックスが別なので、アプリからは常に空にしか
  // 見えず「消したら二度と戻せない」状態だった。書いている側で見せる。

  publishSuppressedRows(): void {
    const map = SuppressedCandidates.getSuppressed();
    const rows: string[] = [];
    for (const reading of Object.keys(map)) {
      for (const surface of map[reading]) { rows.push(`${reading}\t${surface}`); }
    }
    rows.sort();
    AppStorage.setOrCreate('suppressedRows', rows);
  }

  restoreSuppressedWord(reading: string, surface: string): void {
    const next = SuppressedCandidates.restore(
      SuppressedCandidates.getSuppressed(), reading, surface);
    SuppressedCandidates.setSuppressed(next);
    this.saveSuppressed();
    this.publishSuppressedRows();
    // 内蔵辞書で作れない語は、抑制を解いただけでは戻らない。自分で登録して
    // 消した語がこれにあたる (削除時に登録からも外しているため)。
    // 作れないと分かったときだけ登録し直す。
    if (!AiConverter.canProduce(reading, surface)) {
      this.registerUserWord(reading, surface);
    }
    // 削除で候補から外れていた語をその場で戻す。
    this.updateCandidates();
  }

  clearAllSuppressed(): void {
    SuppressedCandidates.clear();
    this.saveSuppressed();
    this.publishSuppressedRows();
    this.updateCandidates();
  }

  // ---- 修復・リセット ---------------------------------------------------
  //
  // 本体アプリの「修復」は本体アプリ側のストアを消すだけで、キーボードの学習・
  // 設定・辞書には一切届かない (別サンドボックス)。壊れた状態からの復旧手段が
  // 実質機能していなかったので、こちらにも用意する。

  resetLearningData(): void {
    if (!this.imeContext) { return; }
    AppMaintenance.resetLearning(this.imeContext)
      .then(() => {
        KanaKanjiConverter.setLearned({});
        NextWordPredictor.setLearned({});
        InputHandler.symbolFreq = {};
        InputHandler.publishFrequentSymbols();
        UnknownWordLearner.setLearned({});
        this.publishDictRows();
        console.info('shuntiIME resetLearningData ok');
      })
      .catch((e: ESObject) => { console.error('shuntiIME resetLearningData failed', JSON.stringify(e)); });
  }

  resetAllData(): void {
    if (!this.imeContext) { return; }
    AppMaintenance.resetAll(this.imeContext)
      .then(() => {
        KanaKanjiConverter.setLearned({});
        NextWordPredictor.setLearned({});
        InputHandler.symbolFreq = {};
        InputHandler.publishFrequentSymbols();
        UnknownWordLearner.setLearned({});
        SuppressedCandidates.clear();
        this.userDictRaw = {};
        AiConverter.setUserDict({}, {});
        this.publishDictRows();
        this.publishSuppressedRows();
        console.info('shuntiIME resetAllData ok');
      })
      .catch((e: ESObject) => { console.error('shuntiIME resetAllData failed', JSON.stringify(e)); });
  }



  // 全文を hilog へ吐き出す。クリップボードの setData (生テキスト・URI 両方)は
  // この拡張プロセスからだと常に "async call failed" で落ちることを実測で確認
  // 済み (759,070 文字のテキストでも file:// URI でも同じ)。原因の切り分けは
  // 打ち切り、このセッション中ずっと問題なく読めている hilog 経由に変える --
  // サンドボックスの外から hdc で読める、ここまでで唯一確実な経路。
  //
  // Base64 にしてから 1 行 1500 文字ずつ区切って console.info する。生の
  // JSONL のまま流すと、たまたま長い1レコードが hilog の1行上限を超えたときに
  // そこだけ千切れる可能性がある。Base64 化しておけば区切り位置は文字数だけの
  // 問題になり、復元側 (PC 側スクリプト) で単純に連結してから戻せる。
  // 各行に SHUNTILOGDUMP という専用マーカーと連番/総数を入れているので、
  // hilog の他の行に紛れても grep で拾い、番号順に並べ直せる。


  // 登録候補をすべて消す。削除を考慮していなかった頃の壊れた対応が大量に
  // 溜まっているため、1件ずつ消さずに済む手段を用意する。
  clearUnknownWords(): void {
    UnknownWordLearner.setLearned({});
    this.saveUnknownWords();
    this.publishDictRows();
  }

  // 登録済みの1件を消す。
  //
  // 辞書から外すだけでなく「削除した候補」にも入れる。外しただけだと、同じ語が
  // 内蔵辞書にもあった場合に変換へ出続けて「消したのに出る」ことになるし、
  // 消した記録がどこにも残らないので戻す手段が無くなる。
  removeUserWord(reading: string, surface: string): void {
    if (!this.imeContext) { return; }
    const ctx = this.imeContext;
    UserDictStore.load(ctx, true)
      .then((dict: Record<string, string[]>) => {
        const next: Record<string, string[]> = {};
        for (const k of Object.keys(dict)) {
          if (k !== reading) { next[k] = dict[k]; continue; }
          const kept = dict[k].filter((w: string) => w !== surface);
          if (kept.length > 0) { next[k] = kept; }
        }
        return UserDictStore.save(ctx, next).then(() => next);
      })
      .then((next: Record<string, string[]>) => {
        this.userDictRaw = next;
        if (SuppressedCandidates.suppress(reading, surface)) {
          this.saveSuppressed();
          this.publishSuppressedRows();
        }
        this.publishDictRows();
        this.reloadUserDict();
      })
      .catch((e: ESObject) => { console.error('shuntiIME removeUserWord failed', JSON.stringify(e)); });
  }

  // 候補の長押し削除で消した語が自分の登録語だったら、登録からも外す。
  // 抑制だけして登録に残すと、「登録済み」に一生変換に出ない語が並んでしまう。
  private unregisterIfUserWord(reading: string, surface: string): void {
    const cur = this.userDictRaw[reading];
    if (cur === undefined || cur.indexOf(surface) < 0) { return; }
    if (!this.imeContext) { return; }
    const ctx = this.imeContext;
    const next: Record<string, string[]> = {};
    for (const k of Object.keys(this.userDictRaw)) {
      if (k !== reading) { next[k] = this.userDictRaw[k]; continue; }
      const kept = this.userDictRaw[k].filter((w: string) => w !== surface);
      if (kept.length > 0) { next[k] = kept; }
    }
    this.userDictRaw = next;
    UserDictStore.save(ctx, next)
      .then(() => { this.publishDictRows(); })
      .catch((e: ESObject) => { console.error('shuntiIME unregisterIfUserWord failed', JSON.stringify(e)); });
  }

  // 登録候補から外す (辞書には入れない)。
  dismissUnknownWord(reading: string, surface: string): void {
    UnknownWordLearner.forget(reading, surface);
    this.saveUnknownWords();
    this.publishDictRows();
  }

  private writeSuppressed(): void {
    if (!this.suppressedDirty) { return; }
    const prefs = this.suppressedPrefs;
    if (!prefs) { return; }
    this.suppressedDirty = false;
    prefs.put(SUPPRESSED_KEY, JSON.stringify(SuppressedCandidates.getSuppressed()))
      .then(() => prefs.flush())
      // 書けなかったら次の機会に書き直す (前は dirty を先に落として学習が消えた。2026-10-25)
      .catch((_e: Error) => { this.suppressedDirty = true; });
  }

  private saveSuppressed(): void {
    this.suppressedDirty = true;
    clearTimeout(this.suppressedSaveTimer);
    this.suppressedSaveTimer = setTimeout(() => { this.writeSuppressed(); }, 500);
  }

  // 予約されている保存を今すぐ全部書く。入力欄を離れるとき(onInputStop)と
  // 拡張が終了するとき(onDestroy)に呼ぶ。デバウンス待ちのまま消える経路を
  // 塞ぐのが目的なので、ここを外すと上のコメントの壊れ方に戻る。
  flushPendingSaves(): void {
    clearTimeout(this.flowSaveTimer);
    this.writeFlow();
    clearTimeout(this.learningSaveTimer);
    clearTimeout(this.nextWordSaveTimer);
    clearTimeout(this.unknownWordSaveTimer);
    clearTimeout(this.suppressedSaveTimer);
    this.writeLearning();
    this.writeNextWord();
    this.writeUnknownWords();
    this.writeSuppressed();
  }

  // Re-read the deletion list from disk. The main app's 削除した候補 screen is a
  // different process, so restores made there are invisible here until this
  // runs -- called on every onInputStart, the same refresh point the user
  // dictionary and settings use.
  private reloadSuppressed(): void {
    if (!this.imeContext) { return; }
    SuppressedCandidates.load(this.imeContext, true)
      .then((data: Record<string, string[]>) => {
        SuppressedCandidates.setSuppressed(data);
      })
      .catch((_e: Error) => {});
  }

  // The reading the visible candidates were produced from: the composing text
  // clipped to the conversion range (see splitAtConversionEnd). Shared by
  // updateCandidates and suppressCandidate so a deletion is always keyed on
  // exactly the reading the deleted candidate was offered for.
  // 変換範囲を ◄ で縮めているか。範囲が 0 (全体) のときも、読みの長さちょうどのとき (かなを直接打つと
  // handleDirectKana がそうする) も「縮めていない」。以前は 0 だけを見ていたので、フリックで打つと
  // AI 変換の自動確定が一度も走らなかった。
  private isRangeShrunk(composing: string): boolean {
    return this.conversionEnd > 0 && this.conversionEnd < composing.length;
  }

  private conversionTarget(): string {
    const composing = AppStorage.get<string>('composingText') ?? '';
    const rangeEnd = this.conversionEnd > 0 ? this.conversionEnd : composing.length;
    return composing.slice(0, rangeEnd);
  }

  // Long-press → 削除 on a candidate. Stops that surface being offered for this
  // reading again, and erases the learned records behind it so neither the
  // conversion-learning boost nor unknown-word learning can bring it straight
  // back. Bundled dictionaries are never modified -- see SuppressedCandidates.
  // Returns the deleted surface (for the confirmation toast), or '' if the
  // deletion was refused (e.g. the as-typed kana, which must stay committable).
  suppressCandidate(index: number): string {
    const candidates = AppStorage.get<string[]>('candidates') ?? [];
    if (index < 0 || index >= candidates.length) { return ''; }
    const surface = candidates[index];
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    const bunsetsu = state === InputState.BUNSETSU;
    const reading = bunsetsu
      ? (this.bCur < this.bSegs.length ? this.bSegs[this.bCur] : '')
      : this.conversionTarget();
    if (reading.length === 0 || surface.length === 0) { return ''; }
    if (!SuppressedCandidates.suppress(reading, surface)) { return ''; }
    InputLog.recordDeleteCandidate(reading, surface, index, candidates.length,
      this.candidatePickVia(), '');
    KanaKanjiConverter.forgetChoice(reading, surface);
    UnknownWordLearner.forget(reading, surface);
    // 自分で登録した語を消した場合は登録からも外す (両方の一覧の辻褄を合わせる)。
    this.unregisterIfUserWord(reading, surface);
    this.saveSuppressed();
    this.saveLearning();
    this.saveUnknownWords();
    // Rebuild what's on screen so the chip disappears immediately.
    if (bunsetsu) {
      // 文節の今の表記は候補の一覧とは別に持っているので、それを消したら残った候補の先頭に替える
      // (替えないと、消した表記が文節の表示に残って確定される)
      if (this.bChosen[this.bCur] === surface) {
        const left = SuppressedCandidates.filter(reading, this.bunsetsuCandidates(this.bCur));
        this.bChosen[this.bCur] = left.length > 0 ? left[0] : reading;
      }
      this.updateBunsetsuStorage();
    } else {
      AppStorage.setOrCreate('selectedCandidateIndex', 0);
      this.updateCandidates();
    }
    return surface;
  }

  // "Can the converter already produce this surface for this reading?" -- the
  // test that makes a hand-assembled word count as UNKNOWN (see
  // UnknownWordLearner). Checks the live candidate list rather than the raw
  // dictionaries so a word reachable through any current path (dict.json,
  // mozc, the user dictionary, an earlier unknown-word learn) is never
  // re-learned as if it were missing.
  private isKnownConversion(reading: string, surface: string): boolean {
    if (UnknownWordLearner.candidates(reading).indexOf(surface) >= 0) { return true; }
    // AI 変換は読み全体を語に分けて変換するので、2 回に分けて確定した複合語 (会社 -> 見学) も
    // 読み全体の変換で出せれば「作れない語」ではない
    return AiConverter.canProduce(reading, surface);
  }

  // Feed one committed conversion to the unknown-word learner, then persist if
  // it changed anything. Called from the ordinary commit paths only -- bunsetsu
  // editing and raw-kana commits close the run instead (see endUnknownWordRun).
  private observeUnknownWord(reading: string, surface: string): void {
    // タップされた表記が、確定直前に候補一覧で「準候補」として見えていたか。
    // observe() は既存の run に積むだけで判定を変えてしまうので、その前に
    // AppStorage の表示済みスナップショットを見る。単体タップは run 経由では
    // 絶対にカウントされない (flush の assembled 判定を満たさない) ので、
    // confirmQuasi で直接1つ進める -- これが「押されたら昇進」の実体。
    const quasi = AppStorage.get<string[]>('quasiCandidateSurfaces') ?? [];
    if (quasi.includes(surface)) {
      UnknownWordLearner.confirmQuasi(reading, surface);
      InputLog.recordQuasiPick(reading, surface);
    }
    UnknownWordLearner.observe(reading, surface,
      (r: string, s: string): boolean => this.isKnownConversion(r, s));
    this.saveUnknownWords();
  }

  private endUnknownWordRun(): void {
    UnknownWordLearner.endRun((r: string, s: string): boolean => this.isKnownConversion(r, s));
    this.saveUnknownWords();
  }

  // The individual words a commit consists of, newest-relevant last: every 文節
  // surface when a multi-segment sentence was committed (recovered from the same
  // alignment learnCommit uses), otherwise the whole text as one word. Splitting
  // a committed sentence into its words is what lets next-word learning pick up
  // the sentence-internal flow (映画→を→見る), not just the final word.
  private committedWords(text: string): string[] {
    const a = this.lastAlignment;
    if (a !== undefined && text.length > a.prefixText.length && text.startsWith(a.prefixText)) {
      const words = a.prefixSurfaces.slice();
      words.push(text.slice(a.prefixText.length));
      return words.filter((w: string) => w.length > 0);
    }
    return text.length > 0 ? [text] : [];
  }

  // Learn this commit's word transitions with a rolling 2-word context, then
  // publish the next-word strip. `newWords` are the words just committed (a
  // whole sentence contributes each of its 文節). Called on every commit
  // (conversion or tapped prediction); the strip stays visible in IDLE until a
  // new composition starts (which clears it -- see updateCandidates).
  private pushPredictions(newWords: string[]): void {
    let w1 = this.recentWords.length >= 2 ? this.recentWords[this.recentWords.length - 2] : '';
    let w2 = this.recentWords.length >= 1 ? this.recentWords[this.recentWords.length - 1] : '';
    for (const w of newWords) {
      if (w.length === 0) { continue; }
      // A word with nothing before it, or one right after a sentence-ending
      // word, begins a sentence -- learn it as a habitual opener rather than
      // recording a cross-sentence bigram (which would just be junk context).
      if (w2.length === 0 || NextWordPredictor.isSentenceEnd(w2)) {
        NextWordPredictor.recordOpener(w);
      } else {
        NextWordPredictor.record(w1, w2, w); // (prev2, prev1) -> w
      }
      w1 = w2;
      w2 = w;
    }
    this.saveNextWord();
    this.recentWords = [];
    if (w1.length > 0) { this.recentWords.push(w1); }
    if (w2.length > 0) { this.recentWords.push(w2); }
    // Strip, in priority order: a personal habitual phrase (口癖/定型文, e.g. a
    // recurring sign-off 了解しました！ or a favorite filler like マジで) as ONE
    // multi-word chip when the user has repeated it enough to be a clear habit
    // (see NextWordPredictor.phraseCompletion) -- one tap instead of one per
    // word; then emoji for the last word; then ordinary single-word next-word
    // predictions. predictionWordGroups is kept parallel to the displayed strip
    // so commitPrediction can replay a multi-word chip's OWN words through
    // pushPredictions (correct learning bookkeeping) rather than treating the
    // whole concatenated phrase as a single "word".
    const strip: string[] = [];
    const wordGroups: string[][] = [];
    const pushChip = (text: string, words: string[]): void => {
      if (text.length > 0 && strip.indexOf(text) < 0 && strip.length < 16) {   // 絵文字・顔文字を多めに出すので 16 まで (横に送れる)
        strip.push(text);
        wordGroups.push(words);
      }
    };
    // 文が終わった直後は何も出さない。以前は書き出しの口癖 (openers) を出していたが、
    // 変な文を打つとそれが出てきてしまい、使われることもほぼなかった (1.6.1 で廃止)。
    if (!NextWordPredictor.isSentenceEnd(w2) && w2.length > 0) {
      const phrase = NextWordPredictor.phraseCompletion(w2, w1);
      if (phrase.length >= 2) { pushChip(phrase.join(''), phrase); }
      for (const e of EmojiSuggest.forSurface(w2)) { pushChip(e, [e]); }
      // 文の中の語に当たる絵文字・顔文字 (辞書の。ユーザー 2026-10-14)
      for (const e of AiConverter.takeAfterEmoji()) { pushChip(e, [e]); }
      for (const w of NextWordPredictor.predict(w2, w1)) { pushChip(w, [w]); }
    }
    AppStorage.setOrCreate('predictions', strip);
    this.predictionWordGroups = wordGroups;
    InputLog.recordPredictions(strip, [w1, w2]);
  }

  // In casual JP chat "。" is rarely typed; sentences/clauses are broken with a
  // 全角スペース, a newline, or by sending (Enter). Treat those the same way a
  // sentence-ending mark is treated in pushPredictions: drop the running word
  // context so the NEXT commit is learned as a fresh opener (口癖 の書き出し)
  // rather than chained across the break. 何も打っていない状態なので、予測の帯は空にする
  // (書き出しの候補は出さない。pushPredictions の説明)。
  private markSentenceBoundary(): void {
    InputLog.recordBoundary('sentence');
    SentenceFlow.boundary();
    this.recentWords = [];
    // A finished sentence also finishes whatever word was being assembled.
    this.endUnknownWordRun();
    AppStorage.setOrCreate('predictions', [] as string[]);
    this.predictionWordGroups = [];
  }

  // Reorder conversion candidates so any the user has actually written after the
  // current recent-word context lead. General homophone disambiguation from real
  // usage (the learned next-word bigram), the scalable counterpart to the
  // hand-curated CONTEXT_HINTS table: it needs no per-word entries and covers
  // whatever vocabulary this user writes. Stable: matched candidates keep their
  // learned order, everything else keeps its original order.
  private applyContextRerank(candidates: string[]): string[] {
    if (candidates.length < 2 || this.recentWords.length === 0) { return candidates; }
    const prev1 = this.recentWords[this.recentWords.length - 1];
    const prev2 = this.recentWords.length >= 2 ? this.recentWords[this.recentWords.length - 2] : undefined;
    const learned = NextWordPredictor.learnedNext(prev1, prev2);
    if (learned.length === 0) { return candidates; }
    const boosted: string[] = [];
    for (const s of learned) {
      if (candidates.indexOf(s) >= 0 && boosted.indexOf(s) < 0) { boosted.push(s); }
    }
    if (boosted.length === 0) { return candidates; }
    const rest = candidates.filter((c: string) => boosted.indexOf(c) < 0);
    return [...boosted, ...rest];
  }

  // Commit a tapped next-word prediction (a plain word, an emoji, or a
  // multi-word habitual-phrase chip). Nothing is composing, so this inserts
  // the chip's text directly, then replays its OWN word(s) through
  // pushPredictions -- for a phrase chip that both correctly learns each
  // internal transition (reinforcing the habit further) and re-predicts from
  // its true last word, so predictions keep chaining naturally.
  commitPrediction(index: number): void {
    const preds = AppStorage.get<string[]>('predictions') ?? [];
    if (index < 0 || index >= preds.length) { return; }
    const text = preds[index];
    if (text.length === 0) { return; }
    InputLog.recordPredictionPick(text, index, preds);
    this.insertText(text, { raw: true } as InsertTextOptions);
    KanaKanjiConverter.addContext(text);
    const words = index < this.predictionWordGroups.length && this.predictionWordGroups[index].length > 0
      ? this.predictionWordGroups[index]
      : [text];
    this.pushPredictions(words);
  }

  onInputStart(
    kbCtrl: inputMethodEngine.KeyboardController,
    client: inputMethodEngine.InputClient
  ): void {
    this.kbController = kbCtrl;
    this.inputClient = client;
    SentenceFlow.reset();   // 別の欄に移った: 途中の節はつながらない
    // 入力中のかなを、キーボード内ではなくホストアプリの入力欄に直接
    // 下線付きで出せるか (setPreviewTextSync)。対応は入力欄ごとに違い、
    // 非対応の欄で呼ぶと 12800011 で失敗するので、必ずここで確認してから使う。
    // 対応していればパネル内の composing 表示を畳んで、その分キーボードを
    // 低くできる (publishPreviewMode)。
    this.previewSupported = false;
    this.previewSupportPending = true;
    this.publishPreviewMode();
    // 入力ログは、この欄が secure (パスワード等) か分かるまで保留にする。
    InputLog.beginField();
    client.getEditorAttribute()
      .then((attr: inputMethodEngine.EditorAttribute) => {
        this.previewSupported = attr.isTextPreviewSupported === true;
        this.previewSupportPending = false;
        this.publishPreviewMode();
        InputLog.setFieldPattern(attr.inputPattern);
      })
      .catch((_e: Error) => {
        this.previewSupported = false;
        this.previewSupportPending = false;
        this.publishPreviewMode();
        InputLog.setFieldPattern(0);
      });
    this.converter.reset();
    this.reloadUserDict();
    this.reloadSettings();
    this.reloadSuppressed();
    if (this.imeContext) { ClipboardPins.load(this.imeContext).catch((_e: Error) => {}); }
    AppStorage.setOrCreate('composingText', '');
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('candidates', [] as string[]);
    AppStorage.setOrCreate('selectedCandidateIndex', 0);
    AppStorage.setOrCreate('inputState', InputState.IDLE);
    AppStorage.setOrCreate('keyboardSubMode', 'kana');
    this.insertedBuffer = [];
    this.composingUndoStack = [];
    // 新しい入力欄 = 別のドキュメント/別のアプリの可能性があるので、hostCursor
    // が指す座標系ごと無効になる。持ち越すと別の場所を編集しかねない。
    this.hostUndoStack = [];
    this.hostCursor = 0;
    this.syncHostCursor();
    this.setPendingBracketClose('');
    this.conversionEnd = 0;
    AppStorage.setOrCreate('convEnd', 0);
    if (!AppStorage.has('inputMode')) {
      AppStorage.setOrCreate('inputMode', InputMode.HIRAGANA);
    }
    this.refreshClipboardPreview();
  }

  onInputStop(): void {
    // 溜めた記録を書き出してから、次の欄の属性が分かるまで保留に入る。
    InputLog.beginField();
    // 欄を離れる前にプレビューを畳む (inputClient を捨てた後では解除できない)。
    this.cancelPreviewText();
    this.previewSupported = false;
    this.publishPreviewMode();
    this.kbController = undefined;
    this.inputClient = undefined;
    this.converter.reset();
    this.insertedBuffer = [];
    this.composingUndoStack = [];
    this.hostUndoStack = [];
    this.hostCursor = 0;
    this.setPendingBracketClose('');
    this.conversionEnd = 0;
    this.composingWatermark = '';
    AppStorage.setOrCreate('composingText', '');
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('candidates', [] as string[]);
    AppStorage.setOrCreate('predictions', [] as string[]);
    this.recentWords = [];
    this.predictionWordGroups = [];
    // Leaving the field: drop the recent-context buffer so homophone ranking in
    // the next app/text box doesn't inherit this one's context.
    KanaKanjiConverter.clearContext();
    this.endUnknownWordRun();
    // 入力欄を離れた = この拡張プロセスがこの先落とされうる。デバウンス待ちの
    // 保存をここで確定させないと、タイマーごと消えて一度も書かれない
    // (flushPendingSaves のコメント参照)。
    this.flushPendingSaves();
    AppStorage.setOrCreate('inputState', InputState.IDLE);
    AppStorage.setOrCreate('convEnd', 0);
  }

  private alphaComposeOn(): boolean {
    return AppStorage.get<boolean>('alphaCompose') !== false;
  }

  handleKeyPress(key: string): void {
    const mode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;

    if (mode === InputMode.ALPHANUMERIC) {
      InputLog.recordKey(key, '', '', mode);
      // 英数字も入力中の文字として組み立てて変換する (2000 -> 2,000・二千、ぱちんこ + CR -> パチンコCR、iphone -> iPhone)。
      // 英文は空白・Enter でそのまま確定して続けて打てる (handleSpaceKey)。2026-10-09 ユーザー: 英数字キーボードから変換を起こせるように。
      // 設定「英字も変換する」がオフなら前のまま: 入力中の文字があるときと数字だけ組み立て、ほかはそのまま入れる
      if (this.alphaComposeOn() || (AppStorage.get<string>('composingText') ?? '').length > 0 || /^[0-9]$/.test(key)) {
        this.appendRaw(key);
        return;
      }
      this.commitPending();
      this.insertText(key);
      return;
    }

    if (state === InputState.SELECTING) {
      // New key → commit top candidate first, then start new composition
      this.commitTopCandidate();
    }

    const result = this.converter.processKey(key);
    InputLog.recordKey(key, result.committed, result.pending, mode);

    if (result.committed.length > 0) {
      const kana = mode === InputMode.KATAKANA
        ? toKatakana(result.committed)
        : result.committed;
      const current = AppStorage.get<string>('composingText') ?? '';
      AppStorage.setOrCreate('composingText', current + kana);
      this.updateCandidates();
    }

    AppStorage.setOrCreate('pendingRomaji', result.pending);

    const composing = AppStorage.get<string>('composingText') ?? '';
    const pending = result.pending;
    if (composing.length > 0 || pending.length > 0) {
      AppStorage.setOrCreate('inputState', InputState.COMPOSING);
    } else {
      AppStorage.setOrCreate('inputState', InputState.IDLE);
    }
  }

  handleSpaceKey(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    let composing = AppStorage.get<string>('composingText') ?? '';
    if (state !== InputState.SELECTING) {
      InputLog.recordSpace(state, composing, -1, 0);
    }

    // 英数字で打った英文 (入力中の文字が英数字・記号だけ) は、空白でそのまま確定して空白を入れる (変換は候補から選んだときだけ)
    if (state === InputState.COMPOSING && (AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA) === InputMode.ALPHANUMERIC
        && /^[\x21-\x7e]+$/.test(composing) && (AppStorage.get<string>('pendingRomaji') ?? '') === '') {
      this.commitRaw();
      this.insertText(' ');
      return;
    }
    if (state === InputState.IDLE || composing.length === 0) {
      const mode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
      if (mode === InputMode.ALPHANUMERIC) {
        this.insertText(' ');
      } else {
        // A 全角スペース in JP mode is a chat clause break -> sentence boundary.
        this.insertText('　');
        this.markSentenceBoundary();
      }
      return;
    }

    if (state === InputState.COMPOSING) {
      // Flush any pending romaji first
      const pending = this.converter.flushPending();
      const mode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
      if (pending.length > 0) {
        const kana = mode === InputMode.KATAKANA ? toKatakana(pending) : pending;
        composing = composing + kana;
        AppStorage.setOrCreate('composingText', composing);
        AppStorage.setOrCreate('pendingRomaji', '');
      }
      // 候補は打っている間と同じ一覧 (AI 変換)。以前はここだけ旧エンジンで引き直していた
      this.updateCandidates();
      AppStorage.setOrCreate('selectedCandidateIndex', 0);
      AppStorage.setOrCreate('inputState', InputState.SELECTING);
      return;
    }

    if (state === InputState.SELECTING) {
      const candidates = AppStorage.get<string[]>('candidates') ?? [];
      let idx = (AppStorage.get<number>('selectedCandidateIndex') ?? 0) + 1;
      if (idx >= candidates.length) { idx = 0; }
      AppStorage.setOrCreate('selectedCandidateIndex', idx);
      InputLog.recordSpace(state, composing, idx, candidates.length);
    }
  }

  handleBackspace(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;

    if (state === InputState.IDLE) {
      // 自由入力中はパネル内の欄から1文字消す (insertText と対になる出口)。
      if (this.captureTarget.length > 0) {
        this.backspaceCaptured();
        return;
      }
      // 消す文字は「直前に自分が打ち込んだ文字」ではなく「今カーソルの
      // 直前に実際にある文字」を読む。矢印キーでカーソルを動かした後の
      // backspace は別の文字を消すので、insertedBuffer.pop() だけに
      // 頼ると取消キーへ誤った文字を記録してしまう (今回の修正対象)。
      // getForwardSync が使えない/失敗する欄向けに従来の推測へ落とす。
      let deleted = '';
      try {
        // getForward = カーソルの前、getBackward = カーソルの後ろ (名前と逆。deleteForward が前を消すのと同じ)。
        // getBackwardSync にしていて、文の途中で消すとカーソルの後ろの字を覚え、取消キーで「いう」の代わりに「ええ」が入った (2026-10-14)
        deleted = this.inputClient?.getForwardSync(1) ?? '';
      } catch (_e) {
        deleted = '';
      }
      SentenceFlow.backspace();
      if (deleted.length === 0) { deleted = this.insertedBuffer.pop() ?? ''; }
      else { this.insertedBuffer.pop(); }
      if (deleted.length > 0) { this.pushHostOp('delete', deleted); }
      InputLog.recordDeleteChar(deleted, InputState.IDLE);
      // 自動モードの保留は「開き括弧〜次の確定」の間しか生きないので、
      // 何を消したかに関わらず破棄してよい (従来どおり。賢く追おうとは
      // しない)。手動モードの保留は確定をまたいで生き続けるため、同じ
      // 扱いにすると括弧の中身を1文字打ち間違えて消しただけで閉じ括弧まで
      // 消えてしまう。そちらは「開き括弧そのものを消した = 括弧をやめた」
      // ときだけ破棄する。
      if (BRACKET_PAIRS[deleted] === this.pendingBracketClose) {
        this.setPendingBracketClose('');
      }
      this.inputClient?.deleteForward(1);
      // メールのドメインを出していたら、消した後の形で出し直す (@ を消したら帯を空に。ユーザー 2026-10-13)
      if (this.emailStripShown.length > 0 && (AppStorage.get<string[]>('predictions') ?? []).join('') === this.emailStripShown) {
        if (!this.showEmailDomains()) {
          this.emailStripShown = '';
          AppStorage.setOrCreate('predictions', [] as string[]);
          this.predictionWordGroups = [];
        }
      }
      // 直前の確定 (熟語など) をバックスペースで削っていく間、漢字の手がかりが
      // 残っている限り truncate() で末尾を追う (かける→翔ける→⌫⌫→翔 のような、
      // 熟語や活用語尾の一部だけを残すケースを検出するため)。全部ひらがな
      // (生のかな確定・助詞など、対応が保証できない) まで削り込んだら
      // truncate() 自身が false を返すので、そこから先は以前と同じ
      // abandonRun() で run ごと捨てる (「〜して」のバグの再発防止 --
      // UnknownWordLearner.truncate のコメント参照)。
      if (!UnknownWordLearner.truncate()) {
        UnknownWordLearner.abandonRun();
      }
      // The committed word the next-word strip was keyed on is being edited
      // away -- retire the strip so a now-stale prediction can't be tapped.
      if (this.recentWords.length > 0) {
        this.recentWords = [];
        this.predictionWordGroups = [];
        AppStorage.setOrCreate('predictions', [] as string[]);
      }
      return;
    }

    if (state === InputState.BUNSETSU) {
      // Exit bunsetsu, restore composing text
      const composing = this.bSegs.join('');
      InputLog.recordBunsetsuCancel(composing);
      this.bSegs = [];
      this.bChosen = [];
      this.bCur = 0;
      AppStorage.setOrCreate('bunsetsuTexts', [] as string[]);
      AppStorage.setOrCreate('bunsetsuRaw', [] as string[]);
      AppStorage.setOrCreate('bunsetsuCurrent', 0);
      AppStorage.setOrCreate('composingText', composing);
      AppStorage.setOrCreate('inputState', InputState.COMPOSING);
      this.updateCandidates();
      return;
    }

    if (state === InputState.SELECTING) {
      InputLog.recordDeleteChar('', InputState.SELECTING);
      AppStorage.setOrCreate('candidates', [] as string[]);
      AppStorage.setOrCreate('inputState', InputState.COMPOSING);
      return;
    }

    if (state === InputState.COMPOSING) {
      const pending = AppStorage.get<string>('pendingRomaji') ?? '';
      if (pending.length > 0) {
        this.converter.backspace();
        AppStorage.setOrCreate('pendingRomaji', pending.slice(0, -1));
        InputLog.recordDeleteChar(pending.slice(-1), 'COMPOSING_ROMAJI');
      } else {
        const composing = AppStorage.get<string>('composingText') ?? '';
        if (composing.length > 0) {
          const ch = composing.slice(-1);
          this.composingUndoStack.push(ch);
          AppStorage.setOrCreate('composingText', composing.slice(0, -1));
          InputLog.recordDeleteChar(ch, InputState.COMPOSING);
        }
      }

      const composing = AppStorage.get<string>('composingText') ?? '';
      const newPending = AppStorage.get<string>('pendingRomaji') ?? '';
      if (composing.length === 0 && newPending.length === 0) {
        AppStorage.setOrCreate('inputState', InputState.IDLE);
      }
      this.updateCandidates();
    }
  }

  handleEnterKey(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    InputLog.recordEnter(state, AppStorage.get<string>('composingText') ?? '');
    if (state === InputState.BUNSETSU) {
      this.commitBunsetsu();
    } else if (state === InputState.SELECTING) {
      this.commitTopCandidate();
    } else if (state === InputState.COMPOSING) {
      this.commitRaw();
    } else {
      // 送信/改行してしまうと、保留中の閉じ括弧が出る機会は永遠に失われる
      // (手動モードでは ► を押すまで待ち続けるので、なおさら取り残されや
      // すい)。開きっぱなしの文が飛んでいくのを防ぐため、送信の直前に
      // 閉じておく。
      this.flushPendingBracketClose();
      this.inputClient?.getEditorAttribute().then((attr: inputMethodEngine.EditorAttribute) => {
        this.sendEnter(attr.enterKeyType);
      }).catch((_e: Error) => {
        this.sendEnter(0);
      });
      // Enter in an idle field sends the message (or inserts a newline): either
      // way the current sentence is finished, so reset context and clear the strip.
      this.markSentenceBoundary();
    }
  }

  // Enter の長押し: アプリが Enter を「送信」などに指定していても、必ず改行を入れる
  // (複数行の欄なのに送信に指定して、受け取っても何もしないアプリへの逃げ道)。
  // 入力中の文字があれば、先にふつうの Enter と同じく確定してから改行する。
  handleEnterLongPress(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state === InputState.BUNSETSU) {
      this.commitBunsetsu();
    } else if (state === InputState.SELECTING) {
      this.commitTopCandidate();
    } else if (state === InputState.COMPOSING) {
      this.commitRaw();
    }
    this.inputClient?.insertText('\n');
    this.markSentenceBoundary();
  }

  // 何も入力していないときの Enter をホストへ渡す。
  // sendKeyFunction は「送信」「検索」などのアクションをアプリに伝えるだけで、
  // 改行を入れるかどうかはアプリ次第。改行のアクション (NEWLINE) や、複数行の欄で
  // アクション未指定 (UNSPECIFIED / NONE) を受け取っても何もしないアプリがあり、
  // そこでは改行できなかった。アプリが明示的なアクション (送信・検索・次へ・完了など)
  // を指定していない欄では、アプリに頼らず改行文字を直接入れる (他の IME と同じく、
  // Enter は改行として使う場面のほうが多い)。ArkUI の 1 行の欄 (TextInput) は既定が
  // DONE なので、ここで改行になるのは複数行の欄か、ウェブ・他の UI 部品の欄。
  //   enterKeyType: 0 UNSPECIFIED, 1 NONE, 2 GO, 3 SEARCH, 4 SEND, 5 NEXT, 6 DONE, 7 PREVIOUS, 8 NEWLINE
  private sendEnter(enterKeyType: number): void {
    if (enterKeyType === 8 || enterKeyType === 0 || enterKeyType === 1) {
      this.inputClient?.insertText('\n');
    } else {
      this.inputClient?.sendKeyFunction(enterKeyType);
    }
  }

  commitRaw(): void {
    const composing = AppStorage.get<string>('composingText') ?? '';
    const pending = this.converter.flushPending();
    const all = composing + pending;
    InputLog.recordRaw(composing, all);
    if (all.length > 0) { this.insertText(all); }
    // Committing as raw kana is not a conversion, so it closes any word the
    // user was assembling rather than extending it.
    this.endUnknownWordRun();
    this.resetComposition();
  }

  // When the user has shrunk the conversion range (cursor-left: conversionEnd
  // set to a point before the end of the composing text), only that prefix was
  // converted into `candidates`; everything after it stays as typed kana.
  // Returns [convertedReading, kanaTail] so a commit learns the correct
  // reading→surface (the prefix only) and knows what is left to keep composing
  // (see continueWithTail). With no range adjustment the tail is empty and the
  // reading is the whole composing text -- identical to before.
  private splitAtConversionEnd(composing: string): [string, string] {
    const end = (this.conversionEnd > 0 && this.conversionEnd < composing.length)
      ? this.conversionEnd : composing.length;
    return [composing.slice(0, end), composing.slice(end)];
  }

  // 提案 (ひらがなの帯の「提案: …」) をタップした: 変換する所をその表記で確定する。学習はしない (打ったとおりの変換ではないので)
  commitSuggestion(index: number = 0): void {
    const list = AppStorage.get<string[]>('suggestList') ?? [];
    let text = index < list.length ? list[index] : '';
    if (text.length === 0) { return; }
    const composing = AppStorage.get<string>('composingText') ?? '';
    const parts = this.splitAtConversionEnd(composing);
    const candidates = AppStorage.get<string[]>('candidates') ?? [];
    // 気分の絵文字・顔文字 (6): 絵文字だけを、いま見えている 1 位の後ろに足す (提案を作った時の 1 位とずれることがある)
    const kinds = AppStorage.get<number[]>('suggestKinds') ?? [];
    const from = AppStorage.get<string>('suggestFrom') ?? '';
    if (index < kinds.length && kinds[index] === 6 && text.startsWith(from) && candidates.length > 0) {
      text = candidates[0] + text.slice(from.length);
    }
    InputLog.recordCommit(parts[0], text, -1, candidates.length, candidates, 'suggest', [] as string[]);
    AiConverter.recordSuggestUse(list[index]);
    this.insertText(text);
    KanaKanjiConverter.addContext(text);
    AppStorage.setOrCreate('suggestList', [] as string[]);
    if (parts[1].length > 0) {
      this.continueWithTail(parts[1]);
      return;
    }
    this.resetComposition();
  }

  commitCandidate(index: number): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state === InputState.BUNSETSU) {
      this.commitBunsetsuCandidate(index);
      return;
    }
    const composing = AppStorage.get<string>('composingText') ?? '';
    const candidates = AppStorage.get<string[]>('candidates') ?? [];
    const text = index < candidates.length ? candidates[index] : '';
    if (text.length > 0) {
      const parts = this.splitAtConversionEnd(composing);
      const reading = parts[0];
      const tail = parts[1];
      InputLog.recordCommit(reading, text, index, candidates.length, candidates,
        this.candidatePickVia(), [] as string[]);
      // A predictive completion (会社 committed from the prefix かいし) must NOT
      // feed conversion learning: its reading is longer than what was typed, so
      // recordChoice(かいし→会社) would teach a wrong reading. Skip the learn;
      // completion personalisation instead flows from full-word commits (typing
      // かいしゃ→会社 correctly teaches かいしゃ, which then completes かいし).
      if (!this.isNonLearningCandidate(reading, text)) {
        this.learnCommit(reading, text);
        this.saveLearning();
        this.observeUnknownWord(reading, text);
      }
      this.insertText(text);
      KanaKanjiConverter.addContext(text);
      const words = this.committedWords(text);
      // 残りがあるなら、それはまだ変換していない入力。確定させずに composing に
      // 戻して続けて変換できるようにする (continueWithTail のコメント参照)。
      if (tail.length > 0) {
        this.pushPredictions(words);
        this.continueWithTail(tail);
        return;
      }
      this.resetComposition();
      this.pushPredictions(words);
      return;
    }
    this.resetComposition();
  }

  // True when `text` is an ephemeral suggestion offered for the composing
  // prefix -- a predictive completion (reading longer than typed) or a live
  // date/time value -- and so must bypass reading→surface conversion learning
  // (かいし→会社 or きょう→"2026年7月23日" would both be wrong to learn).
  private isNonLearningCandidate(composing: string, text: string): boolean {
    if (PredictiveConversion.predict(composing, KanaKanjiConverter.getLearned(),
        KanaKanjiConverter.prefixCompletions(composing, PREDICT_DICT_HITS),
        ShortTermMemory.hitsForPrefix(composing)).indexOf(text) >= 0) {
      return true;
    }
    if (DateTimePredictor.predict(composing, new Date()).indexOf(text) >= 0) {
      return true;
    }
    return NumberFormatter.predict(composing).indexOf(text) >= 0;
  }

  // Learn from a committed candidate. For a multi-segment sentence whose
  // committed surface still begins with the auto-converted prefix captured in
  // updateCandidates, peel that prefix off to recover the accepted final-
  // segment surface, then learn every 文節 individually -- so correcting one
  // homophone (しゃしんをとった→写真を撮った) teaches とった→撮った for the
  // next sentence too, the way every mature IME's learning generalizes.
  // Anything else (single segment, symbol prefix, a predictive/alt-segment
  // pick whose surface no longer starts with the prefix) falls back to the
  // plain whole reading→surface record.
  // Longest whole reading still recorded verbatim by learnCommit. Long enough
  // for the names, set phrases and stock sentences people actually retype;
  // past that a reading is a one-off sentence that would only burn a slot in
  // the learning store (and turn into noisy predictive completions, since
  // PredictiveConversion completes straight out of that store).
  private static readonly MAX_WHOLE_LEARN_LEN = 12;

  private learnCommit(composing: string, text: string): void {
    const a = this.lastAlignment;
    if (a !== undefined && text.length > a.prefixText.length && text.startsWith(a.prefixText)) {
      const lastSurface = text.slice(a.prefixText.length);
      const surfaces = a.prefixSurfaces.slice();
      surfaces.push(lastSurface);
      KanaKanjiConverter.recordSegmentedChoice(a.segs, surfaces);
      // ...and the whole reading too. Per-segment records are what make a
      // correction generalize (とった→撮った carries to the next sentence), but
      // they are keyed on the SEGMENTS while the candidate list for a
      // multi-segment reading is reordered by applyLearning under the WHOLE
      // reading -- so picking 写真を取った for しゃしんをとった recorded
      // しゃしん/とった and left しゃしんをとった itself unlearned. Re-typing the
      // exact same phrase therefore never promoted the choice, no matter how
      // many times it was made: the one case a user most expects to stick.
      if (composing.length <= InputHandler.MAX_WHOLE_LEARN_LEN) {
        KanaKanjiConverter.recordChoice(composing, text);
        ShortTermMemory.record(composing, text);
      }
    } else {
      KanaKanjiConverter.recordChoice(composing, text);
      ShortTermMemory.record(composing, text);
    }
  }

  commitTopCandidate(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state === InputState.BUNSETSU) {
      this.commitBunsetsu();
      return;
    }
    const composing = AppStorage.get<string>('composingText') ?? '';
    const candidates = AppStorage.get<string[]>('candidates') ?? [];
    const idx = AppStorage.get<number>('selectedCandidateIndex') ?? 0;
    if (candidates.length > 0) {
      const chosen = candidates[idx];
      const parts = this.splitAtConversionEnd(composing);
      const reading = parts[0];
      const tail = parts[1];
      InputLog.recordCommit(reading, chosen, idx, candidates.length, candidates, 'key',
        [] as string[]);
      if (!this.isNonLearningCandidate(reading, chosen)) {
        this.learnCommit(reading, chosen);
        this.saveLearning();
        this.observeUnknownWord(reading, chosen);
      }
      this.insertText(chosen);
      KanaKanjiConverter.addContext(chosen);
      const words = this.committedWords(chosen);
      if (tail.length > 0) {
        this.pushPredictions(words);
        this.continueWithTail(tail);
        return;
      }
      this.resetComposition();
      this.pushPredictions(words);
      return;
    }
    this.commitPending();
  }

  private commitPending(): void {
    const composing = AppStorage.get<string>('composingText') ?? '';
    const pending = this.converter.flushPending();
    const all = composing + pending;
    if (all.length > 0) { this.insertText(all); }
    this.resetComposition();
  }

  // 変換範囲を縮めて確定したときの「残り」を、続けて変換できる状態にする。
  //
  // 「かくていされる」を「かくてい」までで変換して確定したとき、残りの「される」
  // はまだ変換していない入力であって、確定した文字列ではない。それを
  // insertText(text + tail) で一緒に送り出していたので、
  //   ・画面の変換保留欄には「される」が残って見える
  //   ・しかし実際にはかなのまま確定済みで、もう変換できない
  // という食い違いが起きていた。文節ごとに変換していく操作そのものが
  // 成立していない状態で、範囲を縮める機能の存在意義が無くなっていた。
  //
  // 確定するのは変換した部分だけにして、残りは composing に戻す。
  // resetComposition と違って composingText を空にせず、変換範囲も残りの
  // 長さに張り直してから候補を出し直す。
  private continueWithTail(tail: string): void {
    this.converter.reset();
    this.bSegs = [];
    this.bChosen = [];
    this.bCur = 0;
    this.composingUndoStack = [];
    this.conversionEnd = tail.length;
    AppStorage.setOrCreate('composingText', tail);
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('bunsetsuTexts', [] as string[]);
    AppStorage.setOrCreate('bunsetsuRaw', [] as string[]);
    AppStorage.setOrCreate('bunsetsuCurrent', 0);
    AppStorage.setOrCreate('inputState', InputState.COMPOSING);
    AppStorage.setOrCreate('convEnd', this.conversionEnd);
    AppStorage.setOrCreate('selectedCandidateIndex', 0);
    this.updateCandidates();
  }

  private resetComposition(): void {
    // 入力欄に残っているプレビューを必ず消す。insertText を経ない経路
    // (取消・欄の切り替えなど) でもここを通るので、消し忘れの受け皿になる。
    this.cancelPreviewText();
    this.converter.reset();
    this.bSegs = [];
    this.bChosen = [];
    this.bCur = 0;
    // NOTE: insertedBuffer is deliberately NOT cleared here. It is a rolling
    // log of characters actually inserted into the field (capped at 50), used
    // by the IDLE backspace as a fallback when getBackwardSync can't read the
    // field. resetComposition runs after every commit, so clearing it here
    // threw away the just-committed word's characters -- backspacing a
    // committed word then had no fallback available. It is cleared only on
    // field start/stop (onInputStart / onInputStop), which is the correct
    // scope for it.
    //
    // composingUndoStack (この回のCOMPOSING中backspace履歴) はここで空にする
    // -- 確定したらその読みの組み立て直しは終わりなので。hostUndoStack
    // (確定済みホストテキストの履歴) はここでは触らない。別の対象なので
    // commit のたびに消す理由が無い。
    this.composingUndoStack = [];
    this.conversionEnd = 0;
    this.composingWatermark = '';
    AppStorage.setOrCreate('composingText', '');
    AiConverter.clearSuggestion();
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('candidates', [] as string[]);
    AppStorage.setOrCreate('selectedCandidateIndex', 0);
    AppStorage.setOrCreate('bunsetsuTexts', [] as string[]);
    AppStorage.setOrCreate('bunsetsuRaw', [] as string[]);
    AppStorage.setOrCreate('bunsetsuCurrent', 0);
    AppStorage.setOrCreate('inputState', InputState.IDLE);
    AppStorage.setOrCreate('convEnd', 0);
    this.refreshClipboardPreview();
  }

  // Kept in-memory only (AppStorage), not persisted to disk — clipboard
  // contents can be sensitive, and this app's privacy stance is to keep data
  // on-device only for as long as actually needed. History resets when the
  // keyboard extension process restarts.
  private static readonly MAX_CLIPBOARD_HISTORY = 8;

  // Signature of the clipboard entry the user has already pasted from the
  // suggestion strip. Once pasted, the paste chip stays hidden for that exact
  // content (no more flicker on every refresh) until the system clipboard
  // actually changes to something else. Empty = nothing dismissed.
  private static dismissedClipSig: string = '';

  // Stable signature for a clipboard entry so a re-read can tell "same content
  // I already pasted" from "a new copy". Text/URI/raw-image are namespaced so
  // they can never collide.
  private static clipSig(text: string, uri: string, isImage: boolean): string {
    if (text.length > 0) { return 'T:' + text; }
    if (uri.length > 0) { return 'U:' + uri; }
    if (isImage) { return 'I:'; }
    return '';
  }

  private static setClipboardPreviewState(preview: string, isImage: boolean, isFile: boolean, uri: string): void {
    AppStorage.setOrCreate('clipboardPreview', preview);
    AppStorage.setOrCreate('clipboardIsImage', isImage);
    AppStorage.setOrCreate('clipboardIsFile', isFile);
    AppStorage.setOrCreate('clipboardUri', uri);
  }

  // Public re-read of the system clipboard, for the settings panel to call
  // when it (or its クリップボード履歴 view) opens -- refreshClipboardPreview
  // otherwise only runs on keyboard show (onInputStart) and after a commit
  // (resetComposition), so a copy made in the host app while the keyboard was
  // already up wasn't picked up until the keyboard was closed and reopened
  // (an AppGallery reviewer flagged exactly this staleness). Reading the
  // pasteboard on this deliberate user action is expected and privacy-safe.
  refreshClipboard(): void {
    this.refreshClipboardPreview();
  }

  // 実機の SELinux ログで原因を確定済み:
  //
  //   avc: denied { get } for service=3701
  //     scontext=u:r:input_isolate_debug_hap:s0
  //     tcontext=u:object_r:sa_pasteboard_service:s0  permissive=0
  //   PasteboardClient# proxyService is null
  //
  // 入力方式拡張の隔離ドメイン (input_isolate_debug_hap) から
  // PasteboardService への接続そのものが SELinux で恒久的に拒否されている。
  // 今日判明した振動 (sa_miscdevice_service) の拒否と同じ形の制限で、
  // コード側のバグではなく直せない。setData (exportInputLog 参照) と
  // getData (このメソッド) はどちらも同じ経路を通るので、書き込みが
  // "async call failed" で落ちるのと同じ理由で、読み取りも常に失敗して
  // 静かに空へフォールバックしている -- 「クリップボード検出」チップが
  // 最近ずっと出ないのは、機能が壊れたのではなく、この隔離ドメインからは
  // そもそも一度も届いたことがなかった可能性が高い。
  //
  // OS がこの制限を緩めれば動くようになるので、機構はそのまま残す
  // (振動の markHapticUnavailable と同じ方針)。
  private refreshClipboardPreview(): void {
    pasteboard.getSystemPasteboard().getData().then((data: pasteboard.PasteData) => {
      // 一度でも読めたなら、この端末ではクリップボード機能が使える。
      AppStorage.setOrCreate('clipboardUnavailable', false);
      if (!data || data.getRecordCount() === 0) {
        InputHandler.setClipboardPreviewState('', false, false, '');
        return;
      }
      const isImage = data.hasType(pasteboard.MIMETYPE_PIXELMAP);
      const text = data.getPrimaryText();
      if (text && text.length > 0) {
        // Already pasted this exact content once -> keep the chip hidden.
        if (InputHandler.clipSig(text, '', false) === InputHandler.dismissedClipSig) {
          InputHandler.setClipboardPreviewState('', false, false, '');
          return;
        }
        // Different content than what was last pasted -> the dismissal is spent.
        InputHandler.dismissedClipSig = '';
        InputHandler.setClipboardPreviewState(text, false, false, '');
        this.recordClipboardHistory(text);
        return;
      }
      // A file (from Files/a Gallery share sheet) shows up as a URI record.
      // A directly-copied image often carries BOTH a pixelmap record and a
      // URI record (many apps populate both), so check for a URI regardless
      // of isImage. Preview it as a filename rather than the raw URI.
      const uri = data.getPrimaryUri();
      if (uri && uri.length > 0) {
        if (InputHandler.clipSig('', uri, isImage) === InputHandler.dismissedClipSig) {
          InputHandler.setClipboardPreviewState('', false, false, '');
          return;
        }
        InputHandler.dismissedClipSig = '';
        const preview = isImage ? '画像' : InputHandler.formatFileClipPreview(uri);
        InputHandler.setClipboardPreviewState(preview, isImage, true, uri);
        return;
      }
      if (isImage) {
        // Raw pixels only, no URI to point at (e.g. some apps' plain
        // "コピー" on an image) -- pasteClipboard() still works fine here
        // since sendPasteExtendAction doesn't need the URI, just the
        // isImage/isFile flag; only the filename preview text is degraded.
        if (InputHandler.clipSig('', '', true) === InputHandler.dismissedClipSig) {
          InputHandler.setClipboardPreviewState('', false, false, '');
          return;
        }
        InputHandler.dismissedClipSig = '';
        InputHandler.setClipboardPreviewState('画像', true, false, '');
        return;
      }
      InputHandler.setClipboardPreviewState('', false, false, '');
    }).catch((err: Error) => {
      console.error('shuntiIME refreshClipboardPreview error', JSON.stringify(err));
      // 読み出しに失敗した = 権限が下りていないか、OS がこの経路を許して
      // いない端末。効かない項目を触れる状態で置いておくと混乱するので、
      // 設定側でグレーアウトできるよう印を付ける (振動の hapticUnavailable
      // と同じ扱い)。次に一度でも読めれば自動で解除される。
      AppStorage.setOrCreate('clipboardUnavailable', true);
      InputHandler.setClipboardPreviewState('', false, false, '');
    });
  }

  // "国税庁_請求書テンプレート2026.pdf" -> "国税庁_請求書.pdf": first few
  // characters of the filename plus its extension, so the chip stays short
  // without losing the cue of what kind of file it is.
  private static formatFileClipPreview(uri: string): string {
    const clean = uri.split('?')[0].split('#')[0];
    const segments = clean.split('/');
    let rawName = segments[segments.length - 1] ?? '';
    try {
      rawName = decodeURIComponent(rawName);
    } catch {
      // Malformed percent-encoding -- fall back to the raw segment.
    }
    if (rawName.length === 0) { return 'ファイル'; }
    const dotIdx = rawName.lastIndexOf('.');
    const hasExt = dotIdx > 0 && dotIdx < rawName.length - 1;
    const base = hasExt ? rawName.slice(0, dotIdx) : rawName;
    const ext = hasExt ? rawName.slice(dotIdx + 1) : '';
    const truncatedBase = base.length > 8 ? base.slice(0, 8) : base;
    return hasExt ? `${truncatedBase}.${ext}` : truncatedBase;
  }

  private recordClipboardHistory(text: string): void {
    if (text.length === 0) { return; }
    const history = AppStorage.get<string[]>('clipboardHistory') ?? [];
    if (history[0] === text) { return; }
    const next = [text, ...history.filter((t: string) => t !== text)]
      .slice(0, InputHandler.MAX_CLIPBOARD_HISTORY);
    AppStorage.setOrCreate('clipboardHistory', next);
  }

  // No input method on any platform can inject non-text content into an
  // arbitrary third-party app's text field through the standard IME
  // text-commit API (insertText here) -- that's not a bug to fix in this
  // app, it's what every keyboard app is limited to.
  //
  // For image/file clipboard contents this instead asks the host app's OWN
  // focused field to run ITS native paste (sendPasteExtendAction below) --
  // that reads the same system pasteboard the field's own long-press
  // 貼り付け already uses, so any editor that supports pasting an image at
  // all picks it up through its own existing handling. An earlier attempt
  // used ArkUI drag-and-drop instead (dragging the clipboard chip onto the
  // focused field), but that turned out not to work in nearly any real app
  // -- most fields' drop targets don't accept a dragged UnifiedData record
  // the way they accept their own paste action, so this replaces it as the
  // primary mechanism rather than sitting alongside it as a "maybe" path.
  pasteClipboard(): 'text' | 'extendAction' | 'none' {
    const isImage = AppStorage.get<boolean>('clipboardIsImage') ?? false;
    const isFile = AppStorage.get<boolean>('clipboardIsFile') ?? false;
    if (isImage || isFile) {
      const uri = AppStorage.get<string>('clipboardUri') ?? '';
      if (this.sendPasteExtendAction()) {
        InputLog.recordPaste(isImage ? 'image' : 'file', 0);
        // Dismiss the chip for this file/image until the clipboard changes.
        InputHandler.dismissedClipSig = InputHandler.clipSig('', uri, isImage);
        InputHandler.setClipboardPreviewState('', false, false, '');
        return 'extendAction';
      }
      return 'none';
    }
    const text = AppStorage.get<string>('clipboardPreview') ?? '';
    if (text.length === 0) {
      return 'none';
    }
    // クリップボードの中身は入力ログに残さない (貼り付けた事実と文字数だけ)。
    InputLog.recordPaste('chip', text.length);
    this.suppressInsertLog = true;
    this.insertText(text, { raw: true } as InsertTextOptions);
    this.suppressInsertLog = false;
    // Hide the paste chip now that it's been used, and remember this content so
    // subsequent refreshes keep it hidden until the user copies something new.
    InputHandler.dismissedClipSig = InputHandler.clipSig(text, '', false);
    InputHandler.setClipboardPreviewState('', false, false, '');
    return 'text';
  }

  // Tells the currently-focused edit box to run its own native paste
  // operation (inputMethodEngine.ExtendAction.PASTE) instead of the IME
  // trying to supply the content itself. Fire-and-forget: sendExtendAction
  // resolves/rejects on whether the *request* was delivered, not on whether
  // the field actually had a paste handler for image/file content -- both
  // "field doesn't support pasting images" and "there's nothing to paste"
  // look the same from here, so the caller can only report that the request
  // was sent, not that it definitely worked.
  private sendPasteExtendAction(): boolean {
    if (!this.inputClient) { return false; }
    try {
      this.inputClient.sendExtendAction(inputMethodEngine.ExtendAction.PASTE)
        .catch((err: Error) => console.error('shuntiIME sendExtendAction(PASTE) error', JSON.stringify(err)));
      return true;
    } catch (e) {
      console.error('shuntiIME sendExtendAction(PASTE) threw', JSON.stringify(e));
      return false;
    }
  }

  // Paste an arbitrary entry from clipboard history (not necessarily the
  // current system clipboard contents).
  // 固定した文を貼る / 固定する・外す (クリップボードのピン留め。ユーザー 2026-10-16)
  pastePinned(text: string): void {
    if (text.length === 0) { return; }
    InputLog.recordPaste('pinned', text.length);
    this.suppressInsertLog = true;
    this.insertText(text, { raw: true } as InsertTextOptions);
    this.suppressInsertLog = false;
  }

  togglePin(text: string): void {
    if (this.imeContext) { ClipboardPins.toggle(this.imeContext, text).catch((_e: Error) => {}); }
  }

  pasteFromHistory(index: number): void {
    const history = AppStorage.get<string[]>('clipboardHistory') ?? [];
    const text = history[index];
    if (text === undefined || text.length === 0) { return; }
    InputLog.recordPaste(`history#${index}`, text.length);
    this.suppressInsertLog = true;
    this.insertText(text, { raw: true } as InsertTextOptions);
    this.suppressInsertLog = false;
  }

  // ---- 入力ログ (デバッグビルドのみ。設定画面の「開発者用」から呼ぶ) ------
  //
  // 取り出しは USB デバッグ経由。拡張プロセスからはクリップボードへの書き込みも
  // サンドボックスの直接読み出しも通らないので、hilog に流して PC 側
  // (tools/pull_input_log.js) で拾う (InputLog.dumpToHilog 参照)。

  // 候補のタップがどこから来たか。▼の一覧 (ExpandedCandidateView) は
  // commitCandidate を呼んでから一覧を閉じるので、呼ばれた時点ではまだ開いている。
  private candidatePickVia(): string {
    return (AppStorage.get<boolean>('candidateExpanded') ?? false) ? 'list' : 'bar';
  }

  // フリック1回の詳細 (FlickKeyboardView から)。
  noteFlick(center: string, out: string, dir: string, dx: number, dy: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.recordFlick(center, out, dir, dx, dy);
  }

  // 候補バーの横スクロールが止まった (CandidateBar から)。
  noteCandidateScroll(offsetVp: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.recordScroll(this.conversionTarget(), offsetVp,
      (AppStorage.get<string[]>('candidates') ?? []).length);
  }

  // 候補の長押しメニューを開いた / 削除せずに閉じた。
  noteCandidateMenu(action: string, index: number): void {
    if (!DEBUG_INPUT_LOG) { return; }
    const cands = AppStorage.get<string[]>('candidates') ?? [];
    InputLog.recordMenu(action, this.conversionTarget(),
      index >= 0 && index < cands.length ? cands[index] : '', index);
  }

  // 記号・絵文字・設定・数字パッドなどのパネル切替 (KeyboardPage から)。
  noteSubMode(mode: string): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.recordSubMode(mode);
  }

  // 候補一覧 (▼) を開いた/選ばずに閉じた (CandidateBar・ExpandedCandidateView から)。
  noteCandidateListToggle(open: boolean): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.recordExpand(open, this.conversionTarget(),
      AppStorage.get<string[]>('candidates') ?? []);
  }

  publishInputLogSummary(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    AppStorage.setOrCreate('inputLogSummary', InputLog.summary());
  }

  exportInputLog(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    AppStorage.setOrCreate('inputLogSummary', '書き出し中… (PC側で pull_input_log.js を実行)');
    InputLog.dumpToHilog((chunks: number) => {
      const done = chunks < 0 ? '書き出しに失敗' : `書き出し完了 (${chunks}チャンク)`;
      AppStorage.setOrCreate('inputLogSummary', `${done} / ${InputLog.summary()}`);
    });
  }

  clearInputLog(): void {
    if (!DEBUG_INPUT_LOG) { return; }
    InputLog.clear();
    this.publishInputLogSummary();
  }

  // 「クリップボード履歴を消しても最新の1件だけ消えない」の実体はここ。
  // 履歴を空にしても OS 側のシステムクリップボードの中身はそのまま残って
  // いるため、次に refreshClipboardPreview が走った瞬間 (履歴画面を開き
  // 直す・入力欄でコミットする等) に recordClipboardHistory がその内容を
  // また履歴の先頭へ足し戻してしまっていた。消した時点の先頭
  // (=現在システムクリップボードにある内容とほぼ確実に一致する) を
  // dismissedClipSig に登録しておくことで、内容が変わるまでは
  // 足し戻されないようにする。
  clearClipboardHistory(): void {
    const history = AppStorage.get<string[]>('clipboardHistory') ?? [];
    if (history.length > 0) {
      InputHandler.dismissedClipSig = InputHandler.clipSig(history[0], '', false);
    }
    AppStorage.setOrCreate('clipboardHistory', [] as string[]);
  }

  // ホスト側(確定済み、IDLE状態)テキストへの1回の操作を記録する。
  // insertText() の末尾(挿入)と handleBackspace の IDLE 分岐(削除)から
  // 呼ばれる。at はその時点の hostCursor から自己完結で決まる (フィールド
  // コメント参照)。
  // 入力欄のカーソルの本当の位置 (HarmonyOS 7 で正しく取れる。取れなければ -1)
  private hostIndex(): number {
    try {
      const i = this.inputClient?.getTextIndexAtCursorSync() ?? -1;
      return i >= 0 ? i : -1;
    } catch (_e) {
      return -1;
    }
  }

  // pre: 入れる・消す前のカーソルの本当の位置 (-1 なら自分で数えた hostCursor を使う)
  private pushHostOp(kind: 'insert' | 'delete', text: string, pre: number = -2): void {
    if (text.length === 0) { return; }
    if (pre === -2) { pre = this.hostIndex(); }
    if (pre >= 0) { this.hostCursor = pre; }
    const at = kind === 'insert' ? this.hostCursor : this.hostCursor - text.length;
    const now = Date.now();
    const last = this.hostUndoStack.length > 0 ? this.hostUndoStack[this.hostUndoStack.length - 1] : undefined;
    // ⌫ を続けて押して消した分は 1 回の操作にまとめる (取消キー 1 回で消した語ごと戻る。ユーザー 2026-10-13)
    if (kind === 'delete' && last !== undefined && last.kind === 'delete' && last.at === this.hostCursor &&
      now - (last.t ?? 0) < InputHandler.UNDO_MERGE_MS) {
      last.text = text + last.text;
      last.at = at;
      last.t = now;
      this.markSelfEdit();
      this.hostCursor -= text.length;
      return;
    }
    this.hostUndoStack.push({ kind, text, at, t: now });
    if (this.hostUndoStack.length > InputHandler.MAX_HOST_UNDO) {
      this.hostUndoStack = this.hostUndoStack.slice(-InputHandler.MAX_HOST_UNDO);
    }
    this.markSelfEdit();
    this.hostCursor += kind === 'insert' ? text.length : -text.length;
  }

  // hostCursor を target まで動かす。1歩ずつ moveCursor の確定を待ってから
  // 次を出す -- insertText 直後に間を置かず moveCursor を出すと、ホスト側が
  // まだ編集を反映していない状態に対して移動が発行され無視される実機バグが
  // あった (コミット 39f4147)。取消キーはユーザーが最後の編集から間を置いて
  // 押す操作なのでこの経路そのものは起きないが、複数歩を連続で出すときの
  // 相互干渉を避けるため、ここでも律儀に1歩ずつ待つ。
  private async moveHostCursorTo(target: number): Promise<void> {
    // 本当の位置が取れるなら、1 字動かすたびに読み直して target まで (自分で数えるとずれた。selectByRange は
    // 範囲が空だとカーソルが動かなかった。どちらも 2026-10-14 実機)
    let cur = this.hostIndex();
    if (cur >= 0 && target >= 0) {
      for (let guard = 0; cur !== target && guard < 2000; guard++) {
        this.markSelfEdit();
        try {
          await this.inputClient?.moveCursor(target > cur ? inputMethodEngine.Direction.CURSOR_RIGHT : inputMethodEngine.Direction.CURSOR_LEFT);
        } catch (_e) {
          break;
        }
        let next = this.hostIndex();
        if (next === cur) {   // 位置の反映が遅れているだけかもしれないので、少し待って読み直す
          await new Promise<void>((r: () => void) => { setTimeout(r, 20); });
          next = this.hostIndex();
        }
        if (next === cur) { break; }   // 動かない (欄の端など)
        cur = next;
      }
      this.markSelfEdit();
      this.hostCursor = cur;
      return;
    }
    const dir = target > this.hostCursor
      ? inputMethodEngine.Direction.CURSOR_RIGHT
      : inputMethodEngine.Direction.CURSOR_LEFT;
    const steps = Math.abs(target - this.hostCursor);
    for (let i = 0; i < steps; i++) {
      try {
        this.markSelfEdit();
        await this.inputClient?.moveCursor(dir);
      } catch (_e) {
        break; // 動かせなかった。これ以上ズレを広げずここで諦める。
      }
      this.markSelfEdit();
      this.hostCursor += dir === inputMethodEngine.Direction.CURSOR_RIGHT ? 1 : -1;
    }
  }

  // ホスト側テキストの直近の操作(挿入でも削除でも)を1件取り消す。本当の
  // ctrl+zと同じ順序 -- 挿入の取り消しは削除、削除の取り消しは打ち戻し。
  // 公開の insertText() ラッパーは使わない (それを呼ぶと今の取り消し自体が
  // 新しい操作として hostUndoStack に積まれてしまい、スタックが正しく
  // 縮まなくなる)。
  private async undoLastHostEdit(): Promise<void> {
    const op = this.hostUndoStack.pop();
    if (op === undefined) { return; }
    InputLog.recordUndo(`${op.kind}:${op.text}`);
    if (op.kind === 'move') {
      await this.moveHostCursorTo(op.at);
      return;
    }
    if (op.kind === 'insert') {
      // 挿入の取り消し = その範囲を削除。挿入直後の位置までカーソルを
      // 動かしてから、その直前を deleteForward で削る。
      await this.moveHostCursorTo(op.at + op.text.length);
      try {
        this.markSelfEdit();
        await this.inputClient?.deleteForward(op.text.length);
        this.markSelfEdit();
        this.hostCursor -= op.text.length;
        for (let i = 0; i < op.text.length; i++) { this.insertedBuffer.pop(); }
      } catch (_e) { /* 欄が外れた等。ここで失敗しても入力は続けられる。 */ }
    } else {
      // 削除の取り消し = 元あった位置へ打ち戻す。
      await this.moveHostCursorTo(op.at);
      try {
        this.markSelfEdit();
        await this.inputClient?.insertText(op.text);
        this.markSelfEdit();
        this.hostCursor += op.text.length;
        for (const ch of op.text) { this.insertedBuffer.push(ch); }
      } catch (_e) { /* 同上。 */ }
    }
  }

  // 取消キー。COMPOSING(変換前のかな組み立て中)ならその場で1文字ずつ復元
  // する (このバッファは常に末尾でしか編集できないので位置のズレが起きない)。
  // IDLE(確定済みのホスト側テキスト)なら、直近の操作を実際に起きた位置まで
  // 戻ってから取り消す -- 挿入も削除も貼り付けも区別せず、本当のctrl+zと
  // 同じ順序で遡る。
  undoLastDeletion(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state === InputState.IDLE) {
      this.undoLastHostEdit();
      return;
    }
    const ch = this.composingUndoStack.pop();
    if (ch === undefined || ch.length === 0) { return; }
    InputLog.recordUndo(ch);
    this.handleDirectKana(ch);
  }

  // Launches the main app (settings / user dictionary screen) from the
  // keyboard's own settings panel. InputMethodExtensionContext extends
  // ExtensionContext, which supports startAbility for abilities in the same
  // bundle.
  openMainApp(): void {
    if (!this.imeContext) { return; }
    const want: Want = {
      bundleName: 'com.shunti.japaneseime',
      abilityName: 'EntryAbility',
    };
    this.imeContext.startAbility(want)
      .catch((err: Error) => console.error('shuntiIME openMainApp error', JSON.stringify(err)));
  }

  // AppGallery's review deep link needs this app's AppGallery Connect app ID,
  // which only exists once it has been published there — until then this
  // stays empty and the caller should show a "not published yet" message
  // instead of firing a broken want.
  private static readonly APP_GALLERY_APP_ID = '';

  openAppGalleryReview(): boolean {
    const id = InputHandler.APP_GALLERY_APP_ID;
    if (id.length === 0 || !this.imeContext) { return false; }
    const want: Want = { uri: `store://appgallery.huawei.com/app/detail?id=${id}` };
    this.imeContext.startAbility(want)
      .catch((err: Error) => console.error('shuntiIME openAppGalleryReview error', JSON.stringify(err)));
    return true;
  }

  // Opens an arbitrary URL in the main app's own in-app browser
  // (pages/WebViewPage, an ArkWeb view) rather than handing off to the
  // system's "choose a browser" chooser -- routed through EntryAbility's
  // want parameters (see EntryAbility.ets/Index.ets) since a URL want here
  // would open the system browser chooser instead.
  openExternalUrl(url: string, title: string): boolean {
    if (!this.imeContext) { return false; }
    const want: Want = {
      bundleName: 'com.shunti.japaneseime',
      abilityName: 'EntryAbility',
      parameters: { openUrl: url, openTitle: title },
    };
    this.imeContext.startAbility(want)
      .catch((err: Error) => console.error('shuntiIME openExternalUrl error', JSON.stringify(err)));
    return true;
  }

  // Opens this project's GitHub repository (README, source code, etc).
  openGithubRepo(): boolean {
    return this.openExternalUrl(
      'https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next', 'GitHub');
  }

  // Hands a URL to the main app to raise the system share sheet on (see
  // EntryAbility's shareUrl param / Index.onPendingShareUrlChanged). Routed
  // through EntryAbility exactly like openExternalUrl -- the share sheet needs
  // a UIAbilityContext, which an input-method extension doesn't have.
  shareUrl(url: string): boolean {
    if (!this.imeContext) { return false; }
    const want: Want = {
      bundleName: 'com.shunti.japaneseime',
      abilityName: 'EntryAbility',
      parameters: { shareUrl: url },
    };
    this.imeContext.startAbility(want)
      .catch((err: Error) => console.error('shuntiIME shareUrl error', JSON.stringify(err)));
    return true;
  }

  // The donation links raise the system share sheet instead of opening an
  // in-app browser: entering card details on a soft-keyboard IME is awkward
  // and the current device may not have a payment method registered at all,
  // so sharing the link (Huawei Share to another device, a messaging app, copy
  // link, …) lets the user complete payment wherever it's convenient. Linked
  // directly from the settings panel's 開発を応援 section (SettingsBar.ets) --
  // a deliberate call after reviewing AppGallery's review guidelines (App
  // Payment/Functions/Content/Ads): no rule found prohibiting a plain external
  // donation link that doesn't gate any app feature, and HUAWEI IAP is stated
  // as recommended, not required.
  openBuyMeACoffeeLink(): boolean {
    return this.shareUrl('https://buymeacoffee.com/shunti');
  }

  openGithubSponsorsLink(): boolean {
    return this.shareUrl('https://github.com/sponsors/shuntilettuce');
  }

  // 入力中のかなの表示先を UI 側へ配る。
  //
  // 入力欄側 (previewInField) は「設定でONかつ入力欄が対応している」ときだけ
  // 実際に効く。パネル側 (showComposingPanel) は設定そのままだが、入力欄側を
  // 選んでいるのに入力欄が非対応で、かつパネルもOFFだと、打っているかなが
  // どこにも出なくなる。それは設定の意図(入力欄に出す)ではなく端末側の都合
  // なので、その場合だけパネルに戻して見えなくならないようにする。
  // 両方を自分でOFFにした場合は意図どおり何も出さない (設定画面で警告済み)。
  private publishPreviewMode(): void {
    const fieldActive = this.composingInField && this.previewSupported;
    AppStorage.setOrCreate('previewInField', fieldActive);
    AppStorage.setOrCreate('showComposingPanel',
      this.composingInPanel || (this.composingInField && !fieldActive && !this.previewSupportPending));
    // 判定待ちの間は UI 側のフォールバック通知を抑止する (この間の
    // previewInField=false は「非対応」ではなく「まだ分からない」の意味)。
    AppStorage.setOrCreate('previewSupportPending', this.previewSupportPending);
  }

  // 入力中のかなをホストアプリの入力欄へ反映する。composing が空になったら
  // プレビューを終了する。updateCandidates の入口/出口から呼ぶので、
  // composing を変える経路すべてがここを通る。
  private syncPreviewText(): void {
    if (!this.previewSupported || !this.composingInField) { return; }
    const text = (AppStorage.get<string>('composingText') ?? '')
      + (AppStorage.get<string>('pendingRomaji') ?? '');
    if (text.length === 0) {
      this.cancelPreviewText();
      return;
    }
    try {
      // range は「置き換える範囲」。-1/-1 で現在のプレビュー(無ければ
      // カーソル位置)を差し替える指定になる。
      this.inputClient?.setPreviewTextSync(text, { start: -1, end: -1 });
      this.previewActive = true;
      this.previewLen = text.length;
    } catch (e) {
      // 12800011 = この入力欄はプレビュー非対応。以後は呼ばずパネル内表示へ
      // 戻す (対応可否は欄ごとなので、ここで諦めても次の欄でまた試される)。
      this.previewSupported = false;
      this.previewActive = false;
      this.previewLen = 0;
      this.publishPreviewMode();
      console.info('shuntiIME setPreviewText unsupported, falling back ' + JSON.stringify(e));
    }
  }

  // プレビューを「中身を残さずに」取り消す。
  //
  // finishTextPreviewSync() はプレビューを消すのではなく確定するので、
  // 単に呼ぶと打っていたかながそのまま入力欄に残ってしまう (全消ししたのに
  // 文字が残る)。空文字で置き換えてから確定するのが正しい取り消し方。
  // 空文字を拒否する入力欄向けに、確定してから同じ長さを消す経路も用意する。
  private cancelPreviewText(): void {
    if (!this.previewActive) { return; }
    const len = this.previewLen;
    this.previewActive = false;
    this.previewLen = 0;
    try {
      this.inputClient?.setPreviewTextSync('', { start: -1, end: -1 });
      this.inputClient?.finishTextPreviewSync();
    } catch (_e) {
      try {
        this.inputClient?.finishTextPreviewSync();
        if (len > 0) { this.inputClient?.deleteForward(len); }
      } catch (_e2) {
        // 欄が外れた等。ここで失敗しても入力は続けられる。
      }
    }
  }

  // 変換1回の所要時間を debug ビルドだけ集計する (InputLog の perf 行)。
  // リリースでは DEBUG_INPUT_LOG が false なので、そのまま中身を呼ぶだけ。
  private updateCandidates(): void {
    // 入力中の文字が無くなったら提案も消す (み を消したあとに提案だけ残っていた)
    if ((AppStorage.get<string>('composingText') ?? '').length === 0) {
      AppStorage.setOrCreate('suggestList', [] as string[]);
      AppStorage.setOrCreate('suggestPending', [] as string[]);
    }
    if (!DEBUG_INPUT_LOG) {
      this.updateCandidatesInner();
      return;
    }
    const t0 = Date.now();
    this.updateCandidatesInner();
    const ms = Date.now() - t0;
    InputLog.notePerf(this.enrichRun ? 'enrich' : 'conv', ms);
    if (AppStorage.get<string>('conversionEngine') === 'ai') {
      console.info(`shuntiIME updateCandidates ${(AppStorage.get<string>('composingText') ?? '').length}字 ${ms}ms`);
    }
  }

  private updateCandidatesInner(): void {
    this.syncPreviewText();
    const composing = AppStorage.get<string>('composingText') ?? '';
    if (composing.length === 0) {
      // 打っていた読みを変換せず全部消して諦めた (resetComposition 経由の
      // 確定後クリアはこの関数を通らないので、ここに来るのは常に backspace)。
      // この直後に同じ読みが手作業で組み立て直されたら強い確認信号になる。
      if (this.composingWatermark.length > 0) {
        UnknownWordLearner.noteAbandonedReading(this.composingWatermark);
        InputLog.recordAbandon(this.composingWatermark);
        this.composingWatermark = '';
      }
      AppStorage.setOrCreate('candidates', [] as string[]);
      AppStorage.setOrCreate('quasiCandidateSurfaces', [] as string[]);
      return;
    }
    if (composing.length > this.composingWatermark.length) { this.composingWatermark = composing; }
    // A new composition has started -- retire the post-commit next-word strip
    // so it doesn't linger next to live conversion candidates.
    AppStorage.setOrCreate('predictions', [] as string[]);
    this.predictionWordGroups = [];

    // conversionEnd defines the split point: text before it is converted,
    // text after stays as kana. 0 means convert the full text.
    const target = this.conversionTarget();

    // 直前の打鍵から間が無いときは「おまけ」を後回しにする。後回しにしたら
    // 指が止まったときに enrich() が同じ読みで計算し直す。
    const now = Date.now();
    const typingFast = now - this.lastKeystrokeAt < ENRICH_DELAY_MS;
    if (!this.enrichRun) { this.lastKeystrokeAt = now; }
    const skipExtras = typingFast && !this.enrichRun && target.length >= EXTRAS_MIN_KANA;
    clearTimeout(this.enrichTimer);
    this.enrichPending = false;
    const fullKatakana = KanaKanjiConverter.toKatakana(target);
    // AI 変換: 変換は裏のスレッドで行い、届いたら updateCandidates をもう一度呼ぶ (打鍵を止めない)。
    // 届くまでは前回の候補に新しく打ったかなをつないで出す (AiConverter.candidates)。読み込みが済むまでは読みのまま
    const aiMode = AiConverter.isReady();
    let aiCands: string[] = [];
    if (aiMode) {
      aiCands = AiConverter.candidates(KanaKanjiConverter.recentContext(), target, () => {
        if (this.conversionTarget() === target) { this.updateCandidates(); }
      });
    }
    // 長い入力で前の方が固定されても入力欄には確定しない (AiConverter が固定した部分に候補を持たせ、一覧から選び直せる)
    const segs = [target];
    // 入力ログ用: 各候補をどの段が列に入れたか (InputLog の o 欄)。段ごとに
    // 「まだ印の無い候補」へ印を付けていくので、先に入れた段が勝つ。
    const origin = new Map<string, string>();
    const promo = new Map<string, string>();
    const tagNew = (list: string[], tag: string): void => {
      if (!DEBUG_INPUT_LOG) { return; }
      for (const c of list) {
        if (!origin.has(c)) { origin.set(c, tag); }
      }
    };
    this.lastAlignment = undefined;
    let candidates: string[] = AiConverter.preferGreetings(target, [...aiCands]);
    if (!candidates.includes(target)) { candidates.push(target); }
    if (!candidates.includes(fullKatakana)) { candidates.push(fullKatakana); }
    tagNew(candidates, aiMode ? 'ai' : 'kana');
    // ユーザー辞書に登録した語は、読みがちょうど一致したら先頭に (AI 変換の網にも入っているが、確実に出す。Android 版と同じ)
    const user = this.userDictRaw[target];
    if (user !== undefined && user.length > 0) {
      candidates = [...user, ...candidates.filter((c: string) => !user.includes(c))];
    }
    tagNew(candidates, 'user');

    // General learned-context rerank: if the user has actually written one of
    // these candidates after the current context before (映画を→見る, 資料を→作る),
    // pull it to the front. Unlike the hand-curated CONTEXT_HINTS table this
    // generalises to EVERY word pair the user types -- no per-homophone coding,
    // it just learns. Empty (no reorder) until the pair has been written, and
    // it only ever matches a full-word candidate, so partial-input candidate
    // lists are untouched.
    tagNew(candidates, 'reseg');
    const topBeforeCtx = candidates.length > 0 ? candidates[0] : '';
    candidates = this.applyContextRerank(candidates);
    if (DEBUG_INPUT_LOG && candidates.length > 0 && candidates[0] !== topBeforeCtx) {
      promo.set(candidates[0], 'C');
    }

    // Predictive completion: once enough of a known common phrase's reading
    // has been typed to be unambiguous, lead with the whole thing rather than
    // making the user type out a full greeting/set phrase.
    const predictive = findPredictiveCompletion(target);
    if (predictive !== null && !candidates.includes(predictive)) {
      candidates = [predictive, ...candidates];
    }

    tagNew(candidates, 'phrase');

    // Predictive conversion (予測変換): common words whose reading EXTENDS what's
    // been typed so far (かいし→会社, にほん→日本語), inserted right after the exact
    // conversion so they're one tap away without finishing the reading. Only
    // fires for words that are strictly longer than the prefix, so a fully-typed
    // reading (which the exact conversion already handles) adds nothing here --
    // this is why it never disturbs the whole-sentence conversion candidates.
    let insAt = candidates.length > 0 ? 1 : 0;
    // 未知語学習: a word this user has hand-assembled from single kanji / by
    // trimming a longer conversion enough times (see UnknownWordLearner) is
    // something the dictionaries genuinely cannot produce, so it leads the
    // injected alternatives -- it is the one entry here that exists solely
    // because this user kept asking for it.
    for (const u of UnknownWordLearner.candidates(target)) {
      if (!candidates.includes(u)) { candidates.splice(insAt, 0, u); insAt++; }
    }
    tagNew(candidates, 'unk');
    // Date/time expansion (きょう→2026年7月23日, いま→15:05, あした→翌日…) leads the
    // alternatives for its trigger readings -- when someone types きょう the live
    // date is a likelier intent than a prefix-completion (教育 for きょういく).
    // Ephemeral live values -- excluded from conversion learning by
    // isNonLearningCandidate.
    const dateTimes = DateTimePredictor.predict(target, new Date());
    for (const d of dateTimes) {
      if (!candidates.includes(d)) { candidates.splice(insAt, 0, d); insAt++; }
    }

    tagNew(candidates, 'date');

    // Common-word prefix completions (かいし→会社), after any date/time values.
    const completions = PredictiveConversion.predict(target, KanaKanjiConverter.getLearned(),
      KanaKanjiConverter.prefixCompletions(target, PREDICT_DICT_HITS),
      ShortTermMemory.hitsForPrefix(target));
    for (const c of completions) {
      if (!candidates.includes(c)) { candidates.splice(insAt, 0, c); insAt++; }
    }

    tagNew(candidates, 'pred');

    // Numeral formatting (1000000→1,000,000 / 100万 / 百万) for bare-digit input.
    // Ephemeral like the above -- excluded from conversion learning.
    const numForms = NumberFormatter.predict(target);
    for (const nf of numForms) {
      if (!candidates.includes(nf)) { candidates.splice(insAt, 0, nf); insAt++; }
    }

    // メールアドレス: @ の後ろにドメインの候補 (taro@ -> taro@gmail.com …、taro@ya -> taro@yahoo.co.jp)。ユーザー 2026-10-13
    for (const m of emailCompletions(target)) {
      if (!candidates.includes(m)) { candidates.splice(insAt, 0, m); insAt++; }
    }
    // あっと -> 全角の ＠ しか無かった。メールアドレスに使う半角の @ をその前に
    const fwAt = candidates.indexOf('＠');
    if (fwAt >= 0 && !candidates.includes('@')) { candidates.splice(fwAt, 0, '@'); }

    // 括弧は全種類を必ず出す (Brackets.ets)。括弧 1 字を打ったときは同じ側の全種類を先頭から
    // (打った字が 1 位)、かっこ などの読みでは全種類の組を 1 位の後ろに足す。
    const brackets = bracketVariants(target);
    if (brackets.length > 0 && brackets[0] === target) {
      candidates = [...brackets, ...candidates.filter((c: string) => !brackets.includes(c))];
    } else {
      for (const b of brackets) {
        // 辞書に下の方で入っている組 (（）「」 など) も引き上げる
        const at = candidates.indexOf(b);
        if (at >= 0 && at < insAt) continue;
        if (at >= 0) candidates.splice(at, 1);
        candidates.splice(insAt, 0, b); insAt++;
      }
    }
    tagNew(candidates, 'bracket');

    // Now that every source has contributed, pull whatever this user has
    // actually been picking for this reading to the front. Applied here rather
    // than relying on lookup()'s own learning boost because most of the list
    // above never passed through lookup() -- see applyLearnedOrder.
    tagNew(candidates, 'num');
    const topBeforeLearn = candidates.length > 0 ? candidates[0] : '';
    candidates = KanaKanjiConverter.applyLearnedOrder(target, candidates);
    if (DEBUG_INPUT_LOG && candidates.length > 0 && candidates[0] !== topBeforeLearn) {
      promo.set(candidates[0], 'L');
    }
    // 準候補 (quasi-candidates): まだ LEARN_THRESHOLD 未満の、組み立てかけの
    // 未知語。確定候補(上の UnknownWordLearner.candidates)とは違い列の末尾に
    // だけ足す -- まだ確認できていない語を目立つ位置に出すと通常の変換を
    // 押しのけてしまうため。CandidateBar 側は quasiCandidateSurfaces を見て
    // うっすら区別して表示する。
    const quasi = UnknownWordLearner.quasiCandidates(target);
    for (const q of quasi) {
      if (!candidates.includes(q)) { candidates.push(q); }
    }
    tagNew(candidates, 'quasi');
    AppStorage.setOrCreate('quasiCandidateSurfaces', quasi);
    // 半角カタカナ (ほかに出す手段が無かった。ユーザー 2026-10-13)。いちばん後ろに
    const halfKata = toHalfKatakana(fullKatakana);
    if (halfKata !== fullKatakana && !candidates.includes(halfKata)) { candidates.push(halfKata); }
    // Candidates the user deleted by long-pressing them are dropped last, so
    // every source above (dictionaries, learning, the injected predictions) is
    // covered by the one filter -- see SuppressedCandidates.
    let shown = SuppressedCandidates.filter(target, candidates);
    // 提案の帯に出ているもの (打ち間違いの直し・日付・住所) は候補の帯から外す。日付の提案があるときは、
    // こちらの日付の予測 (DateTimePredictor、形が少し違う) もまとめて外す
    const sugg = AppStorage.get<string[]>('suggestPending') ?? [];   // 帯に出る前 (0.2 秒待つ間) でも外す
    if (sugg.length > 0) {
      const kinds = AppStorage.get<number[]>('suggestPendingKinds') ?? [];
      const dates = kinds.indexOf(2) >= 0 ? DateTimePredictor.predict(target, new Date()) : [] as string[];
      const rest = shown.filter((c: string) => sugg.indexOf(c) < 0 && dates.indexOf(c) < 0);
      if (rest.length > 0) { shown = rest; }
    }
    AppStorage.setOrCreate('candidates', shown);
    AppStorage.setOrCreate('selectedCandidateIndex', 0);
    if (DEBUG_INPUT_LOG) {
      promo.forEach((mark: string, c: string) => {
        const o = origin.get(c);
        if (o !== undefined) { origin.set(c, `${o}^${mark}`); }
      });
      InputLog.noteCandidateOrigins(target, origin, '');
    }
    InputLog.recordUpdate(composing, AppStorage.get<string>('pendingRomaji') ?? '',
      this.conversionEnd, shown, segs);
    if (skipExtras) {
      // 指が止まったら同じ読みで計算し直す。読みが変わっていれば次の
      // updateCandidates が先に走ってこのタイマーは捨てられる。
      this.enrichPending = true;
      this.enrichTimer = setTimeout(() => { this.enrichCandidates(composing); }, ENRICH_DELAY_MS);
    }
  }

  // 後回しにした「おまけ」候補を、指が止まってから足すためだけの再計算。
  // enrichRun を立てて updateCandidates をもう一度通すので、候補列の作り方は
  // 1通りのまま (ここで別に組み立てると本体とずれる)。
  private enrichRun: boolean = false;
  private enrichCandidates(reading: string): void {
    if (!this.enrichPending) { return; }
    this.enrichPending = false;
    // 打ち続けて読みが変わっていたら、その読みの更新がもう走っている。
    if ((AppStorage.get<string>('composingText') ?? '') !== reading) { return; }
    if ((AppStorage.get<string>('inputState') ?? InputState.IDLE) !== InputState.COMPOSING) { return; }
    // 候補を選んでいる途中(先頭以外を見ている)なら、列を入れ替えない。
    if ((AppStorage.get<number>('selectedCandidateIndex') ?? 0) !== 0) { return; }
    this.enrichRun = true;
    try {
      this.updateCandidates();
    } finally {
      this.enrichRun = false;
    }
  }

  handleVariantCycle(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state !== InputState.COMPOSING) { return; }
    const composing = AppStorage.get<string>('composingText') ?? '';
    if (composing.length === 0) { return; }
    const last = composing.slice(-1);
    const next = VARIANT_CYCLE[last];
    if (next !== undefined) {
      InputLog.recordVariant('cycle', last, next);
      AppStorage.setOrCreate('composingText', composing.slice(0, -1) + next);
      this.updateCandidates();
    }
  }

  handleCursorLeft(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    const composing = AppStorage.get<string>('composingText') ?? '';
    if (state === InputState.BUNSETSU) {
      this.selectBunsetsuSegment(this.bCur - 1);
    } else if ((state === InputState.COMPOSING || state === InputState.SELECTING) && composing.length > 0) {
      if (this.conversionEnd === 0) {
        this.conversionEnd = composing.length;
      }
      if (this.conversionEnd > 1) {
        this.conversionEnd--;
        AppStorage.setOrCreate('convEnd', this.conversionEnd);
        InputLog.recordRange(composing, this.conversionEnd, 'L');
        this.updateCandidates();
      } else {
        InputLog.recordRange(composing, this.conversionEnd, 'L(端)');
      }
    } else {
      InputLog.recordCursor(`L ${state}`);
      // 実際に動いた分だけ hostCursor に反映する。取消キーの位置追跡は
      // このIME自身が動かした分の自己申告なので、ここを飲むと矢印キーでの
      // 移動後に取消を押しても正しい位置へ戻れる (hostUndoStack のコメント
      // 参照)。動かせたかどうか確認できないので、成功した(=例外が出なかった)
      // ときだけ加算する。
      const hi = this.hostIndex();
      const from = hi >= 0 ? hi : this.hostCursor;
      this.markSelfEdit();
      this.inputClient?.moveCursor(inputMethodEngine.Direction.CURSOR_LEFT)
        .then(() => { this.markSelfEdit(); this.pushHostMove(from); this.hostCursor -= 1; })
        .catch((_e: Error) => {});
    }
  }

  handleCursorRight(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    const composing = AppStorage.get<string>('composingText') ?? '';
    if (state === InputState.BUNSETSU) {
      this.selectBunsetsuSegment(this.bCur + 1);
    } else if ((state === InputState.COMPOSING || state === InputState.SELECTING) && composing.length > 0) {
      if (this.conversionEnd > 0 && this.conversionEnd < composing.length) {
        this.conversionEnd++;
        AppStorage.setOrCreate('convEnd', this.conversionEnd);
        InputLog.recordRange(composing, this.conversionEnd, 'R');
        this.updateCandidates();
      } else {
        // Already at full length, no change needed
        InputLog.recordRange(composing, composing.length, 'R(端)');
      }
    } else {
      // 閉じ括弧が保留されている間の ► は、カーソル移動より先に
      // 「括弧をここで閉じる」を担当する。括弧の範囲を利用者自身が
      // 決めるための操作で、閉じたあとにもう一度押せば通常どおり
      // カーソルが動く (BRACKET_PAIRS のコメント参照)。
      const closing = this.pendingBracketClose;
      if (this.flushPendingBracketClose()) {
        InputLog.recordCursor(`R 閉じ括弧=${closing} ${state}`);
        return;
      }
      InputLog.recordCursor(`R ${state}`);
      const hi = this.hostIndex();
      const from = hi >= 0 ? hi : this.hostCursor;
      this.markSelfEdit();
      this.inputClient?.moveCursor(inputMethodEngine.Direction.CURSOR_RIGHT)
        .then(() => { this.markSelfEdit(); this.pushHostMove(from); this.hostCursor += 1; })
        .catch((_e: Error) => {});
    }
  }

  // 数字パッドの数字・記号と、入力中の文字があるときの英字。英字モードの中でも入力中の文字に足して変換する
  // (2000 -> 2,000・二千・２０００、日本語に戻って えん と続ければ 2000円、ぱちんこ + CR -> パチンコCR)。
  // あA キーでは入力中の文字を確定しないので、モードをまたいで続けて打てる
  appendRaw(ch: string): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    InputLog.recordKana(ch, 'raw');
    if (state === InputState.SELECTING || state === InputState.BUNSETSU) {
      this.commitTopCandidate();
    }
    const current = AppStorage.get<string>('composingText') ?? '';
    AppStorage.setOrCreate('composingText', current + ch);
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('inputState', InputState.COMPOSING);
    this.conversionEnd = (current + ch).length;
    AppStorage.setOrCreate('convEnd', this.conversionEnd);
    this.updateCandidates();
  }

  handleDirectKana(kana: string): void {
    const mode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    InputLog.recordKana(kana, mode);

    if (mode === InputMode.ALPHANUMERIC) {
      // 英数字も入力中の文字として組み立てて変換する (handleKeyPress と同じ。設定がオフなら入力中の文字があるときと数字だけ)
      if (this.alphaComposeOn() || (AppStorage.get<string>('composingText') ?? '').length > 0 || /^[0-9]$/.test(kana)) {
        this.appendRaw(kana);
        return;
      }
      this.commitPending();
      // raw: true -- 英数モードは1打鍵ごとに insertText が飛ぶ (かなモード
      // のように単語単位でまとまらない)。素のまま扱わないと、括弧を開いた
      // 直後に "t" を1文字打っただけで閉じ括弧が確定してしまい、続く
      // "est" が閉じの外側に出てしまう。保留中の閉じは英数入力では
      // 消費も生成もしない(消さずに残す -- 後で かな側の確定があれば
      // そちらで正しく閉じられる)。
      this.insertText(kana, { raw: true } as InsertTextOptions);
      return;
    }

    if (state === InputState.SELECTING || state === InputState.BUNSETSU) {
      this.commitTopCandidate();
    }

    const current = AppStorage.get<string>('composingText') ?? '';
    AppStorage.setOrCreate('composingText', current + kana);
    AppStorage.setOrCreate('pendingRomaji', '');
    AppStorage.setOrCreate('inputState', InputState.COMPOSING);
    this.conversionEnd = (current + kana).length;
    AppStorage.setOrCreate('convEnd', this.conversionEnd);
    // composingUndoStack はここで消さない -- 「けす→ら足す→やっぱりさっき
    // 消した方を戻したい」のように、この回の組み立て中はbackspace履歴を
    // 保ったまま何度でも取消キーで遡れるようにする (resetComposition で
    // 確定したときだけ、その回の組み立ては終わりとして空にする)。
    this.updateCandidates();
  }

  // Enter bunsetsu mode: segment composing text and show first segment's candidates
  handleBunsetsu(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state !== InputState.COMPOSING) { return; }
    let composing = AppStorage.get<string>('composingText') ?? '';
    const pending = this.converter.flushPending();
    const mode = AppStorage.get<string>('inputMode') ?? InputMode.HIRAGANA;
    if (pending.length > 0) {
      const kana = mode === InputMode.KATAKANA ? toKatakana(pending) : pending;
      composing = composing + kana;
      AppStorage.setOrCreate('composingText', composing);
      AppStorage.setOrCreate('pendingRomaji', '');
    }
    if (composing.length === 0) { return; }
    this.splitBunsetsu(composing);
    this.bCur = 0;
    InputLog.recordBunsetsu(composing, this.bSegs);
    this.updateBunsetsuStorage();
  }

  // 文節に分ける: 入力全体を AI 変換して、1 位の候補の語の区切りで分ける。かなだけの語 (助詞・助動詞・送り) は
  // 前の語につなげる (しゃしん|を|とっ|た -> 写真を|撮った)。区切りが取れなければ全体を 1 文節に
  private splitBunsetsu(composing: string): void {
    const r = AiConverter.convertNow(KanaKanjiConverter.recentContext(), composing, 1);
    const top = r.cands.length > 0 ? r.cands[0] : composing;
    this.bSegs = [];
    this.bChosen = [];
    if (r.ends.length === 0) {
      this.bSegs.push(composing);
      this.bChosen.push(top);
      return;
    }
    const kanaOnly = (t: string): boolean => /^[ぁ-ゖー]+$/.test(t);
    let rs = 0;
    let ss = 0;
    for (let i = 0; i < r.ends.length; i++) {
      const reading = composing.slice(rs, r.ends[i]);
      const surf = top.slice(ss, ss + r.lens[i]);
      rs = r.ends[i];
      ss += r.lens[i];
      if (this.bSegs.length > 0 && kanaOnly(surf) && '、。！？!?'.indexOf(reading) < 0) {
        this.bSegs[this.bSegs.length - 1] += reading;
        this.bChosen[this.bChosen.length - 1] += surf;
      } else {
        this.bSegs.push(reading);
        this.bChosen.push(surf);
      }
    }
    if (rs < composing.length) {   // 念のため: 区切りが読みの終わりまで届いていなければ、残りを最後の文節に
      this.bSegs[this.bSegs.length - 1] += composing.slice(rs);
      this.bChosen[this.bChosen.length - 1] += composing.slice(rs);
    }
  }

  // i 番目の文節の候補: 今の表記を先頭に、前の文節 (今の表記) を文脈にして AI 変換した候補
  private bunsetsuCandidates(i: number): string[] {
    const seg = this.bSegs[i];
    const ctx = (KanaKanjiConverter.recentContext() + this.bChosen.slice(0, i).join('')).slice(-40);
    const out: string[] = [this.bChosen[i]];
    for (const c of AiConverter.convertNow(ctx, seg, 30).cands.concat([seg, KanaKanjiConverter.toKatakana(seg)])) {
      if (out.indexOf(c) < 0) { out.push(c); }
    }
    return KanaKanjiConverter.applyLearnedOrder(seg, out);
  }

  // Navigate to a specific segment and refresh candidates
  selectBunsetsuSegment(idx: number): void {
    if (idx < 0 || idx >= this.bSegs.length) {
      InputLog.recordBunsetsuFocus(this.bSegs, this.bChosen, this.bCur, idx);
      return;
    }
    InputLog.recordBunsetsuFocus(this.bSegs, this.bChosen, this.bCur, idx);
    this.bCur = idx;
    this.updateBunsetsuStorage();
  }

  // Commit all segments as final text
  commitBunsetsu(): void {
    const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
    if (state !== InputState.BUNSETSU) { return; }
    const text = this.bChosen.join('');
    InputLog.recordBunsetsuFinal(this.bSegs.join(''), text, this.bChosen);
    for (let i = 0; i < this.bSegs.length; i++) {
      KanaKanjiConverter.recordChoice(this.bSegs[i], this.bChosen[i]);
      // 「足立レイ」のような、辞書が読み全体では出せず自動で2文節に割れて
      // 毎回セグメントごとにタップし直している名前も、UnknownWordLearner
      // から見れば PIECEWISE (音+街+ウナ 方式) と全く同じ信号のはず -- 唯一の
      // 違いは、別々の確定を積み重ねて手で組み立てるか、1回の文節変換の中で
      // セグメントごとに選ぶかだけ。今まではここが observeUnknownWord 系を
      // 一切呼んでおらず、文節変換で組み立てた語がいつまで経っても
      // 「あだちれい」→「足立レイ」の一発候補に昇格しなかった。ここで各
      // セグメントを普通の確定と同じように observe() へ流し込む (run は
      // markSentenceBoundary 等の既存の境界処理でそのうち flush される)。
      UnknownWordLearner.observe(this.bSegs[i], this.bChosen[i],
        (r: string, s: string): boolean => this.isKnownConversion(r, s));
      ShortTermMemory.record(this.bSegs[i], this.bChosen[i]);
    }
    this.saveUnknownWords();
    this.insertText(text);
    // 複数文節にまたがる文の確定はここが通り道で、commitCandidate/
    // commitTopCandidate と違って pushPredictions() が一度も呼ばれていな
    // かった。次単語予測の学習(NextWordPredictor.record)も予測ストリップの
    // 更新も pushPredictions 経由なので、文節変換で文を打つ限り学習データが
    // 全く育たず、予測が「ずっと変わってないレパートリー」に見えていた。
    const words = this.bChosen.slice();
    this.bSegs = [];
    this.bChosen = [];
    this.bCur = 0;
    this.resetComposition();
    this.saveLearning();
    this.pushPredictions(words);
  }

  // Called when user taps a candidate while in BUNSETSU state
  commitBunsetsuCandidate(index: number): void {
    const candidates = AppStorage.get<string[]>('candidates') ?? [];
    if (index < candidates.length) {
      InputLog.recordBunsetsuPick(this.bSegs[this.bCur], candidates[index], index,
        candidates.length, candidates, this.candidatePickVia(),
        [] as string[]);
      KanaKanjiConverter.recordChoice(this.bSegs[this.bCur], candidates[index]);
      this.bChosen[this.bCur] = candidates[index];
    }
    // Advance to next segment if available
    if (this.bCur + 1 < this.bSegs.length) {
      this.bCur += 1;
      this.updateBunsetsuStorage();
    } else {
      this.commitBunsetsu();
    }
  }

  private updateBunsetsuStorage(): void {
    const segTexts: string[] = this.bChosen.map((ch: string, i: number) =>
      i === this.bCur ? `[${ch}]` : ch
    );
    AppStorage.setOrCreate('bunsetsuTexts', this.bChosen as string[]);
    AppStorage.setOrCreate('bunsetsuRaw', this.bSegs as string[]);
    AppStorage.setOrCreate('bunsetsuCurrent', this.bCur);
    const seg = this.bSegs[this.bCur];
    const cands = SuppressedCandidates.filter(seg, this.bunsetsuCandidates(this.bCur));
    AppStorage.setOrCreate('candidates', cands);
    AppStorage.setOrCreate('selectedCandidateIndex', 0);
    AppStorage.setOrCreate('inputState', InputState.BUNSETSU);
  }

  // raw: true は括弧の自動閉じを掛けない (貼り付け・予測タップなど、
  // 「打ち込んだ通りそのまま出したい」文字列専用)。ユーザーがコピーした
  // 内容がたまたま "()" のような2文字と一致しても、括弧機能に横取りされて
  // 別の場所へ送られては困るため。
  insertText(text: string, opts?: InsertTextOptions): void {
    // 辞書の自由入力中は、確定した文字をホストアプリではなくパネル内の欄に送る。
    //
    // キーボードのパネルに TextInput を置いても、キーボード自身が入力対象に
    // なるため通常の経路では文字を入れられない。一方で「確定した文字が外に出る
    // 唯一の出口」はこの insertText なので、ここだけ差し替えれば、ローマ字入力・
    // かな漢字変換・候補選択といった仕組みはそのまま使える。
    if (this.captureTarget.length > 0) {
      if (!this.suppressInsertLog) { InputLog.recordInsert(text, 'capture'); }
      this.appendCaptured(text);
      return;
    }

    // 括弧の自動閉じ (設計意図は BRACKET_PAIRS のコメント参照)。
    // カーソルは一切動かさず、開きだけを今確定して、閉じは次の確定の
    // 直後まで保留する。
    let finalText = text;
    if (opts?.raw !== true) {
      let newOpen: string | undefined;
      let newClose: string | undefined;
      if (text.length === 2 && BRACKET_PAIRS[text[0]] === text[1]) {
        // 「（）」のように開き+閉じ1組で確定された(かっこ等の変換候補)。
        newOpen = text[0];
        newClose = text[1];
      } else if (text.length === 1 && BRACKET_PAIRS[text] !== undefined) {
        // 記号パネルで開き括弧を単体でタップした場合も同様に扱う。
        newOpen = text;
        newClose = BRACKET_PAIRS[text];
      }
      if (newOpen !== undefined && newClose !== undefined) {
        // 保留中の古い閉じがあれば、入れ子にはせず先に閉じてしまう。次の
        // 確定が古い括弧の中身か新しい括弧の中身か区別する手段が無いため、
        // 単純に「前のは前ので完結させ、新しい括弧を開く」にしてある。
        if (this.bracketCloseMode() === 'pair') {
          // HarmonyOS 7 から: 開きと閉じを一緒に入れて、カーソルを 1 つ左 (括弧の中) へ。保留は使わない
          // (ユーザー 2026-10-13。昔は moveCursor が実機で信用できず保留式にしていた)
          finalText = this.pendingBracketClose + newOpen + newClose;
          this.setPendingBracketClose('');
          this.cursorIntoPair = true;
        } else {
          finalText = this.pendingBracketClose + newOpen;
          this.setPendingBracketClose(newClose);
        }
      } else if (this.pendingBracketClose.length > 0) {
        if (text === this.pendingBracketClose) {
          // ユーザーが閉じ括弧を自分で打った。保留分と合わせて二重に
          // 出すのではなく、保留を消費してそのまま1つだけ出す
          // (「自分で閉じた」として扱う)。これが無いと "）" が
          // "））" になって出ていた (実際の不具合報告の原因)。
          this.setPendingBracketClose('');
        }
        // 'manual' では確定の内容にも保留にも手を付けない。閉じが出るのは
        // ► キー・待機バッジのタップ・送信直前のいずれかで
        // flushPendingBracketClose が呼ばれたときだけ。
      }
    }

    // 入力欄プレビューが出ているときは insertText を使わない。
    //
    // finishTextPreviewSync() は「プレビューを消す」のではなく「プレビューの
    // 内容をその場で確定する」API だった。そのため 解除 → insertText の順に
    // 呼ぶと、確定したかな + 変換結果 で入力欄に二重に残る (実機で確認)。
    // 正しくは、プレビュー自体を最終的な文字列で置き換えてから確定する。
    let previewCommitted = false;
    const preIdx = this.hostIndex();   // 入れる前の本当の位置 (取消キー用)
    if (this.previewActive) {
      try {
        this.inputClient?.setPreviewTextSync(finalText, { start: -1, end: -1 });
        this.inputClient?.finishTextPreviewSync();
        this.previewActive = false;
        previewCommitted = true;
      } catch (_e) {
        // 置き換えに失敗したらプレビューを畳んで通常の挿入に落とす。
        this.cancelPreviewText();
      }
    }
    this.markSelfEdit();
    if (!previewCommitted && this.cursorIntoPair) {
      // 括弧を組で入れた: 同期の API で入れてすぐ中へ動かす (待ってから動かすとカーソルが跳ぶのが見えた。ユーザー 2026-10-13)
      try {
        this.inputClient?.insertTextSync(finalText);
        this.inputClient?.moveCursorSync(inputMethodEngine.Direction.CURSOR_LEFT);
        this.cursorIntoPair = false;
        this.markSelfEdit();
        this.hostCursor -= 1;
      } catch (_e) {
        this.inputClient?.insertText(finalText);   // 同期の API が使えない: 下で少し待ってから動かす
      }
    } else if (!previewCommitted) { this.inputClient?.insertText(finalText); }
    if (!this.suppressInsertLog) {
      InputLog.recordInsert(finalText,
        `${previewCommitted ? 'preview' : 'direct'}${opts?.raw === true ? ' raw' : ''}`);
    }
    if (finalText.length > 0) {
      for (let i = 0; i < finalText.length; i++) {
        this.insertedBuffer.push(finalText[i]);
      }
      if (this.insertedBuffer.length > 50) {
        this.insertedBuffer = this.insertedBuffer.slice(-50);
      }
      // 変換確定・生かな確定・記号/絵文字タップ・予測変換タップ・
      // クリップボード貼り付け...どの経路で来ても、ホストへ実際に文字が
      // 出た時点はここ1箇所なので、取消キー用の記録もここで行う。
      this.pushHostOp('insert', finalText, preIdx);
      // @ を入れた (あっと の変換・記号の欄・英数字キーボード): 予測の帯にメールのドメインを出す (ユーザー 2026-10-13)。
      // 確定の後に呼ばれる pushPredictions が帯を書き換えるので、その後で
      // 文の流れ (SentenceFlow) にも入れて、確定の後の帯に「。」や次の文を出す (メールのドメインが先)
      SentenceFlow.feed(finalText);
      if (this.cursorIntoPair) {
        this.cursorIntoPair = false;
        // insertText の直後に間を置かず動かすと無視される実機バグがあった (moveHostCursorTo のコメント) ので、少し待ってから
        setTimeout(() => {
          this.inputClient?.moveCursor(inputMethodEngine.Direction.CURSOR_LEFT).then(() => { this.markSelfEdit(); this.hostCursor -= 1; }).catch(() => {});
        }, 30);
      }
      setTimeout(() => {
        if (!this.showEmailDomains()) { this.showFlowPredictions(); }
      }, 0);
    }
  }

  // 入力欄の @ の後ろに打ったドメインの続き (gmail.com・yahoo.co.jp …) を予測の帯に出す。押すとその続きを入れる
  private emailStripShown: string = '';   // 出しているメールのドメインの帯 (消したときに出し直すため)

  private showEmailDomains(): boolean {
    const m = /[A-Za-z0-9._%+\-]*[@＠]([A-Za-z0-9.\-]*)$/.exec(this.insertedBuffer.join(''));
    if (m === null) { return false; }
    const typed = m[1].toLowerCase();
    const strip = EMAIL_DOMAINS.filter((d: string) => d.startsWith(typed) && d !== typed).slice(0, 6)
      .map((d: string) => d.slice(typed.length));
    if (strip.length === 0) { return false; }
    AppStorage.setOrCreate('predictions', strip);
    this.predictionWordGroups = strip.map((t: string) => [t]);
    this.emailStripShown = strip.join('');
    return true;
  }

  // 文の流れの予測を、確定の後の帯の先頭に足す (お疲れ様です -> 。 -> 本日の進捗報告です。)
  private showFlowPredictions(): void {
    if ((AppStorage.get<string>('inputState') ?? InputState.IDLE) !== InputState.IDLE) { return; }
    const f = SentenceFlow.predict();
    if (f.length === 0) { return; }
    const cur = AppStorage.get<string[]>('predictions') ?? [];
    const strip: string[] = [];
    const groups: string[][] = [];
    for (const t of f) {
      if (strip.indexOf(t) < 0) { strip.push(t); groups.push([t]); }
    }
    for (let i = 0; i < cur.length && strip.length < 16; i++) {
      if (strip.indexOf(cur[i]) < 0) {
        strip.push(cur[i]);
        groups.push(i < this.predictionWordGroups.length ? this.predictionWordGroups[i] : [cur[i]]);
      }
    }
    AppStorage.setOrCreate('predictions', strip);
    this.predictionWordGroups = groups;
  }

  hideKeyboard(): void {
    this.kbController?.hide();
  }
}

// ---------- KeyboardController ----------

// 予測変換に渡す辞書由来の前方一致の件数。PredictiveConversion 側は学習・
// 手書き表を先に詰めてから残り枠にこれを入れるので、実際に表に出るのは
// MAX_COMPLETIONS まで。多めに渡して選ばせる。
const PREDICT_DICT_HITS = 8;

export class KeyboardController {
  private panel: inputMethodEngine.Panel | undefined;
  private inputHandle: InputHandler = InputHandler.getInstance();
  private kbDelegate: inputMethodEngine.KeyboardDelegate | undefined;
  // setFloatingMode がバックグラウンド (キーボードを表示していない状態) で
  // の設定同期のときに panel.show() を誤って呼ばないための、パネルの
  // 表示状態の自前追跡。Panel 自体に同期的な可視状態の取得手段が無いため。
  private panelVisible: boolean = false;
  // setFloatingMode が固定⇔フローティング切替のために自分で呼ぶ
  // panel.hide() を、OS が「入力欄を離れた」と誤解して inputStop を
  // 発火させたときに、その後始末 (inputClient破棄・composing状態クリア)
  // を1回だけ無視するためのフラグ。
  private suppressNextInputStop: boolean = false;

  private context: InputMethodExtensionContext;

  constructor(context: InputMethodExtensionContext) {
    this.context = context;
  }

  onCreate(): void {
    // The Japanese font is registered by KeyboardPage.aboutToAppear, not here:
    // initPanel/setUiContent hasn't run yet, so there is no UI instance to
    // register into (see components/JpFont.ets).
    this.inputHandle.setContext(this.context);
    this.inputHandle.setKeyboardControllerRef(this);
    this.applyColorMode();
    this.initPanel();
    this.registerListeners();
  }

  // Re-resolves the live OS color mode via resourceManager (see
  // isDarkFromResourceManager's comment for why this, not context.config,
  // is what must be called anywhere isDark needs to be current -- including
  // repeatedly, from inputStart, not just once at onCreate).
  applyColorMode(): void {
    const mode = AppStorage.get<string>('themeMode') ?? 'system';
    AppStorage.setOrCreate('isDark', resolveIsDark(mode, isDarkFromResourceManager(this.context as ESObject)));
  }

  // Applies the Configuration object handed directly to onConfigurationUpdate
  // (InputMethodExtAbility's own onConfigurationUpdate calls this), on the
  // rare OS versions/timings where that callback does fire for this
  // extension. isDarkFromConfiguration's comment covers why the object
  // handed to the callback (not a context.config re-read) is what's safe to
  // trust here.
  applyColorModeFrom(config: ESObject): void {
    const mode = AppStorage.get<string>('themeMode') ?? 'system';
    AppStorage.setOrCreate('isDark', resolveIsDark(mode, isDarkFromConfiguration(config)));
  }

  onDestroy(): void {
    // 予約中の学習・削除候補の保存を確定させてから畳む。
    this.inputHandle.flushPendingSaves();
    InputLog.flush();
    this.unregisterListeners();
    if (this.panel) {
      IMEAbility.destroyPanel(this.panel);
      this.panel = undefined;
    }
  }

  // フローティングモードの最小サイズ。これより小さいと掴みにくく／候補や
  // キーが読めなくなる。
  private static readonly FLOATING_MIN_W_VP = 220;
  private static readonly FLOATING_MIN_H_VP = 160;
  // 実際のOSパネルウィンドウをカード本体より一回り大きく取っておく余白
  // (vp)。KeyboardPage.ets の FLOATING_MARGIN_VP と同じ値 -- 四隅の
  // L字ハンドルや影がカードの外側へはみ出す分の描画領域を確保する
  // (「Lのハンドル見切れてる」の実体は、窓そのものがカードぴったりの
  // サイズしか無く、はみ出す分がOSにクリップされていたこと)。
  private static readonly FLOATING_MARGIN_VP = 16;
  // カード上端(=掴んで動かすためのピル型ハンドル/L字コーナーハンドル)が
  // 画面最上端すれすれまで行けてしまうと、その帯がステータスバーや端末の
  // エッジジェスチャー領域(通知パネルを引き出す操作等)と重なり、OS 側に
  // タッチを横取りされてハンドルへ指が全く届かなくなる。「フローティング
  // を上辺に持っていくと詰む」の実体はこれ -- 一度そこへ動かすと、動かす
  // ためのハンドル自体に触れなくなり、二度と引きずり戻せなくなる。
  // 単なる窓の余白 (FLOATING_MARGIN_VP) より広い専用の下限を設け、
  // ハンドルが必ずその手前の安全な帯に残るようにする。
  private static readonly FLOATING_TOP_SAFE_VP = 40;

  // ドラッグ/リサイズ中だけ display.getDefaultDisplaySync() の結果を固定する
  // ためのキャッシュ。「固定できる2点間で反復横跳びする」不具合の原因:
  // フローティングパネル自身のリサイズが display の報告値 (使用可能領域)
  // にわずかに影響し、それを毎回読み直して次の矩形を計算していたため、
  // 計算→リサイズ→display値が変わる→別の計算結果→リサイズ…という
  // フィードバックループで2値の間を往復していたとみられる。ドラッグ開始時
  // に1回だけ読み、そのドラッグが終わるまでは同じ値を使い回すことで
  // ループの入力そのものを固定する。
  private draggingFloating: boolean = false;
  private cachedDis: display.Display | undefined = undefined;
  private cachedDensity: number = 3.0;

  beginFloatingDrag(): void {
    try {
      this.cachedDis = display.getDefaultDisplaySync();
      this.cachedDensity = this.cachedDis.densityDPI > 0 ? this.cachedDis.densityDPI / 160 : 3.0;
      this.draggingFloating = true;
    } catch (err) {
      console.error('shuntiIME beginFloatingDrag error', err);
    }
  }

  endFloatingDrag(): void {
    this.draggingFloating = false;
    this.cachedDis = undefined;
  }

  // ドラッグ中はキャッシュ、そうでなければ毎回読み直す (回転・折りたたみは
  // 素早く反映したいので、ドラッグしていないときまで固定はしない)。
  private displayForFloating(): DisplayInfo {
    if (this.draggingFloating && this.cachedDis !== undefined) {
      return { dis: this.cachedDis, density: this.cachedDensity };
    }
    const dis = display.getDefaultDisplaySync();
    const density = dis.densityDPI > 0 ? dis.densityDPI / 160 : 3.0;
    return { dis: dis, density: density };
  }

  // フローティングの位置・大きさは画面に対する割合 (0〜1) で
  // AppStorage/設定に持っている (回転・折りたたみで画面サイズ自体が
  // 変わるため、絶対 vp では意味が無くなる)。呼ばれた時点の画面サイズで
  // 実際の vp 矩形に変換し、画面からはみ出さないようクランプする。
  private floatingRectVp(dis: display.Display, density: number): FloatingRect {
    const screenWVp = dis.width / density;
    const screenHVp = dis.height / density;
    const m = KeyboardController.FLOATING_MARGIN_VP;
    const xFrac = AppStorage.get<number>('floatingXFrac') ?? 0.2;
    const yFrac = AppStorage.get<number>('floatingYFrac') ?? 0.12;
    const wFrac = AppStorage.get<number>('floatingWFrac') ?? 0.6;
    const hFrac = AppStorage.get<number>('floatingHFrac') ?? 0.35;
    // 上下左右に余白ぶんを残す (窓 = カード + 2*余白 が画面からはみ出さない
    // ようにするため、カード自体の上限は画面幅から余白の2倍を引いたもの)。
    const w = Math.max(KeyboardController.FLOATING_MIN_W_VP,
      Math.min(screenWVp - 2 * m, wFrac * screenWVp));
    const h = Math.max(KeyboardController.FLOATING_MIN_H_VP,
      Math.min(screenHVp - 2 * m, hFrac * screenHVp));
    const x = Math.max(m, Math.min(screenWVp - w - m, xFrac * screenWVp));
    // ドラッグ中は下端のクランプを緩め、ドック復帰が確定する位置の少し
    // 手前まで実際に画面外へはみ出して見えるようにする(「UIでも下部は
    // み出ししてほしい」)。ドラッグ中でなければ (確定した値) 従来どおり
    // 画面内に収める。
    const yMax = this.draggingFloating
      ? screenHVp - h * (1 - KeyboardController.FLOATING_DOCK_BACK_FRACTION)
      : screenHVp - h - m;
    const y = Math.max(KeyboardController.FLOATING_TOP_SAFE_VP, Math.min(yMax, yFrac * screenHVp));
    return { x, y, w, h };
  }

  private computeKeyWidths(dis: display.Display): void {
    const safeDPI = dis.densityDPI > 0 ? dis.densityDPI : 460;
    const density = safeDPI / 160;
    const fullVp = dis.width / density;
    // On foldables/tablets the unfolded inner screen is very wide; stretching
    // 10 keys across it makes each key huge and hard to reach. Cap the keyboard
    // content width and let the rows center themselves (justifyContent.Center),
    // matching how phone-style keyboards float centered on large displays.
    // Detect tablet/foldable class by the device's SHORTER physical dimension,
    // not by whichever dimension is currently "width" — a normal phone rotated
    // to landscape also reports a large dis.width, but its narrow dimension
    // (the portrait width) is still phone-sized, so it must NOT be capped like
    // a tablet. Without this, every phone in landscape rendered a narrow,
    // centered keyboard with large empty margins on both sides (looked tiny,
    // unlike Gboard which fills the full landscape width).
    const shortVp = Math.min(dis.width, dis.height) / density;
    const isTabletClass = shortVp > 600;
    AppStorage.setOrCreate('isTabletClass', isTabletClass);

    // This is only a best-effort guess from raw display metrics — on-device
    // diagnostics confirmed this device's IME panel framework silently shrinks
    // the panel to a fraction of the width requested via panel.resize() (~76%
    // observed, with no corresponding cutout reported — a software policy, not
    // a hardware inset), so display metrics alone can't be trusted.
    // KeyboardPage.onAreaChange measures the real width and both applies the
    // correction AND records a scale factor (measured/guessed) here, per
    // orientation. Apply that learned scale up front: after the very first
    // launch, this panel is reused across show/hide (see the 'inputStop'
    // handler), so initPanel() never runs again and a later resizePanel() call
    // (e.g. from a display 'change' event that doesn't actually alter the
    // panel's on-screen size) may not be followed by a fresh onAreaChange —
    // without this, that would silently overwrite the corrected widths with
    // the raw (wrong) guess again, with nothing left to re-correct them.
    // Tablet-class devices are excluded: their narrower width there is our own
    // deliberate MAX_KB_VP cap, not the shrink bug, so it shouldn't be learned
    // as a scale factor.
    const isLandscape = dis.width > dis.height;
    const scaleKey = isLandscape ? 'widthScaleLandscape' : 'widthScalePortrait';
    const learnedScale = isTabletClass ? 1 : (AppStorage.get<number>(scaleKey) ?? 1);
    AppStorage.setOrCreate('lastRawFullVp', fullVp);
    AppStorage.setOrCreate('lastRawFullVpIsLandscape', isLandscape);
    applyKeyboardWidthsForVp(fullVp * learnedScale, isTabletClass, isLandscape);
  }

  // densityDPI/160 gives the px-per-vp ratio (e.g. 460dpi/160 = 2.875 on Pura 70)
  private computeKeyHeight(dis: display.Display, isLandscape: boolean): number {
    const density = dis.densityDPI > 0 ? dis.densityDPI / 160 : 3.0;
    // Landscape screens are short, so the keyboard needs a larger share of that
    // short dimension to stay comfortably tappable (Gboard-style) rather than
    // shrinking rows down — portrait: clamp 25–60%; landscape: allow up to 80%.
    const contentPx = Math.ceil((isLandscape ? 420 : 360) * density);
    const maxFrac = isLandscape ? 0.80 : 0.60;
    const base = Math.max(
      Math.floor(dis.height * 0.25),
      Math.min(contentPx, Math.floor(dis.height * maxFrac))
    );
    // 設定 → レイアウト調整 の縦の長さハンドル。既定 1.0 で今までどおり。
    // 上限は向きによらず +30% (それ以上は画面の大半を覆ってしまう)。
    // 下限は向きで分けてある -- 横向きは maxFrac/contentPx が縦向きより
    // 大きく (80%・420px相当) 既定の背が高くなりがちなので、縮める側の
    // 余地をもっと残す必要があった (実際の指摘: 「横向き時、70%より
    // 小さくできた方が良い」)。縦向きは 70% でもキーの当たり判定が
    // 厳しくなり始めるので、下限はそのまま。LayoutAdjustBar のドラッグ
    // 範囲もこれと同じ値を使っている (isLandscape で分岐)。
    const minScale = isLandscape ? 0.4 : 0.7;
    const scale = Math.min(1.3, Math.max(minScale, AppStorage.get<number>(
      isLandscape ? 'keyboardHeightScaleLandscape' : 'keyboardHeightScalePortrait') ?? 1.0));
    return Math.floor(base * scale);
  }

  private initPanel(): void {
    try {
      const dis = display.getDefaultDisplaySync();
      const isLandscape = dis.width > dis.height;
      const keyHeight = this.computeKeyHeight(dis, isLandscape);

      AppStorage.setOrCreate('isLandscape', isLandscape);
      AppStorage.setOrCreate('keyboardHeight', keyHeight);
      // 'keyboardHeight' は panel.resize() に渡す都合で px のまま。ArkUI側
      // (候補バー等の比率計算) が使うのは vp なので別キーで持つ
      // (KeyboardPage.ets の keyboardHeightVp 参照)。
      const initDensity = dis.densityDPI > 0 ? dis.densityDPI / 160 : 3.0;
      AppStorage.setOrCreate('keyboardHeightVp', keyHeight / initDensity);
      this.computeKeyWidths(dis);

      const panelInfo: inputMethodEngine.PanelInfo = {
        type: inputMethodEngine.PanelType.SOFT_KEYBOARD,
        flag: inputMethodEngine.PanelFlag.FLG_FIXED,
      };

      IMEAbility.createPanel(this.context, panelInfo)
        .then((panel: inputMethodEngine.Panel) => {
          this.panel = panel;
          panel.resize(dis.width, keyHeight)
            .then(() => panel.moveTo(0, dis.height - keyHeight))
            .then(() => panel.setUiContent('pages/KeyboardPage'))
            .catch((err: Error) => console.error('shuntiIME panel init error', err));
        })
        .catch((err: Error) => console.error('shuntiIME createPanel error', err));
    } catch (err) {
      console.error('shuntiIME initPanel error', err);
    }
  }

  // resize() → moveTo() の間、パネルは「新しい高さ・古い位置」の状態を
  // 一瞬とる (このAPIは1回で両方を変える手段が無い)。ドラッグ中に連打すると
  // 前の組が終わる前に次を投げてしまい、複数組が競合して下辺が「プルプル」
  // 揺れる (レイアウト調整の縦ハンドルで実際に発生)。
  //
  // 1組が完了するまで次を間引く、という発想自体は正しいが、"完了" を
  // panel.resize()/moveTo() のPromiseで判定する版は実機で別の問題を
  // 起こした: この環境ではその2つのPromiseが確実に解決/拒否するとは
  // 限らないらしく (getTextIndexAtCursorの値が信用できない、insertText
  // 直後のmoveCursorが無視される等、このコードベースの他のハマりどころと
  // 同じ傾向)、1回でも解決が来ないと「保留中」のまま永久に固まり、
  // レイアウト調整が丸ごと反応しなくなった。
  //
  // そこで "完了" の判定をネイティブ側のPromiseに頼らず、setTimeoutの
  // 固定時間 (RESIZE_BUSY_MS) だけで決める。ネイティブ呼び出しが実際に
  // 何秒かかろうと、この時間が来れば必ず「次を投げてよい」に戻るので、
  // 前回のように永久に詰まることが原理的に起きない。
  private static readonly RESIZE_BUSY_MS = 80;
  private resizeBusyUntil: number = 0;
  private resizePendingRequest: boolean = false;
  // フローティングの四隅リサイズは resize() と moveTo() の非原子的な2段階が
  // FIXED時のドラッグより目立ちやすい (掴んでいない角を固定するために毎回
  // 両方叩くことが多いため)。直前に適用した位置と変わっていなければ
  // moveTo() 自体を呼ばない -- 二段階のうち片方だけでも減らすと体感の
  // 「ピクピク」がかなり収まる。
  private lastFloatingXPx: number = Number.NaN;
  private lastFloatingYPx: number = Number.NaN;
  // 同じ考え方で resize() も、大きさが変わっていないとき (上際ハンドルの
  // 純粋な移動ドラッグなど) は呼ばない。動かすだけなのに毎回 resize() まで
  // 叩いていたのがカクつきの一因だった。
  private lastFloatingWPx: number = Number.NaN;
  private lastFloatingHPx: number = Number.NaN;

  // 設定 → レイアウト調整 のハンドルからライブに呼ばれるので公開している
  // (InputHandler.applyKeyboardHeightScaleLive 参照)。パネルの実サイズは
  // panel.resize() でしか変えられず、それを持っているのはこのクラスだけ。
  resizePanel(): void {
    const now = Date.now();
    if (now < this.resizeBusyUntil) {
      this.resizePendingRequest = true;
      return;
    }
    this.doResizePanelNow();
  }

  private doResizePanelNow(): void {
    const floating = AppStorage.get<boolean>('floatingMode') ?? false;
    // 「反復横跳び」の原因はジッタではなく display 値のフィードバック
    // ループだったので (displayForFloating のキャッシュで解決済み)、間引き
    // 自体はFIXED時と揃えてよい。動きの滑らかさ優先で少し短くしてある。
    this.resizeBusyUntil = Date.now() + (floating ? Math.floor(KeyboardController.RESIZE_BUSY_MS / 2) : KeyboardController.RESIZE_BUSY_MS);
    try {
      const dis = this.displayForFloating().dis;
      const isLandscape = dis.width > dis.height;

      AppStorage.setOrCreate('isLandscape', isLandscape);

      if (floating) {
        // フローティング中は画面全幅にも下端固定にもしない。位置・大きさは
        // floatingRectVp が割合から実 vp へ変換したものをそのまま使う
        // (タブレット幅キャップ・「実機は resize 要求を約76%に縮める」
        // 学習係数は、いずれも画面いっぱいに広げるFIXED運用時だけの話なので
        // ここでは適用しない)。
        const density = dis.densityDPI > 0 ? dis.densityDPI / 160 : 3.0;
        const rect = this.floatingRectVp(dis, density);
        AppStorage.setOrCreate('keyboardHeight', rect.h);
        AppStorage.setOrCreate('keyboardHeightVp', rect.h);
        applyKeyboardWidthsForVp(rect.w, false, isLandscape);
        // OSパネルウィンドウ自体はカード(rect)より FLOATING_MARGIN_VP ぶん
        // 一回り大きく取る。四隅ハンドルや影がその余白へはみ出して描ける
        // ようにするため (KeyboardPage.ets 側もカードをこの余白ぶん内側に
        // 絶対配置している -- 数値は両方で揃えてあること)。
        const m = KeyboardController.FLOATING_MARGIN_VP;
        const wPx = Math.round((rect.w + 2 * m) * density);
        const hPx = Math.round((rect.h + 2 * m) * density);
        const xPx = Math.round((rect.x - m) * density);
        const yPx = Math.round((rect.y - m) * density);
        const needsMove = xPx !== this.lastFloatingXPx || yPx !== this.lastFloatingYPx;
        const needsResize = wPx !== this.lastFloatingWPx || hPx !== this.lastFloatingHPx;
        this.lastFloatingXPx = xPx;
        this.lastFloatingYPx = yPx;
        this.lastFloatingWPx = wPx;
        this.lastFloatingHPx = hPx;
        // 変わった軸だけ叩く。上際ハンドルの純粋な移動ドラッグでは大きさが
        // 一切変わらないので resize() 自体を省き moveTo() だけにする (逆に
        // 右下角のリサイズでは位置が変わらないので moveTo() を省く) --
        // どちらの場合も本来不要なネイティブ呼び出しを1回減らせる。
        //
        // 一度 adjustPanelRect(FLG_FLOATING, ...) に置き換えてみたが、
        // フローティング→固定へ戻したときに右側のキーが反応しなくなる
        // 不具合が出た (おそらく adjustPanelRect が設定した入力受付領域が
        // FIXED 側の changeFlag/resize だけでは完全にリセットされない)。
        // 実機での副作用が読み切れないため resize()/moveTo() に戻す。
        // 「フローティング中もアプリの入力欄が押し上げられて空白ができる」
        // 件は別の手段で改めて検討する。
        if (needsResize && needsMove) {
          this.panel?.resize(wPx, hPx)
            .then(() => this.panel?.moveTo(xPx, yPx))
            .catch((err: Error) => console.error('shuntiIME resizePanel(floating) resize/moveTo error', err));
        } else if (needsResize) {
          this.panel?.resize(wPx, hPx)
            .catch((err: Error) => console.error('shuntiIME resizePanel(floating) resize error', err));
        } else if (needsMove) {
          this.panel?.moveTo(xPx, yPx)
            .catch((err: Error) => console.error('shuntiIME resizePanel(floating) move error', err));
        }
      } else {
        const keyHeight = this.computeKeyHeight(dis, isLandscape);
        AppStorage.setOrCreate('keyboardHeight', keyHeight);
        const fixedDensity = dis.densityDPI > 0 ? dis.densityDPI / 160 : 3.0;
        AppStorage.setOrCreate('keyboardHeightVp', keyHeight / fixedDensity);
        this.computeKeyWidths(dis);

        this.panel?.resize(dis.width, keyHeight)
          .then(() => this.panel?.moveTo(0, dis.height - keyHeight))
          .catch((err: Error) => console.error('shuntiIME resizePanel resize/moveTo error', err));
      }
    } catch (err) {
      console.error('shuntiIME resizePanel error', err);
    }
    // busy期間が明けた時点で、その間に来ていた要求があれば最新の状態で
    // 1回だけやり直す。resizePanel() 自身が毎回 computeKeyHeight を
    // AppStorage の最新値から作るので、間引かれた個々の値ではなく
    // 「その時点でのライブな値」がここで反映される。
    setTimeout(() => {
      if (this.resizePendingRequest) {
        this.resizePendingRequest = false;
        this.doResizePanelNow();
      }
    }, floating ? Math.floor(KeyboardController.RESIZE_BUSY_MS / 2) : KeyboardController.RESIZE_BUSY_MS);
  }

  // 固定⇔フローティングの切替。Panel.changeFlag のAPI仕様どおり
  // 「表示中は変えられない → 隠す → flag変更 → 新しい位置/大きさにする →
  // 表示し直す」の順で行う。設定同期などキーボードを表示していない状態で
  // 呼ばれることもあるため、隠す前に表示中だったかを覚えておいて
  // (panelVisible) 最後に表示するかどうかを判断する — 非表示のときに
  // うっかり呼び出してキーボードを勝手に出現させないため。
  setFloatingMode(enabled: boolean): void {
    if (!this.panel) { return; }
    const wasVisible = this.panelVisible;
    AppStorage.setOrCreate('floatingMode', enabled);
    // モード切替の直後は必ず1回 resize()/moveTo() を通す (直前の記憶値は
    // 別モードのものなので使い物にならない)。
    this.lastFloatingXPx = Number.NaN;
    this.lastFloatingYPx = Number.NaN;
    this.lastFloatingWPx = Number.NaN;
    this.lastFloatingHPx = Number.NaN;
    // hide() は Promise を返すが、resizePanel と同じ理由 (このコードベースの
    // 他の箇所で確認済みの「実機ではPromiseが確実には解決しない」問題) で
    // then/catch だけに頼ると永久に次へ進めなくなるおそれがある。
    // 解決/拒否が来ればすぐ、来なくても短い固定時間で必ず1回だけ次へ進む。
    let proceeded = false;
    const proceed = (): void => {
      if (proceeded) { return; }
      proceeded = true;
      try {
        this.panel?.changeFlag(enabled
          ? inputMethodEngine.PanelFlag.FLG_FLOATING
          : inputMethodEngine.PanelFlag.FLG_FIXED);
      } catch (err) {
        console.error('shuntiIME setFloatingMode changeFlag error', err);
      }
      // changeFlag() は戻り値が void で完了を知る手段が無いが、実機では
      // 直後に resize/moveTo/show を叩くと ImsaKit 側が
      // 「ShowPanel,channel is nullptr!」を返すことを確認済み -- ネイティブ側の
      // ウィンドウ切替がまだ終わっていないところに割り込んでいたと見られる。
      // resize/moveTo 自身の「Promiseが確実には解決しない」問題と同じ理由で
      // イベント待ちにはできないので、短い固定時間だけ空けてから進む。
      setTimeout(() => {
        this.doResizePanelNow();
        if (wasVisible) {
          try {
            this.panel?.show();
          } catch (err) {
            console.error('shuntiIME setFloatingMode show error', err);
          }
        }
      }, 100);
    };
    // 「フローティング直前の高さぶんの空白ができる」対策として、changeFlag
    // の前後で高さをほぼ0へ潰す resize() を複数回叩く版を試したが、この
    // 端末では resize()/moveTo() の Promise が確実な順序で解決しないため、
    // 後から発行した「本来のフローティングサイズへのリサイズ」より先に
    // 潰す側の呼び出しが遅れて完了し、フローティングパネルが行方不明に
    // なる (「どっか飛んで行って戻ってこなくなる」) regression を引き
    // 起こした。空白問題より実害が大きいため撤回する。空白問題は改めて
    // 別の(複数リクエストを同時に飛ばさない)安全なやり方を検討すること。
    // 表示中でなければ hide() は実質なにもしない (何かを隠す必要が無い) の
    // で、inputStop を誘発する余地も無い -- 無条件でフラグを立てると
    // inputStop が結局来なかった場合にフラグが残り続け、次の本当の
    // (無関係な) inputStop を誤って握りつぶしてしまう。表示中だった
    // ときだけ立て、かつ安全弁として少し待っても来なければ自分で下ろす。
    if (wasVisible) {
      this.suppressNextInputStop = true;
      setTimeout(() => { this.suppressNextInputStop = false; }, 500);
    }
    try {
      this.panel.hide().then(proceed).catch(proceed);
    } catch (err) {
      console.error('shuntiIME setFloatingMode hide error', err);
      proceed();
    }
    setTimeout(proceed, 150);
  }

  // フローティング中のドラッグ/リサイズハンドルからライブに呼ばれる。
  // 渡された割合を画面からはみ出さないようクランプしてから保存し、
  // resizePanel (setTimeoutベースの間引き。「実機では resize/moveTo の
  // Promiseが確実には解決しない」対策は既存のものをそのまま使う) に渡す。
  private applyFloatingRectLive(xFrac: number, yFrac: number, wFrac: number, hFrac: number): void {
    try {
      const dispInfo = this.displayForFloating();
      const dis = dispInfo.dis;
      const density = dispInfo.density;
      const screenWVp = dis.width / density;
      const screenHVp = dis.height / density;
      const wVp = Math.max(KeyboardController.FLOATING_MIN_W_VP, Math.min(screenWVp, wFrac * screenWVp));
      const hVp = Math.max(KeyboardController.FLOATING_MIN_H_VP, Math.min(screenHVp, hFrac * screenHVp));
      const clampedW = wVp / screenWVp;
      const clampedH = hVp / screenHVp;
      const clampedX = Math.max(0, Math.min(1 - clampedW, xFrac));
      AppStorage.setOrCreate('floatingXFrac', clampedX);
      // Y は下方向をここではクランプしない (「UIでも下部はみ出ししてほしい」)。
      // ドラッグ中に画面外へ出た値をそのまま保存し、実際にどこまで描画して
      // よいかは floatingRectVp 側が draggingFloating を見て判断する。
      // ドラッグが終わったら endFloatingMoveDrag が改めて範囲内に戻すか、
      // ドック復帰を確定させるかを決める。
      AppStorage.setOrCreate('floatingYFrac', yFrac);
      AppStorage.setOrCreate('floatingWFrac', clampedW);
      AppStorage.setOrCreate('floatingHFrac', clampedH);
    } catch (err) {
      console.error('shuntiIME applyFloatingRectLive error', err);
    }
    this.resizePanel();
  }

  // 上際ハンドルのドラッグ (LayoutAdjustBar 等と同じく、ドラッグ開始時点の
  // 値 + 累積オフセットで計算する)。offsetX/Y は ArkUI ジェスチャの vp値を
  // そのまま渡してもらい、画面 vp サイズへの換算はここで最新の
  // display.getDefaultDisplaySync() を使って行う (回転を跨いでも狂わない)。
  moveFloatingPanelLive(startXFrac: number, startYFrac: number, offsetXVp: number, offsetYVp: number): void {
    try {
      const dispInfo = this.displayForFloating();
      const dis = dispInfo.dis;
      const density = dispInfo.density;
      const screenWVp = dis.width / density;
      const screenHVp = dis.height / density;
      const wFrac = AppStorage.get<number>('floatingWFrac') ?? 0.6;
      const hFrac = AppStorage.get<number>('floatingHFrac') ?? 0.35;
      this.applyFloatingRectLive(
        startXFrac + offsetXVp / screenWVp,
        startYFrac + offsetYVp / screenHVp,
        wFrac, hFrac);
    } catch (err) {
      console.error('shuntiIME moveFloatingPanelLive error', err);
    }
  }

  // 「画面下にはみ出るように動かしたら通常の状態に戻る」の判定そのもの。
  // カードのこの割合以上が画面下端からはみ出す位置まで引っ張ったら true。
  // ドラッグ中のライブプレビュー表示 (checkFloatingDockBack) と、離した
  // 瞬間の確定 (endFloatingMoveDrag) の両方から使う共通ロジック。
  // 元は 0.6 (カードの6割はみ出すまで戻らない) だったが、「下はみだし、
  // 60%だと過剰」の指摘どおりそこまで引っ張るのは大袈裟すぎたので、
  // もっと少ないはみ出しで戻るように緩めた。
  private static readonly FLOATING_DOCK_BACK_FRACTION = 0.35;

  // 0 = 画面内に完全に収まっている、1 = ドック復帰が確定する位置まで
  // 引っ張った。カードが画面下端からはみ出し始めた瞬間から連続的に増える
  // ので、KeyboardPage 側はこれをそのまま「下から這い出てくる」プレビュー
  // の高さに使える。
  private dockBackProgressRaw(startYFrac: number, offsetYVp: number): number {
    try {
      const dispInfo = this.displayForFloating();
      const dis = dispInfo.dis;
      const density = dispInfo.density;
      const screenHVp = dis.height / density;
      const hFrac = AppStorage.get<number>('floatingHFrac') ?? 0.35;
      const m = KeyboardController.FLOATING_MARGIN_VP;
      const h = Math.max(KeyboardController.FLOATING_MIN_H_VP,
        Math.min(screenHVp - 2 * m, hFrac * screenHVp));
      const rawYVp = (startYFrac + offsetYVp / screenHVp) * screenHVp;
      const onscreenBottom = screenHVp - h - m;
      const dockBackThreshold = screenHVp - h * (1 - KeyboardController.FLOATING_DOCK_BACK_FRACTION);
      if (rawYVp <= onscreenBottom || dockBackThreshold <= onscreenBottom) {
        return rawYVp > dockBackThreshold ? 1 : 0;
      }
      return Math.max(0, Math.min(1, (rawYVp - onscreenBottom) / (dockBackThreshold - onscreenBottom)));
    } catch (err) {
      console.error('shuntiIME dockBackProgressRaw error', err);
      return 0;
    }
  }

  private isPastDockBackThreshold(startYFrac: number, offsetYVp: number): boolean {
    return this.dockBackProgressRaw(startYFrac, offsetYVp) >= 1;
  }

  // ドラッグ中、いま離したらドックに戻るかを連続値 (0〜1) でライブに問い
  // 合わせる。KeyboardPage 側はこれを「下から這い出てくる」青いプレビュー
  // 帯の高さに直接使う (1 で「離すとドックに戻る」ことも同時に分かる)。
  checkFloatingDockBack(startYFrac: number, offsetYVp: number): number {
    return this.dockBackProgressRaw(startYFrac, offsetYVp);
  }

  endFloatingMoveDrag(startYFrac: number, offsetYVp: number): void {
    if (this.isPastDockBackThreshold(startYFrac, offsetYVp)) {
      // setFloatingMode (自分自身) だけだと実行時の状態は変わるが設定への
      // 保存がされないので、保存まで面倒を見る InputHandler 側の窓口を
      // 経由する (kbCtrlRef はこの自分自身を指している)。
      this.inputHandle.setFloatingModeExplicit(false);
      return;
    }
    // ドック復帰しない場合は、ドラッグ中に緩めていた下端クランプを元に
    // 戻すため、いまの (画面外かもしれない) 値を正規のクランプに通してから
    // 保存する。resizePanel は endFloatingDrag 側で既に呼ばれているので
    // ここでは AppStorage の値だけ直せば足りる。
    const xFrac = AppStorage.get<number>('floatingXFrac') ?? 0.2;
    const yFrac = AppStorage.get<number>('floatingYFrac') ?? 0.12;
    const wFrac = AppStorage.get<number>('floatingWFrac') ?? 0.6;
    const hFrac = AppStorage.get<number>('floatingHFrac') ?? 0.35;
    try {
      const dispInfo = this.displayForFloating();
      const dis = dispInfo.dis;
      const density = dispInfo.density;
      const screenHVp = dis.height / density;
      const m = KeyboardController.FLOATING_MARGIN_VP;
      const h = Math.max(KeyboardController.FLOATING_MIN_H_VP,
        Math.min(screenHVp - 2 * m, hFrac * screenHVp));
      const clampedYFrac = Math.max(KeyboardController.FLOATING_TOP_SAFE_VP,
        Math.min(screenHVp - h - m, yFrac * screenHVp)) / screenHVp;
      AppStorage.setOrCreate('floatingYFrac', clampedYFrac);
    } catch (err) {
      console.error('shuntiIME endFloatingMoveDrag clamp error', err);
    }
    this.resizePanel();
    this.inputHandle.saveFloatingRect();
  }

  // 四隅のリサイズハンドル。掴んだ角の対角(=動かない角)が画面上で固定に
  // 見えるよう、角によって x/y も一緒に動かす。
  resizeFloatingPanelLive(corner: 'tl' | 'tr' | 'bl' | 'br',
      startXFrac: number, startYFrac: number, startWFrac: number, startHFrac: number,
      offsetXVp: number, offsetYVp: number): void {
    try {
      const dispInfo = this.displayForFloating();
      const dis = dispInfo.dis;
      const density = dispInfo.density;
      const dxFrac = offsetXVp / (dis.width / density);
      const dyFrac = offsetYVp / (dis.height / density);
      let x = startXFrac, y = startYFrac, w = startWFrac, h = startHFrac;
      if (corner === 'br') {
        w = startWFrac + dxFrac; h = startHFrac + dyFrac;
      } else if (corner === 'bl') {
        w = startWFrac - dxFrac; h = startHFrac + dyFrac; x = startXFrac + dxFrac;
      } else if (corner === 'tr') {
        w = startWFrac + dxFrac; h = startHFrac - dyFrac; y = startYFrac + dyFrac;
      } else {
        w = startWFrac - dxFrac; h = startHFrac - dyFrac; x = startXFrac + dxFrac; y = startYFrac + dyFrac;
      }
      this.applyFloatingRectLive(x, y, w, h);
    } catch (err) {
      console.error('shuntiIME resizeFloatingPanelLive error', err);
    }
  }

  private registerListeners(): void {
    try {
      display.on('change', () => { this.resizePanel(); this.applyColorMode(); });
    } catch (err) {
      console.error('shuntiIME display.on error', err);
    }

    try {
      IMEAbility.on('inputStart', (
        kbCtrl: inputMethodEngine.KeyboardController,
        client: inputMethodEngine.InputClient
      ) => {
        // Re-resolve the live color mode on every keyboard invocation --
        // see applyColorMode's comment. An earlier attempt at this called
        // the old context.config-reading applyColorMode() here, which
        // didn't help (that read was just as stale as the one it was
        // replacing); applyColorMode() itself is now backed by
        // resourceManager, which actually is live.
        this.applyColorMode();
        this.inputHandle.onInputStart(kbCtrl, client);
        this.panel?.show();
        this.panelVisible = true;
      });

      IMEAbility.on('inputStop', () => {
        // フローティング切替のための panel.hide() が、OS 側には「入力欄を
        // 離れた」と解釈されて inputStop を誘発することがある (「固定
        // キーボードから引っ張り出したとき、テキスト欄の選択が消えて
        // フローティング出ない」の実体 -- onInputStop が inputClient を
        // 破棄し composing 状態を消してしまうため)。setFloatingMode 発の
        // hide() の直後だけはこの後始末をまるごと飛ばす。
        if (this.suppressNextInputStop) {
          this.suppressNextInputStop = false;
          return;
        }
        // Only hide — don't tear down the context/panel here. initPanel()
        // (which creates this.panel) only runs once, from onCreate(), so
        // destroying the context on every inputStop left no panel to show()
        // for the *next* inputStart unless the OS happened to fully restart
        // the extension in between. That's what made the keyboard vanish
        // after a couple of show/hide cycles until something (e.g. rotating
        // back to portrait and switching IME) forced a fresh restart.
        // The real teardown belongs in the ability's own onDestroy(), which
        // already calls KeyboardController.onDestroy() when the IME service
        // is actually shut down.
        this.inputHandle.onInputStop();
        this.panel?.hide();
        this.panelVisible = false;
      });

      IMEAbility.on('setSubtype', (subtype: InputMethodSubtype) => {
        const modeMap: Record<string, string> = {
          'hiragana': InputMode.HIRAGANA,
          'katakana': InputMode.KATAKANA,
          'alphanumeric': InputMode.ALPHANUMERIC,
        };
        const mode = modeMap[subtype.id];
        if (mode) {
          this.inputHandle.commitTopCandidate();
          AppStorage.setOrCreate('inputMode', mode);
        }
      });
    } catch (err) {
      console.error('shuntiIME IMEAbility.on error', err);
    }

    try {
      this.kbDelegate = inputMethodEngine.getKeyboardDelegate();
      this.kbDelegate.on('keyDown', (_keyEvent: inputMethodEngine.KeyEvent) => {
        return false;
      });
      // 入力欄のカーソルの本当の位置を取消キーの位置合わせに使う (InputHandler.onHostSelection)
      this.kbDelegate.on('selectionChange', (_ob: number, _oe: number, newBegin: number, _ne: number) => {
        InputHandler.getInstance().onHostSelection(newBegin);
      });
      // 入力欄の文字が変わった (送信ボタンで欄が空になったら、打っている途中の状態を捨てる)
      this.kbDelegate.on('textChange', (text: string) => {
        InputHandler.getInstance().onHostText(text);
      });
    } catch (err) {
      console.error('shuntiIME getKeyboardDelegate error', err);
    }
  }

  private unregisterListeners(): void {
    display.off('change');
    this.kbDelegate?.off('keyDown');
    this.kbDelegate?.off('selectionChange');
    this.kbDelegate?.off('textChange');
  }
}

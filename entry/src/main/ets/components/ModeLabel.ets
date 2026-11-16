import { InputMode } from '../ime/KeyboardController';

// あa1 切り替えキーの文字の大きさ (全パッド共通)
export const MODE_KEY_FONT_SIZE: number = 16;

// あA1 切り替えキーに出す文字の並び。順番は常に あ・a・1 で固定する。
// 数字パッドが無効のときは 1 を出さない (循環に含まれないため)。
export function modeGlyph(_inputMode: string, numericPadEnabled: boolean, i: number): string {
  const seq: string[] = numericPadEnabled ? ['あ', 'a', '1'] : ['あ', 'a'];
  return i < seq.length ? seq[i] : '';
}

// 今の入力モードの文字か (濃く出す文字)。ひらがなのときは「あ」、それ以外は「a」。
export function modeIsActive(inputMode: string, numericPadEnabled: boolean, i: number): boolean {
  const g: string = modeGlyph(inputMode, numericPadEnabled, i);
  return g.length > 0 && g === (inputMode === InputMode.HIRAGANA ? 'あ' : 'a');
}

// 9 キー (あかさ盤) の中央の文字の大きさ。キー幅と、キーボード全体の高さから求める。
// FlickKeyboardView と数字パッドの ⌫ が同じ値を使い、大きさを揃える。
export function flickGlyphSize(keyWidthVp: number, keyboardHeightVp: number): number {
  const rowH: number = Math.max(0, (keyboardHeightVp - 24) / 4);
  const base: number = Math.min(keyWidthVp, rowH);
  return Math.max(9, Math.min(24, Math.round(base / 3)));
}

// 数字パッドの中央の数字の大きさ (vp)
export const NUM_CENTER_FONT_SIZE: number = 20;        // 通常
export const NUM_CENTER_ACTIVE_FONT_SIZE: number = 22; // フリック中 (打とうとしている向き)
export const NUM_CENTER_LANDSCAPE_FONT_SIZE: number = 18; // 横向き (方向ヒントなし)

// 数字パッドとフリック盤の ⌫ (バックスペース) の文字の大きさ (vp)
export const BACKSPACE_FONT_SIZE: number = 26;

// 英字 (QWERTY) キーボードの文字の大きさ (vp)
export const QWERTY_KEY_FONT_SIZE: number = 18;      // 英字キー (A〜Z)
export const QWERTY_ICON_FONT_SIZE: number = 20;     // シフト・⌫・Enter のアイコン
export const QWERTY_CARET_FONT_SIZE: number = 16;    // ◀ ▶ のカーソルキー
export const QWERTY_SPACE_FONT_SIZE: number = 16;    // 空白・文節・変換の文字
export const QWERTY_SYMBOL_FONT_SIZE: number = 14;   // 「記号」ボタン
export const QWERTY_PUNCT_FONT_SIZE: number = 18;    // , と . のボタン

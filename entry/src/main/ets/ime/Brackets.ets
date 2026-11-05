// 括弧の全種類 (開き, 閉じ)。どの括弧も必ず打てるように、変換の候補はここから出す。
// Android 版 Brackets.kt・Windows 版 desktop/core/composer.cpp の BRACKETS も同じ並び (変えるときは 3 つとも)。
export const ALL_BRACKETS: string[][] = [
  ['（', '）'], ['(', ')'], ['「', '」'], ['『', '』'], ['【', '】'], ['［', '］'], ['[', ']'],
  ['｛', '｝'], ['{', '}'], ['〔', '〕'], ['〈', '〉'], ['《', '》'], ['〖', '〗'], ['〘', '〙'],
  ['〚', '〛'], ['｢', '｣'], ['＜', '＞'], ['<', '>'], ['«', '»'], ['‹', '›'], ['“', '”'],
  ['‘', '’'], ['〝', '〟'], ['｟', '｠'],
];

// 読みに合う括弧の候補。
//   括弧 1 字 → 同じ側 (開き・閉じ) の全種類 (打った字が先頭)
//   かっこ → 全種類の組 / かっこひらき・かっことじ → 開き・閉じの全種類
// 括弧でない読みは空。
export function bracketVariants(reading: string): string[] {
  const opens: string[] = ALL_BRACKETS.map((p: string[]) => p[0]);
  const closes: string[] = ALL_BRACKETS.map((p: string[]) => p[1]);
  if (reading === 'かっこ') { return ALL_BRACKETS.map((p: string[]) => p[0] + p[1]); }
  if (reading === 'かっこひらき') { return opens; }
  if (reading === 'かっことじ') { return closes; }
  if (opens.includes(reading)) { return [reading, ...opens.filter((c: string) => c !== reading)]; }
  if (closes.includes(reading)) { return [reading, ...closes.filter((c: string) => c !== reading)]; }
  return markVariants(reading);
}

// 記号 1 字: 全角・半角の両方と仲間の記号 (打った字が先頭)。MS-IME と同じく・で ／ /、￥ で ＼ \ (ブログの報告 2026-11-02。
// Windows 版 composer.cpp の MARKS・Android 版 Brackets.kt と同じ)。英数字は空
const MARKS: string[][] = [
  ['・', '／', '/'], ['、', '，', ','], ['。', '．', '.'], ['ー', '－', '-', '―', '‐'], ['～', '〜', '~'],
  ['￥', '＼', '\\'], ['＊', '*', '※', '×'], ['＂', '"', '“', '”'], ['＇', '\'', '‘', '’'],
];

function markVariants(reading: string): string[] {
  if (reading.length !== 1) { return []; }
  let group: string[] = [];
  for (const g of MARKS) {
    if (g.includes(reading)) { group = g; break; }
  }
  if (group.length === 0) {
    const ch = reading.charCodeAt(0);
    const half = ch >= 0xFF01 && ch <= 0xFF5E ? ch - 0xFEE0 : ch;
    const isAlnum = (half >= 0x30 && half <= 0x39) || (half >= 0x41 && half <= 0x5A) || (half >= 0x61 && half <= 0x7A);
    if (half <= 0x20 || half > 0x7E || isAlnum) { return []; }
    group = [String.fromCharCode(half + 0xFEE0), String.fromCharCode(half)];
  }
  return [reading, ...group.filter((c: string) => c !== reading)];
}

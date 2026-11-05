// HarmonyOS 版 (ArkTS) の定数の表を、Android 版の資産 (android/app/src/main/assets/hmos_tables.json) に書き出す。
// キーの配置・記号・絵文字・濁点の切り替え・色・差別語の除外などは HarmonyOS 版が正本で、Android 版はこの JSON を読むだけ。
// HarmonyOS 側で表を直したら、これを流し直せば Android 側も揃う (表を Kotlin に書き写さない)。
//
//   node tools/gen_android_tables.mjs
//
// 表は ArkTS の `const 名前: 型 = <リテラル>;` をそのまま JavaScript として評価する (表は純粋なリテラルだけなので)。
import { readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

const TABLES = {
  'components/FlickKeyboardView.ets': ['FLICK_GRID'],
  'components/AlphabetFlickKeyboardView.ets': ['ALPHA_GRID'],
  'components/NumericPadView.ets': ['NUM_FLICK_GRID'],
  'components/SymbolView.ets': ['SYM_TABS', 'DEFAULT_RECENT_SYMBOLS', 'SYMBOL_CATEGORIES'],
  'components/EmojiView.ets': ['EMOJI_TABS', 'EMOJI_CATEGORIES'],
  'components/Theme.ets': ['LIGHT_THEME', 'DARK_THEME'],
  'ime/EmojiSuggest.ets': ['EMOJI_FOR_SURFACE'],
  'ime/KanaKanjiConverter.ets': ['EMOJI_MAP', 'KAOMOJI_MAP'],
  'ime/KeyboardController.ets': ['VARIANT_CYCLE'],
  'ime/JapaneseConverter.ets': ['ROMAJI_TABLE'],
  'ime/PredictivePhrases.ets': ['PREDICTIVE_PHRASES'],
  // QWERTY の配置。「元の名前:JSON での名前」(元の名前が一般的すぎるので付け替える)
  'components/KeyboardView.ets': ['ROWS:QWERTY_ROWS', 'ROW1_ALPHANUMERIC:QWERTY_ROW1_ALPHANUMERIC', 'NUMBER_ROW:QWERTY_NUMBER_ROW'],
};

// start から、対応する閉じ括弧までの文字列 (文字列とコメントの中の括弧は数えない)
function balanced(src, start, what) {
  let depth = 0;
  for (let i = start; i < src.length; i++) {
    const c = src[i];
    if (c === "'" || c === '"' || c === '`') {
      for (i++; src[i] !== c; i++) if (src[i] === '\\') i++;
    } else if (c === '/' && src[i + 1] === '/') {
      while (src[i] !== '\n') i++;
    } else if (c === '/' && src[i + 1] === '*') {
      i = src.indexOf('*/', i) + 1;
    } else if (c === '[' || c === '{' || c === '(') {
      depth++;
    } else if (c === ']' || c === '}' || c === ')') {
      if (--depth === 0) return src.slice(start, i + 1);
    }
  }
  throw new Error(`${what} の終わりが見つからない`);
}

// `const NAME ... = <リテラル>`
function literal(src, name) {
  const m = new RegExp(`(?:^|\\n)(?:export )?const ${name}\\b[^=]*=\\s*`).exec(src);
  if (!m) throw new Error(`${name} が見つからない`);
  return balanced(src, m.index + m[0].length, name);
}

const ets = (file) => readFileSync(join(root, 'entry/src/main/ets', file), 'utf8');
const out = {};
for (const [file, names] of Object.entries(TABLES)) {
  const src = ets(file);
  for (const spec of names) {
    const [name, as = name] = spec.split(':');
    out[as] = (0, eval)(`(${literal(src, name)})`);
  }
}
// 語をつないで差別語になった候補を出さないための正規表現 (AiConverter.SLURS の `new RegExp(...)` の中身)
{
  const src = ets('ime/AiConverter.ets');
  const m = /SLURS: RegExp = new RegExp/.exec(src);
  if (!m) throw new Error('AiConverter.SLURS が見つからない');
  out.SLURS = (0, eval)(balanced(src, m.index + m[0].length, 'SLURS'));
}
const dst = join(root, 'android/app/src/main/assets/hmos_tables.json');
writeFileSync(dst, JSON.stringify(out));

// ライセンス表記 (android/app/src/main/assets/licenses.txt) は手で書いている (フォント・SCOWL・CC BY-SA などの節が増えたため)。
// ここで作り直すと、それらが消える。直すときは desktop/windows/LICENSE.txt と同じ内容にそろえる
for (const [k, v] of Object.entries(out)) console.log(k, typeof v === 'string' ? `${v.length} 字` : Array.isArray(v) ? v.length : Object.keys(v).length);

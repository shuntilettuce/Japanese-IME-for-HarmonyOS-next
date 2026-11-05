export interface ConversionResult {
  committed: string;
  pending: string;
}

const CONSONANTS = 'bcdfghjklmnpqrstvwxyz';

export const ROMAJI_TABLE: Record<string, string> = {
  // Vowels
  'a': 'あ', 'i': 'い', 'u': 'う', 'e': 'え', 'o': 'お',

  // Long vowel mark — the "-" key always inserts ー directly in kana mode
  // (koohii is never typed as こおひい; こーひー comes from ko-hi-).
  '-': 'ー',

  // K row
  'ka': 'か', 'ki': 'き', 'ku': 'く', 'ke': 'け', 'ko': 'こ',
  'kya': 'きゃ', 'kyi': 'きぃ', 'kyu': 'きゅ', 'kye': 'きぇ', 'kyo': 'きょ',

  // S row
  'sa': 'さ', 'si': 'し', 'su': 'す', 'se': 'せ', 'so': 'そ',
  'shi': 'し', 'sha': 'しゃ', 'shu': 'しゅ', 'she': 'しぇ', 'sho': 'しょ',
  'sya': 'しゃ', 'syi': 'しぃ', 'syu': 'しゅ', 'sye': 'しぇ', 'syo': 'しょ',

  // T row
  'ta': 'た', 'ti': 'ち', 'tu': 'つ', 'te': 'て', 'to': 'と',
  'chi': 'ち', 'cha': 'ちゃ', 'chu': 'ちゅ', 'che': 'ちぇ', 'cho': 'ちょ',
  'tsu': 'つ',
  'tya': 'ちゃ', 'tyi': 'ちぃ', 'tyu': 'ちゅ', 'tye': 'ちぇ', 'tyo': 'ちょ',
  'thi': 'てぃ', 'tha': 'てゃ', 'thu': 'てゅ', 'the': 'てぇ', 'tho': 'てょ',
  'twu': 'とぅ',

  // N row
  'na': 'な', 'ni': 'に', 'nu': 'ぬ', 'ne': 'ね', 'no': 'の',
  'nya': 'にゃ', 'nyi': 'にぃ', 'nyu': 'にゅ', 'nye': 'にぇ', 'nyo': 'にょ',
  'nn': 'ん',

  // H row
  'ha': 'は', 'hi': 'ひ', 'hu': 'ふ', 'he': 'へ', 'ho': 'ほ',
  'fu': 'ふ', 'fa': 'ふぁ', 'fi': 'ふぃ', 'fe': 'ふぇ', 'fo': 'ふぉ',
  'hya': 'ひゃ', 'hyi': 'ひぃ', 'hyu': 'ひゅ', 'hye': 'ひぇ', 'hyo': 'ひょ',

  // M row
  'ma': 'ま', 'mi': 'み', 'mu': 'む', 'me': 'め', 'mo': 'も',
  'mya': 'みゃ', 'myi': 'みぃ', 'myu': 'みゅ', 'mye': 'みぇ', 'myo': 'みょ',

  // Y row
  'ya': 'や', 'yi': 'い', 'yu': 'ゆ', 'ye': 'いぇ', 'yo': 'よ',

  // R row
  'ra': 'ら', 'ri': 'り', 'ru': 'る', 're': 'れ', 'ro': 'ろ',
  'rya': 'りゃ', 'ryi': 'りぃ', 'ryu': 'りゅ', 'rye': 'りぇ', 'ryo': 'りょ',

  // W row
  'wa': 'わ', 'wi': 'うぃ', 'we': 'うぇ', 'wo': 'を',

  // G row
  'ga': 'が', 'gi': 'ぎ', 'gu': 'ぐ', 'ge': 'げ', 'go': 'ご',
  'gya': 'ぎゃ', 'gyi': 'ぎぃ', 'gyu': 'ぎゅ', 'gye': 'ぎぇ', 'gyo': 'ぎょ',

  // Z row
  'za': 'ざ', 'zi': 'じ', 'zu': 'ず', 'ze': 'ぜ', 'zo': 'ぞ',
  'ji': 'じ', 'ja': 'じゃ', 'ju': 'じゅ', 'je': 'じぇ', 'jo': 'じょ',
  'jya': 'じゃ', 'jyi': 'じぃ', 'jyu': 'じゅ', 'jye': 'じぇ', 'jyo': 'じょ',
  'zya': 'じゃ', 'zyi': 'じぃ', 'zyu': 'じゅ', 'zye': 'じぇ', 'zyo': 'じょ',

  // D row
  'da': 'だ', 'di': 'ぢ', 'du': 'づ', 'de': 'で', 'do': 'ど',
  'dya': 'ぢゃ', 'dyi': 'ぢぃ', 'dyu': 'ぢゅ', 'dye': 'ぢぇ', 'dyo': 'ぢょ',
  'dhi': 'でぃ', 'dha': 'でゃ', 'dhu': 'でゅ', 'dhe': 'でぇ', 'dho': 'でょ',
  'dwu': 'どぅ',

  // B row
  'ba': 'ば', 'bi': 'び', 'bu': 'ぶ', 'be': 'べ', 'bo': 'ぼ',
  'bya': 'びゃ', 'byi': 'びぃ', 'byu': 'びゅ', 'bye': 'びぇ', 'byo': 'びょ',

  // P row
  'pa': 'ぱ', 'pi': 'ぴ', 'pu': 'ぷ', 'pe': 'ぺ', 'po': 'ぽ',
  'pya': 'ぴゃ', 'pyi': 'ぴぃ', 'pyu': 'ぴゅ', 'pye': 'ぴぇ', 'pyo': 'ぴょ',

  // V row
  'va': 'ゔぁ', 'vi': 'ゔぃ', 'vu': 'ゔ', 've': 'ゔぇ', 'vo': 'ゔぉ',

  // Small kana (x-prefix)
  'xa': 'ぁ', 'xi': 'ぃ', 'xu': 'ぅ', 'xe': 'ぇ', 'xo': 'ぉ',
  'xya': 'ゃ', 'xyu': 'ゅ', 'xyo': 'ょ', 'xtu': 'っ', 'xtsu': 'っ', 'xwa': 'ゎ',

  // Small kana (l-prefix)
  'la': 'ぁ', 'li': 'ぃ', 'lu': 'ぅ', 'le': 'ぇ', 'lo': 'ぉ',
  'lya': 'ゃ', 'lyu': 'ゅ', 'lyo': 'ょ', 'ltu': 'っ', 'ltsu': 'っ',
};

// Pre-compute prefix set for O(1) prefix lookups
const PREFIX_SET = new Set<string>();
for (const key of Object.keys(ROMAJI_TABLE)) {
  for (let i = 1; i <= key.length; i++) {
    PREFIX_SET.add(key.slice(0, i));
  }
}
// Add 'n' as it's handled separately
PREFIX_SET.add('n');

export function toKatakana(hiragana: string): string {
  return hiragana.replace(/[ぁ-ゖ]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 0x60));
}

// 全角カタカナ -> 半角カタカナ (濁点・半濁点は 2 字に)。半角にできない字はそのまま
const HALF_BASE = 'ｧｱｨｲｩｳｪｴｫｵｶｶｷｷｸｸｹｹｺｺｻｻｼｼｽｽｾｾｿｿﾀﾀﾁﾁｯﾂﾂﾃﾃﾄﾄﾅﾆﾇﾈﾉﾊﾊﾊﾋﾋﾋﾌﾌﾌﾍﾍﾍﾎﾎﾎﾏﾐﾑﾒﾓｬﾔｭﾕｮﾖﾗﾘﾙﾚﾛﾜﾜｲｴｦﾝｳｶｹ';
const HALF_MARK = '00000000000101010101010101010101010010101000000120120120120120000000000000000000000100';

export function toHalfKatakana(katakana: string): string {
  let out = '';
  for (const c of katakana) {
    const i = c.charCodeAt(0) - 0x30A1;   // ァ (U+30A1) から ヶ (U+30F6)
    if (i >= 0 && i < HALF_BASE.length) {
      const mark = HALF_MARK.charAt(i);
      out += HALF_BASE.charAt(i) + (mark === '1' ? 'ﾞ' : mark === '2' ? 'ﾟ' : '');
    } else if (c === 'ー') {
      out += 'ｰ';
    } else if (c === '・') {
      out += '･';
    } else {
      out += c;
    }
  }
  return out;
}

export class JapaneseConverter {
  private buffer: string = '';

  reset(): void {
    this.buffer = '';
  }

  backspace(): void {
    if (this.buffer.length > 0) {
      this.buffer = this.buffer.slice(0, -1);
    }
  }

  flushPending(): string {
    const pending = this.buffer === 'n' ? 'ん' : this.buffer;   // PC と同じ。語末の n は ん
    this.buffer = '';
    return pending;
  }

  processKey(key: string): ConversionResult {
    const k = key.toLowerCase();
    this.buffer += k;

    // Double consonant → っ (not nn, not vowels)
    if (this.buffer.length >= 2) {
      const c0 = this.buffer[0];
      const c1 = this.buffer[1];
      if (c0 === c1 && c0 !== 'n' && CONSONANTS.includes(c0)) {
        // emit っ, restart buffer from second consonant onward
        const rest = this.buffer.slice(1);
        this.buffer = rest;
        const sub = this.processBuffered();
        return { committed: 'っ' + sub.committed, pending: sub.pending };
      }
    }

    return this.processBuffered();
  }

  private processBuffered(): ConversionResult {
    // Special 'n' rule
    if (this.buffer === 'n') {
      // Wait — may become na/ni/nu/ne/no/nya/nn
      return { committed: '', pending: 'n' };
    }

    // If buffer starts with 'n' and second char is not a vowel, y, or n → emit ん
    if (this.buffer.length >= 2 && this.buffer[0] === 'n') {
      const second = this.buffer[1];
      if (!'aiueoyn'.includes(second)) {
        const rest = this.buffer.slice(1);
        this.buffer = rest;
        const sub = this.processBuffered();
        return { committed: 'ん' + sub.committed, pending: sub.pending };
      }
    }

    // Exact match
    if (ROMAJI_TABLE[this.buffer] !== undefined) {
      const kana = ROMAJI_TABLE[this.buffer];
      this.buffer = '';
      return { committed: kana, pending: '' };
    }

    // Prefix match — keep buffering
    if (PREFIX_SET.has(this.buffer)) {
      return { committed: '', pending: this.buffer };
    }

    // No match: try shorter prefixes from the end
    for (let i = this.buffer.length - 1; i >= 1; i--) {
      const tryBuf = this.buffer.slice(0, i);
      if (ROMAJI_TABLE[tryBuf] !== undefined) {
        const kana = ROMAJI_TABLE[tryBuf];
        const rest = this.buffer.slice(i);
        this.buffer = rest;
        const sub = this.processBuffered();
        return { committed: kana + sub.committed, pending: sub.pending };
      }
    }

    // Complete fallback: emit first char as-is, retry rest
    const emitted = this.buffer[0];
    this.buffer = this.buffer.slice(1);
    if (this.buffer.length > 0) {
      const sub = this.processBuffered();
      return { committed: emitted + sub.committed, pending: sub.pending };
    }
    return { committed: emitted, pending: '' };
  }
}

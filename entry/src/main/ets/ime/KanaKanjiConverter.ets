// 変換の周りの小さな部品: 変換の学習 (読み -> 選んだ表記の回数)・直前に確定した文脈・カタカナ・予測の辞書引き。
// 変換そのものは AI 変換 (AiConverter / libkkc.so)。
//
// 以前はここに 3 つの変換エンジン (独自辞書・統計データ (Mozc)・ハイブリッド、手書きの辞書と文脈の規則を含めて 3.8 万行) と
// その辞書 (rawfile 約 85MB) があった。AI 変換が Mozc の辞書を取り込んだうえで精度も速さも上回ったので、
// 1.6.1 の次で AI だけにして外した。名前は呼ぶ側を変えないために残している。
import { AiConverter } from './AiConverter';

interface StrongWord {
  count: number;
  reading: string;
  surface: string;
}

export class KanaKanjiConverter {
  // 読み -> (選んだ表記 -> 回数)。KeyboardController が shunti_learning に保存する
  private static learned: Record<string, Record<string, number>> = {};
  // 読み -> 最後に選んだ日 (1970 年からの日数)。追い出しで古い読みから捨てる (PC 版の @day と同じ。2026-10-25)
  // learned に混ぜると予測が日付を回数と取り違えるので別の表 (shunti_learning の 'day')
  private static learnedDay: Record<string, number> = {};
  // 読み -> 左の文脈 -> 表記 -> 回数 (PC 版の @c)。learned に混ぜると予測が文脈を表記と取り違えるので別の表 (shunti_learning の 'ctx')
  private static learnedCtx: Record<string, Record<string, Record<string, number>>> = {};
  // 直前に確定した文字列 (最大 15 個)。AI 変換に左の文脈として渡す
  private static contextBuffer: string[] = [];

  static setLearned(data: Record<string, Record<string, number>>): void {
    KanaKanjiConverter.learned = data;
  }

  static getLearned(): Record<string, Record<string, number>> {
    return KanaKanjiConverter.learned;
  }

  static setLearnedDay(data: Record<string, number>): void {
    KanaKanjiConverter.learnedDay = data;
  }

  static getLearnedDay(): Record<string, number> {
    return KanaKanjiConverter.learnedDay;
  }

  static setLearnedCtx(data: Record<string, Record<string, Record<string, number>>>): void {
    KanaKanjiConverter.learnedCtx = data;
  }

  static getLearnedCtx(): Record<string, Record<string, Record<string, number>>> {
    return KanaKanjiConverter.learnedCtx;
  }

  private static ctxOf(kana: string): Record<string, Record<string, number>> | undefined {
    const t = KanaKanjiConverter.learnedCtx[kana];
    return typeof t === 'object' && t !== null ? t : undefined;
  }

  private static dropCtx(kana: string): void {
    if (KanaKanjiConverter.ctxOf(kana) === undefined) { return; }
    const next: Record<string, Record<string, Record<string, number>>> = {};
    for (const r of Object.keys(KanaKanjiConverter.learnedCtx)) {
      if (r !== kana) { next[r] = KanaKanjiConverter.learnedCtx[r]; }
    }
    KanaKanjiConverter.learnedCtx = next;
  }

  // 文脈の鍵: 左の CTX_CHARS 字 (サロゲートの片方で切らない)
  private static leftKey(left: string): string {
    if (left.length <= KanaKanjiConverter.CTX_CHARS) { return left; }
    let st = left.length - KanaKanjiConverter.CTX_CHARS;
    if ((left.charCodeAt(st) & 0xFC00) === 0xDC00) { st--; }
    return left.slice(st);
  }

  // 文脈の表に足す。使った文脈を後ろへ。あふれたら古い文脈を捨てる (PC 版と同じ)
  private static note(kana: string, left: string, surface: string): void {
    const key = KanaKanjiConverter.leftKey(left);
    const old: Record<string, Record<string, number>> = KanaKanjiConverter.ctxOf(kana) ?? {};
    const keys = Object.keys(old).filter((k: string) => k !== key);
    if (keys.length >= KanaKanjiConverter.MAX_CONTEXTS) { keys.shift(); }
    const next: Record<string, Record<string, number>> = {};
    for (const k of keys) { next[k] = old[k]; }
    const was = old[key];
    const votes: Record<string, number> = typeof was === 'object' && was !== null ? was : {};
    const n = votes[surface];
    votes[surface] = typeof n === 'number' ? n + 1 : 1;
    next[key] = votes;
    KanaKanjiConverter.learnedCtx[kana] = next;
  }

  // 文中の語の読みの表記を置き換える学習 (PC 版 Learning::top と同じ)。同じ左の文脈で選んだ回数から別の表記の票を引いて
  // SAME_CTX_MIN 以上、または ANY_CTX_MIN 回選んでいてこの文脈に別の表記の票が無い表記のうち、いちばん多く選んだもの
  static top(kana: string, left: string): string | undefined {
    const entry = KanaKanjiConverter.entryOf(kana);
    if (entry === undefined) { return undefined; }
    const here = KanaKanjiConverter.ctxOf(kana)?.[KanaKanjiConverter.leftKey(left)];
    const votes: Record<string, number> = typeof here === 'object' && here !== null ? here : {};
    let all = 0;
    for (const s of Object.keys(votes)) { all += votes[s]; }
    // 文脈を問わずに置き換えるのは、その読みでほかの表記がほとんど選ばれていないときだけ (ほかの票が 1/4 以下。PC 版と同じ)。
    // 用途の違う表記を両方選ぶ短い読み (か/化・と/都) は同じ左の文脈で選んだときだけ置き換え、ほかはモデルに任せる (ユーザー 2026-11-06)
    const total = KanaKanjiConverter.totalOf(entry);
    let best: string | undefined = undefined;
    let most = 0;
    for (const s of Object.keys(entry)) {
      const n = entry[s];
      if (typeof n !== 'number' || n <= most) { continue; }
      const v = votes[s];
      const mine = typeof v === 'number' ? v : 0;
      const against = all - mine;
      const dominant = (total - n) * 4 <= n;
      if (mine - against >= KanaKanjiConverter.SAME_CTX_MIN || (n >= KanaKanjiConverter.ANY_CTX_MIN && against === 0 && dominant)) {
        most = n;
        best = s;
      }
    }
    return best;
  }

  private static dropDay(kana: string): void {
    if (KanaKanjiConverter.learnedDay[kana] === undefined) { return; }
    const next: Record<string, number> = {};
    for (const r of Object.keys(KanaKanjiConverter.learnedDay)) {
      if (r !== kana) { next[r] = KanaKanjiConverter.learnedDay[r]; }
    }
    KanaKanjiConverter.learnedDay = next;
  }

  private static today(): number {
    return Math.floor(Date.now() / 86400000);
  }

  // その読みの学習。constructor などの読みは Object の関数が返るので、表のときだけ
  private static entryOf(kana: string): Record<string, number> | undefined {
    const entry = KanaKanjiConverter.learned[kana];
    return typeof entry === 'object' && entry !== null ? entry : undefined;
  }

  private static readonly MAX_NET_READING = 8;   // これより長い読みは文 (語ではない)
  private static readonly MAX_NET_WORDS = 500;
  private static readonly SAME_CTX_MIN = 1;    // 手で 1 度
  private static readonly ANY_CTX_MIN = 3;     // 手で 3 度
  private static readonly CTX_CHARS = 2;       // 文脈として覚える左の字数
  private static readonly MAX_CONTEXTS = 8;    // 読みごとに覚える文脈の数

  // left = 語の左の文。分からなければ省く (文脈の表には足さない)
  static recordChoice(kana: string, candidate: string, left?: string): void {
    // かなのままの表記も数える。数えないと、かなで打ち続けても 1 回選んだ漢字に勝てない (ユーザー 2026-10-31)
    if (kana.length === 0 || candidate.length === 0) { return; }
    let entry = KanaKanjiConverter.entryOf(kana);
    if (entry === undefined) {
      // 2 万読みに達したら、いちばん使われていない読みを捨てて新しい読みを入れる (以前は 500 で、すぐ押し出されていた。2026-10-24)
      // (断ると、毎日使っているうちに学習が黙って止まる)
      if (Object.keys(KanaKanjiConverter.learned).length >= 20000) {
        KanaKanjiConverter.evictWeakestLearnedReading();
      }
      const created: Record<string, number> = {};
      KanaKanjiConverter.learned[kana] = created;
      entry = created;
    }
    const count = entry[candidate];
    if (typeof count === 'number') {
      entry[candidate] = count + 1;
    } else {
      // 表記は 5 つまで。埋まっていたらいちばん回数の少ない表記を捨てる (断ると 6 つ目を何度選んでも覚えない。PC 版と同じ)
      const surfaces = Object.keys(entry);
      if (surfaces.length >= 5) {
        const old = entry;
        let weakest = surfaces[0];
        for (const s of surfaces) { if (old[s] < old[weakest]) { weakest = s; } }
        const kept: Record<string, number> = {};
        for (const s of surfaces) { if (s !== weakest) { kept[s] = old[s]; } }
        KanaKanjiConverter.learned[kana] = kept;
        entry = kept;
        KanaKanjiConverter.dropVotes(kana, weakest);
      }
      entry[candidate] = 1;
    }
    KanaKanjiConverter.learnedDay[kana] = KanaKanjiConverter.today();
    if (left !== undefined) { KanaKanjiConverter.note(kana, left, candidate); }
  }

  // かなのまま確定した: その読みの学習があるときだけ数える (全部数えると読みの枠を食う。PC 版と同じ)
  static confirmChoice(kana: string, candidate: string, left: string): boolean {
    if (KanaKanjiConverter.entryOf(kana) === undefined) { return false; }
    KanaKanjiConverter.recordChoice(kana, candidate, left);
    return true;
  }

  // 点 (いちばん選ばれた表記の回数 (20 まで) × 30 − 最後に選んでからの日数) が最も低い読みを捨てる。
  // 日付の無い (前の版の) 読みは 365 日前に選んだものとして扱う。PC 版と同じ
  private static evictWeakestLearnedReading(): void {
    const readings = Object.keys(KanaKanjiConverter.learned);
    if (readings.length === 0) { return; }
    let weakest = readings[0];
    let weakestScore = Number.MAX_SAFE_INTEGER;
    const today = KanaKanjiConverter.today();
    for (const r of readings) {
      const entry = KanaKanjiConverter.learned[r];
      let best = 0;
      for (const c of Object.keys(entry)) { if (entry[c] > best) { best = entry[c]; } }
      const score = Math.min(best, 20) * 30 - (today - (KanaKanjiConverter.learnedDay[r] ?? (today - 365)));
      if (score < weakestScore) { weakestScore = score; weakest = r; }
    }
    KanaKanjiConverter.dropDay(weakest);
    KanaKanjiConverter.dropCtx(weakest);
    const remaining: Record<string, Record<string, number>> = {};
    for (const r of readings) {
      if (r !== weakest) { remaining[r] = KanaKanjiConverter.learned[r]; }
    }
    KanaKanjiConverter.learned = remaining;
  }

  // その表記の文脈の票を消す。残すと、もう数えない表記が「別の表記の票」として top を止め続ける
  private static dropVotes(kana: string, surface: string): void {
    const table = KanaKanjiConverter.ctxOf(kana);
    if (table === undefined) { return; }
    const kept: Record<string, Record<string, number>> = {};
    for (const k of Object.keys(table)) {
      const votes: Record<string, number> = {};
      for (const s of Object.keys(table[k])) {
        if (s !== surface) { votes[s] = table[k][s]; }
      }
      if (Object.keys(votes).length > 0) { kept[k] = votes; }
    }
    KanaKanjiConverter.learnedCtx[kana] = kept;
  }

  // 間違いだった表記の票を 1 減らす (確定した直後に消して、同じ読みで別の表記を確定した。PC 版 Learning::demote と同じ。
  // ユーザー 2026-11-06)。票の無い表記は何もしない。減らしたら true
  static demote(kana: string, surface: string, left: string): boolean {
    const entry = KanaKanjiConverter.entryOf(kana);
    if (entry === undefined) { return false; }
    const n0 = entry[surface];
    if (typeof n0 !== 'number' || n0 <= 0) { return false; }
    if (n0 <= 1) {
      KanaKanjiConverter.forgetChoice(kana, surface);
      return true;
    }
    entry[surface] = n0 - 1;
    const table = KanaKanjiConverter.ctxOf(kana);
    const key = KanaKanjiConverter.leftKey(left);
    const votes = table?.[key];
    if (table !== undefined && typeof votes === 'object' && votes !== null && typeof votes[surface] === 'number') {
      const kept: Record<string, number> = {};
      for (const s of Object.keys(votes)) {
        if (s !== surface) { kept[s] = votes[s]; } else if (votes[s] > 1) { kept[s] = votes[s] - 1; }
      }
      const next: Record<string, Record<string, number>> = {};
      for (const k of Object.keys(table)) {
        if (k !== key) { next[k] = table[k]; } else if (Object.keys(kept).length > 0) { next[k] = kept; }
      }
      KanaKanjiConverter.learnedCtx[kana] = next;
    }
    return true;
  }

  // 候補を消したとき: 学習の回数も消す (残すと、消した候補を並べ替えや予測が出し続ける)
  static forgetChoice(kana: string, candidate: string): void {
    const entry = KanaKanjiConverter.entryOf(kana);
    if (entry === undefined || typeof entry[candidate] !== 'number') { return; }
    KanaKanjiConverter.dropVotes(kana, candidate);
    const next: Record<string, number> = {};
    for (const k of Object.keys(entry)) {
      if (k !== candidate) { next[k] = entry[k]; }
    }
    if (Object.keys(next).length > 0) {
      KanaKanjiConverter.learned[kana] = next;
      return;
    }
    const remaining: Record<string, Record<string, number>> = {};
    for (const r of Object.keys(KanaKanjiConverter.learned)) {
      if (r !== kana) { remaining[r] = KanaKanjiConverter.learned[r]; }
    }
    KanaKanjiConverter.learned = remaining;
    KanaKanjiConverter.dropDay(kana);
    KanaKanjiConverter.dropCtx(kana);
  }

  // 文の確定を語ごとに学習する (文全体の読みはほぼ二度と同じものが来ないので、語に分けないと次に効かない)。
  // readings と surfaces は語ごとに対応する。かなのままの語 (助詞など) は語ごとには数えない
  static recordSegmentedChoice(readings: string[], surfaces: string[]): void {
    const n = Math.min(readings.length, surfaces.length);
    for (let i = 0; i < n; i++) {
      if (readings[i] !== surfaces[i]) { KanaKanjiConverter.recordChoice(readings[i], surfaces[i]); }
    }
  }

  // その読みで選んだ回数の合計 (正の数だけ)
  private static totalOf(entry: Record<string, number>): number {
    let t = 0;
    for (const s of Object.keys(entry)) {
      const n = entry[s];
      if (typeof n === 'number' && n > 0) { t += n; }
    }
    return t;
  }

  // 2 回以上選んだ短い語を、変換の網に入れる形で返す ([読み, 表記, 代表語の読み, 代表語の表記] を 4 つずつ)。
  // 文の途中でも AI が文脈で選べるように (PC 版と同じ)。後ろの同じかな (なにを → 何を の を) は外す
  static strongForms(): string[] {
    const found: StrongWord[] = [];
    for (const r of Object.keys(KanaKanjiConverter.learned)) {
      if (r.length > KanaKanjiConverter.MAX_NET_READING) { continue; }
      const entry = KanaKanjiConverter.learned[r];
      // ほかの表記も選んでいる読み (ほかの票が 1/4 を超える: か/化・し/死) は入れない (PC 版と同じ。ユーザー 2026-11-06)
      const total = KanaKanjiConverter.totalOf(entry);
      for (const c of Object.keys(entry)) {
        if (entry[c] < 2 || (total - entry[c]) * 4 > entry[c]) { continue; }
        let n = 0;
        while (n < r.length && n < c.length && r.charAt(r.length - 1 - n) === c.charAt(c.length - 1 - n)
          && r.charAt(r.length - 1 - n) >= 'ぁ' && r.charAt(r.length - 1 - n) <= 'ゖ') { n++; }
        const rr = r.slice(0, r.length - n);
        const cc = c.slice(0, c.length - n);
        // 1 字の読み (し・か・と) は網に入れない: どの文脈でも安い 1 字の辺になり区切りまで変えた (していない -> し/ていない)。
        // 同じ左の文脈で選んだときの置き換え (top) は残る (PC 版と同じ)
        if (rr.length >= 2 && cc.length > 0 && rr !== cc) { found.push({ count: entry[c], reading: rr, surface: cc }); }
      }
    }
    found.sort((a: StrongWord, b: StrongWord) => b.count - a.count);
    // 選ばれた数で上位を取ったあとは読みの順に並べる。数の順のままだと、回数が 1 増えるたびに並びが変わって、
    // 網の入れ直しと変換の控えの捨て直しが毎回起きる (setLearnedWords は前と同じなら何もしない)
    const top = found.slice(0, KanaKanjiConverter.MAX_NET_WORDS);
    top.sort((a: StrongWord, b: StrongWord) => a.reading < b.reading ? -1 : a.reading > b.reading ? 1 :
      (a.surface < b.surface ? -1 : a.surface > b.surface ? 1 : 0));
    const out: string[] = [];
    for (const f of top) { out.push(f.reading, f.surface, f.reading, f.surface); }
    return out;
  }

  // 出来上がった候補の一覧を、この読みでよく選ばれた表記の順に前へ寄せる
  static applyLearnedOrder(kana: string, cands: string[]): string[] {
    const counts = KanaKanjiConverter.entryOf(kana);
    if (counts === undefined || Object.keys(counts).length === 0) { return cands; }
    // 候補が constructor などのときも Object の関数が返る (数えないと、どの組にも入らず候補から消える)
    const n = (c: string): number => {
      const v = counts[c];
      return typeof v === 'number' ? v : 0;
    };
    // 2 回以上選んだ表記は先頭へ (回数の多い順)。1 回だけのものはモデルの 1 位を覆さず 2 位へ
    // (1 回の誤操作や特別な文脈の選択が、どの文でも 1 位になり続けた。PC 版と同じ。2026-10-20)
    const boosted = cands.filter((c: string) => n(c) >= 2)
      .sort((a: string, b: string) => n(b) - n(a));
    const once = cands.filter((c: string) => n(c) === 1);
    const rest = cands.filter((c: string) => n(c) === 0);
    if (once.length > 0 && boosted.length === 0 && rest.length > 0) {
      boosted.push(rest.shift() as string);   // モデルの 1 位 (学習していない最初の候補) の後ろに
    }
    const out = [...boosted, ...once, ...rest];
    // 英カタカナ辞書の英単語 (全部小文字: いんじぇくしょん → injection) は、学習で上がっても 2 位まで (1 位はカタカナのまま)
    if (out.length >= 2 && /^[a-z][a-z-]+$/.test(out[0]) && /[^\u0000-\u007f]/.test(kana)) {
      return [out[1], out[0], ...out.slice(2)];
    }
    return out;
  }

  static recentContext(n: number = 40): string {
    return KanaKanjiConverter.contextBuffer.join('').slice(-n);
  }

  static addContext(text: string): void {
    if (text.length === 0) { return; }
    KanaKanjiConverter.contextBuffer.push(text);
    if (KanaKanjiConverter.contextBuffer.length > 15) {
      KanaKanjiConverter.contextBuffer = KanaKanjiConverter.contextBuffer.slice(1);
    }
  }

  // 入力欄を離れたら文脈を忘れる (別のアプリ・入力欄の文が次の変換に混ざらないように)
  static clearContext(): void {
    KanaKanjiConverter.contextBuffer = [];
  }

  // 予測の辞書引き: 読みが prefix で始まる、より長い辞書の語 (よく使う順)。AI 変換の辞書から引く
  static prefixCompletions(prefix: string, limit: number): string[] {
    return AiConverter.complete(prefix, limit);
  }

  static toKatakana(hiragana: string): string {
    let result = '';
    for (let i = 0; i < hiragana.length; i++) {
      const code = hiragana.charCodeAt(i);
      result += (code >= 0x3041 && code <= 0x3096)
        ? String.fromCharCode(code + 0x60)
        : hiragana[i];
    }
    return result;
  }

  // かな・漢字・英数字を 1 字も含まない (括弧や矢印だけの) 候補
  static isSymbolOnly(c: string): boolean {
    return c.length > 0 && !/[ぁ-んァ-ヶ一-鿿々〆〇ーA-Za-z0-9０-９]/.test(c);
  }
}

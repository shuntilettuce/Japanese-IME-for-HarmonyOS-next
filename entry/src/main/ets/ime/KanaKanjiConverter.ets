// 変換の周りの小さな部品: 変換の学習 (読み -> 選んだ表記の回数)・直前に確定した文脈・カタカナ・予測の辞書引き。
// 変換そのものは AI 変換 (AiConverter / libkkc.so)。
//
// 以前はここに 3 つの変換エンジン (独自辞書・統計データ (Mozc)・ハイブリッド、手書きの辞書と文脈の規則を含めて 3.8 万行) と
// その辞書 (rawfile 約 85MB) があった。AI 変換が Mozc の辞書を取り込んだうえで精度も速さも上回ったので、
// 1.6.1 の次で AI だけにして外した。名前は呼ぶ側を変えないために残している。
import { AiConverter } from './AiConverter';

export class KanaKanjiConverter {
  // 読み -> (選んだ表記 -> 回数)。KeyboardController が shunti_learning に保存する
  private static learned: Record<string, Record<string, number>> = {};
  // 読み -> 最後に選んだ日 (1970 年からの日数)。追い出しで古い読みから捨てる (PC 版の @day と同じ。2026-10-25)
  // learned に混ぜると予測が日付を回数と取り違えるので別の表 (shunti_learning の 'day')
  private static learnedDay: Record<string, number> = {};
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

  static recordChoice(kana: string, candidate: string): void {
    if (kana.length === 0 || candidate.length === 0 || candidate === kana) { return; }
    if (!KanaKanjiConverter.learned[kana]) {
      // 2 万読みに達したら、いちばん使われていない読みを捨てて新しい読みを入れる (以前は 500 で、すぐ押し出されていた。2026-10-24)
      // (断ると、毎日使っているうちに学習が黙って止まる)
      if (Object.keys(KanaKanjiConverter.learned).length >= 20000) {
        KanaKanjiConverter.evictWeakestLearnedReading();
      }
      KanaKanjiConverter.learned[kana] = {};
    }
    const entry = KanaKanjiConverter.learned[kana];
    if (entry[candidate] !== undefined || Object.keys(entry).length < 5) {
      entry[candidate] = (entry[candidate] ?? 0) + 1;
    }
    KanaKanjiConverter.learnedDay[kana] = KanaKanjiConverter.today();
  }

  // 点 (いちばん選ばれた表記の回数 (20 まで) × 30 − 最後に選んでからの日数) が最も低い読みを捨てる。
  // 日付の無い (前の版の) 読みは今日選んだものとして扱う。PC 版と同じ
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
      const score = Math.min(best, 20) * 30 - (today - (KanaKanjiConverter.learnedDay[r] ?? today));
      if (score < weakestScore) { weakestScore = score; weakest = r; }
    }
    KanaKanjiConverter.dropDay(weakest);
    const remaining: Record<string, Record<string, number>> = {};
    for (const r of readings) {
      if (r !== weakest) { remaining[r] = KanaKanjiConverter.learned[r]; }
    }
    KanaKanjiConverter.learned = remaining;
  }

  // 候補を消したとき: 学習の回数も消す (残すと、消した候補を並べ替えや予測が出し続ける)
  static forgetChoice(kana: string, candidate: string): void {
    const entry = KanaKanjiConverter.learned[kana];
    if (entry === undefined || entry[candidate] === undefined) { return; }
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
  }

  // 文の確定を語ごとに学習する (文全体の読みはほぼ二度と同じものが来ないので、語に分けないと次に効かない)。
  // readings と surfaces は語ごとに対応する。かなのままの語 (助詞など) は recordChoice が弾く
  static recordSegmentedChoice(readings: string[], surfaces: string[]): void {
    const n = Math.min(readings.length, surfaces.length);
    for (let i = 0; i < n; i++) {
      KanaKanjiConverter.recordChoice(readings[i], surfaces[i]);
    }
  }

  // 出来上がった候補の一覧を、この読みでよく選ばれた表記の順に前へ寄せる
  static applyLearnedOrder(kana: string, cands: string[]): string[] {
    const counts = KanaKanjiConverter.learned[kana];
    if (!counts || Object.keys(counts).length === 0) { return cands; }
    // 2 回以上選んだ表記は先頭へ (回数の多い順)。1 回だけのものはモデルの 1 位を覆さず 2 位へ
    // (1 回の誤操作や特別な文脈の選択が、どの文でも 1 位になり続けた。PC 版と同じ。2026-10-20)
    const boosted = cands.filter((c: string) => (counts[c] ?? 0) >= 2)
      .sort((a: string, b: string) => (counts[b] ?? 0) - (counts[a] ?? 0));
    const once = cands.filter((c: string) => (counts[c] ?? 0) === 1);
    const rest = cands.filter((c: string) => (counts[c] ?? 0) === 0);
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

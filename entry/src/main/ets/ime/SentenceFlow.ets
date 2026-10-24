// 文の流れの予測 (ユーザー 2026-10-13: 何度も打つ文を出したい。「お疲れ様です」を出したら「。」、続けて「本日の進捗報告です。」)。
//
// 入力欄に出た文字を、句読点・改行・送信で区切った「ひとまとまり」(節) の並びとして覚える:
//   節 -> その後に来た句読点 (お疲れ様です -> 。)
//   節 -> 次の節 (お疲れ様です -> 本日の進捗報告です)
// 確定の後の予測の帯に、
//   - 今打ち終えた所が覚えている節なら、その後によく来た句読点 (。)
//   - 句読点の直後なら、前の節の後によく来た次の節 (+ その節の後によく来る句読点): 「本日の進捗報告です。」
// を出す。2 回以上あった流れだけ (1 回だけの文は出さない)。何回打っても確定の単位がばらばらでも (お疲れ|様です) 同じ節になる。
// 端末の中だけに保存する (学習と同じ)。

const BOUNDARY = '。．！？!?、，\n';
const MAX_CLAUSE = 40;      // これより長い節は覚えない (一度きりの長文)
const MAX_KEYS = 10000;   // 以前は 1500 (2026-10-24)
const MIN_COUNT = 2;

export class SentenceFlow {
  // 'P' + 節 -> {句読点: 回数}、'N' + 節 -> {次の節: 回数}
  private static table: Record<string, Record<string, number>> = {};
  private static buf: string = '';     // 打っている途中の節
  private static prev: string = '';    // 直前に閉じた節
  private static justClosed: boolean = false;
  static onChange: (() => void) | undefined = undefined;

  static setLearned(t: Record<string, Record<string, number>>): void {
    SentenceFlow.table = t ?? {};
  }

  static getLearned(): Record<string, Record<string, number>> {
    return SentenceFlow.table;
  }

  // 入力欄の文脈が切れた (欄の切り替え・カーソルを動かした・消した): 途中の節は捨てる
  static reset(): void {
    SentenceFlow.buf = '';
    SentenceFlow.prev = '';
    SentenceFlow.justClosed = false;
  }

  // 確定した文字を 1 字消した
  static backspace(): void {
    if (SentenceFlow.buf.length > 0) {
      SentenceFlow.buf = SentenceFlow.buf.slice(0, -1);
    } else {
      SentenceFlow.reset();
    }
  }

  // 送信・改行など、文が終わった (句読点は無い)
  static boundary(): void {
    SentenceFlow.close('');
    SentenceFlow.prev = '';
  }

  // 入力欄に文字が出た (どの経路の確定でも)
  static feed(text: string): void {
    for (const ch of text) {
      if (BOUNDARY.indexOf(ch) >= 0) {
        SentenceFlow.close(ch === '\n' ? '' : ch);
      } else {
        SentenceFlow.buf += ch;
        SentenceFlow.justClosed = false;
        if (SentenceFlow.buf.length > MAX_CLAUSE * 2) { SentenceFlow.buf = SentenceFlow.buf.slice(-MAX_CLAUSE * 2); }
      }
    }
  }

  private static close(punct: string): void {
    const c = SentenceFlow.buf.trim();
    SentenceFlow.buf = '';
    if (c.length === 0 || c.length > MAX_CLAUSE) {
      SentenceFlow.justClosed = punct.length > 0;
      if (c.length > MAX_CLAUSE) { SentenceFlow.prev = ''; }
      return;
    }
    if (punct.length > 0) { SentenceFlow.bump('P' + c, punct); }
    if (SentenceFlow.prev.length > 0) { SentenceFlow.bump('N' + SentenceFlow.prev, c); }
    SentenceFlow.prev = c;
    SentenceFlow.justClosed = punct.length > 0;
    SentenceFlow.onChange?.();
  }

  private static bump(key: string, val: string): void {
    let m = SentenceFlow.table[key];
    if (m === undefined) {
      const keys = Object.keys(SentenceFlow.table);
      if (keys.length >= MAX_KEYS) {
        // いちばん使われていない鍵を 1 つ捨てる
        let worst = keys[0];
        let wn = Number.MAX_SAFE_INTEGER;
        for (const k of keys) {
          let n = 0;
          const vs = SentenceFlow.table[k];
          for (const v of Object.keys(vs)) { n += vs[v]; }
          if (n < wn) { wn = n; worst = k; }
        }
        const kept: Record<string, Record<string, number>> = {};
        for (const k of keys) { if (k !== worst) { kept[k] = SentenceFlow.table[k]; } }
        SentenceFlow.table = kept;
      }
      m = {};
      SentenceFlow.table[key] = m;
    }
    m[val] = (m[val] ?? 0) + 1;
  }

  private static top(key: string, n: number): string[] {
    const m = SentenceFlow.table[key];
    if (m === undefined) { return []; }
    return Object.keys(m).filter((v: string) => m[v] >= MIN_COUNT)
      .sort((a: string, b: string) => m[b] - m[a]).slice(0, n);
  }

  // 予測の帯に出す続き (先頭ほど確か)
  static predict(): string[] {
    const out: string[] = [];
    const cur = SentenceFlow.buf.trim();
    if (cur.length > 0) {
      // 覚えている節を打ち終えた: その後によく来た句読点
      for (const p of SentenceFlow.top('P' + cur, 2)) { out.push(p); }
    } else if (SentenceFlow.prev.length > 0) {
      // 句読点・送信の直後: 前の節の後によく来た次の節 (+ その節の後によく来る句読点)
      for (const nx of SentenceFlow.top('N' + SentenceFlow.prev, 3)) {
        const p = SentenceFlow.top('P' + nx, 1);
        out.push(nx + (p.length > 0 ? p[0] : ''));
      }
    }
    return out;
  }
}

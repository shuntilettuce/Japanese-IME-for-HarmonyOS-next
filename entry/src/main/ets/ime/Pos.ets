// 品詞の表 (desktop/core/pos_classes.h の POS_CLASSES・POS_GROUPS、辞書の左 ID ごとに 1 字。エンジンから 1 回だけ読む)。
// 品詞ごとの学習 (PC 版・Android と同じ。2026-11-09) で使う。品詞が分からない (-1) ときは表を読まない
import kkc from 'libkkc.so';

// 文の中の語 (読み・表記・品詞の左 ID)
export interface PosWord {
  reading: string;
  surface: string;
  lid: number;
}

export class Pos {
  static readonly NO_GROUP: string = '';   // 品詞の仲間が分からない
  private static tables: string[] | undefined = undefined;

  private static table(k: number): string {
    if (Pos.tables === undefined) {
      try {
        const t = kkc.posTables();
        Pos.tables = t.length === 2 ? t : ['', ''];
      } catch (e) {
        Pos.tables = ['', ''];
      }
    }
    return Pos.tables[k];
  }

  // 文節の分類: 1 付属語・2 接頭・3 サ変接続の名詞・4 サ変・スルの動詞・0 自立語
  static cls(lid: number): string {
    if (lid < 0) { return '0'; }
    const t = Pos.table(0);
    return lid < t.length ? t.charAt(lid) : '0';
  }

  // 学習を分ける粗い品詞 (N 名詞・V 動詞・P 助詞・X 助動詞 …。PC 版 group_of_lid)。分からなければ NO_GROUP
  static group(lid: number): string {
    if (lid < 0) { return Pos.NO_GROUP; }
    const t = Pos.table(1);
    return lid < t.length ? t.charAt(lid) : Pos.NO_GROUP;
  }

  // 文の語のうち、読みの位置 r0 から始まる文節の頭の語の品詞 (接頭詞は飛ばす。PC 版 head_lid)。無ければ -1
  static headLid(ws: PosWord[], r0: number): number {
    let r = 0;
    for (let i = 0; i < ws.length; i++) {
      if (r === r0) {
        let k = i;
        while (k + 1 < ws.length && ws[k].lid >= 0 && Pos.cls(ws[k].lid) === '2') { k++; }
        return ws[k].lid;
      }
      if (r > r0) { return -1; }
      r += ws[i].reading.length;
    }
    return -1;
  }

  // pathPos の結果 ([読みの終わり, 表記の長さ, 左 ID, 右 ID] の並び) を語に分ける (Android Engine.pathPos と同じ)。合わなければ空
  static words(reading: string, text: string, a: number[]): PosWord[] {
    const out: PosWord[] = [];
    let r0 = 0;
    let s0 = 0;
    for (let i = 0; i + 3 < a.length; i += 4) {
      const r1 = a[i];
      const s1 = s0 + a[i + 1];
      if (r1 > reading.length || s1 > text.length || r1 < r0) { return []; }
      out.push({ reading: reading.slice(r0, r1), surface: text.slice(s0, s1), lid: a[i + 2] });
      r0 = r1;
      s0 = s1;
    }
    return out;
  }
}

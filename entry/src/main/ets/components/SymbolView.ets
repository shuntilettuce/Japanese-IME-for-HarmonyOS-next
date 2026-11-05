import { InputHandler } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';

const SYM_TABS: string[] = ['🕐', '1?#', '( )', '→', '⊞', '①', '◇', '[全]'];

// Fallback for the 🕐 tab until the user has actually tapped enough symbols
// for real usage data (see InputHandler.recordSymbolUse/publishFrequentSymbols)
// to fill it in.
const DEFAULT_RECENT_SYMBOLS: string[] = [
  '、', '。', '！', '？', '・', '…', '〜', '―',
  '「', '」', '『', '』', '【', '】', '（', '）',
  '♪', '♥', '★', '☆', '◎', '○', '●', '◆',
];

const SYMBOL_CATEGORIES: string[][][] = [
  // 0: 🕐 recently used — replaced at render time by SymbolView.recentRows()
  [],
  // 1: 1?# — ASCII symbols
  [
    ['1', '2', '3', '4', '5', '6', '7', '8'],
    ['9', '0', '!', '@', '#', '$', '%', '^'],
    ['&', '*', '(', ')', "'", '"', '=', '_'],
    ['`', ':', ';', '?', '~', '|', '+', '-'],
    ['\\', '/', '[', ']', '{', '}', '<', '>'],
    ['“', '”', '.', '‘', '’', 'ı', '¿', '¥'],
    ['€', '£', '¢', '©', '®', '™', '℃', '°F'],
  ],
  // 2: ( ) brackets / Japanese punctuation
  [
    ['(', ')', '[', ']', '{', '}', '〈', '〉'],
    ['《', '》', '「', '」', '『', '』', '【', '】'],
    ['〔', '〕', '（', '）', '［', '］', '｛', '｝'],
    // 括弧は全種類をここで打てるように (ime/Brackets.ets と同じ種類)
    ['〖', '〗', '〘', '〙', '〚', '〛', '｟', '｠'],
    ['｢', '｣', '«', '»', '‹', '›', '“', '”'],
    ['＜', '＞', '〝', '〟', '〃', '‥', '…', '―'],
    ['、', '。', '・', '：', '；', '！', '？', '〜'],
  ],
  // 3: → arrows
  [
    ['←', '→', '↑', '↓', '↖', '↗', '↘', '↙'],
    ['⇐', '⇒', '⇑', '⇓', '⇖', '⇗', '⇘', '⇙'],
    ['↔', '↕', '⇔', '⇕', '⤴', '⤵', '↩', '↪'],
    ['➡', '⬆', '⬇', '⬅', '↳', '↰', '↱', '↲'],
    ['▷', '▶', '◁', '◀', '▽', '▼', '△', '▲'],
  ],
  // 4: ⊞ math / special
  [
    ['×', '÷', '±', '∓', '√', '∞', 'π', 'μ'],
    ['∑', '∏', '∫', '∂', '∇', '°', '′', '″'],
    ['≠', '≈', '≡', '≤', '≥', '≪', '≫', '∝'],
    ['∈', '∉', '∅', '∩', '∪', '⊂', '⊃', '⊕'],
    ['α', 'β', 'γ', 'δ', 'θ', 'λ', 'σ', 'φ'],
  ],
  // 5: ① circled numbers / Roman numerals
  [
    ['①', '②', '③', '④', '⑤', '⑥', '⑦', '⑧'],
    ['⑨', '⑩', '⑪', '⑫', '⑬', '⑭', '⑮', '⑯'],
    ['⑰', '⑱', '⑲', '⑳', '㉑', '㉒', '㉓', '㉔'],
    ['Ⅰ', 'Ⅱ', 'Ⅲ', 'Ⅳ', 'Ⅴ', 'Ⅵ', 'Ⅶ', 'Ⅷ'],
    ['Ⅸ', 'Ⅹ', 'ⅰ', 'ⅱ', 'ⅲ', 'ⅳ', 'ⅴ', 'ⅵ'],
  ],
  // 6: ◇ shapes / symbols
  [
    ['★', '☆', '♠', '♥', '♦', '♣', '♪', '♫'],
    ['☀', '☁', '☂', '☃', '⚡', '❄', '✿', '❀'],
    ['♀', '♂', '☯', '☮', '✓', '✗', '✔', '✘'],
    ['◆', '◇', '■', '□', '▲', '△', '▼', '▽'],
    ['●', '○', '◎', '◉', '◈', '◊', '⬛', '⬜'],
  ],
  // 7: [全] full-width ASCII
  [
    ['！', '＂', '＃', '＄', '％', '＆', '＇', '（'],
    ['）', '＊', '＋', '，', '－', '．', '／', '：'],
    ['；', '＜', '＝', '＞', '？', '＠', '｛', '｝'],
    ['｜', '￥', '＾', '＿', '｀', '～', '【', '】'],
    ['Ａ', 'Ｂ', 'Ｃ', 'Ｄ', 'Ｅ', 'Ｆ', 'Ｇ', 'Ｈ'],
    ['Ｉ', 'Ｊ', 'Ｋ', 'Ｌ', 'Ｍ', 'Ｎ', 'Ｏ', 'Ｐ'],
    ['Ｑ', 'Ｒ', 'Ｓ', 'Ｔ', 'Ｕ', 'Ｖ', 'Ｗ', 'Ｘ'],
  ],
];

const GRID_ROW_GAP = 4;
// キーは黄金長方形 (横:縦 = 1.618:1) に。44 固定では縦長すぎた (ユーザー 2026-10-13)
const GOLDEN = 1.618;

interface SymRow {
  cat: number;
  items: string[];   // 空なら種類の見出しの行
}

// 種類の見出し (タブの並びと同じ。1 本につないだ一覧の区切りに出す。ユーザー 2026-10-14)
const SYM_GENRES: string[] = ['よく使う', '英数・記号', 'かっこ', '矢印', '数学', '丸数字・ローマ数字', '図形・しるし', '全角の記号'];

@Component
export struct SymbolView {
  @StorageLink('symKeyWidthVp') symKeyWidthVp: number = 46;
  @StorageLink('isDark') isDark: boolean = false;
  // 記号は絵文字の欄と同じく全部の種類を 1 本につないで縦に送る (ユーザー 2026-10-13: キーボードを低くすると
  // 下の段が埋もれ、行を縮めると狭すぎた)。タブはその種類の先頭へ飛ぶ、見えている所に合わせてタブが動く
  private scroller: Scroller = new Scroller();
  @StorageLink('frequentSymbols') frequentSymbols: string[] = [];
  @State currentTab: number = 1;

  private h(): InputHandler { return InputHandler.getInstance(); }

  // Snapshot the 🕐 "recent" tab ordering once, when the panel opens -- so it
  // stays stable while the user taps within it (recordSymbolUse no longer
  // re-sorts live). Re-runs each time the panel is reopened.
  aboutToAppear(): void {
    this.h().refreshFrequentSymbols();
  }

  private insert(sym: string): void {
    this.h().commitTopWithPending();
    this.h().insertText(sym);
    this.h().recordSymbolUse(sym);
  }

  // Chunks the user's most-tapped symbols into rows of 8 for the 🕐 tab,
  // falling back to a fixed common-punctuation set until real usage data
  // (persisted via InputHandler.recordSymbolUse) has built up.
  private recentRows(): string[][] {
    const flat = this.frequentSymbols.length > 0 ? this.frequentSymbols : DEFAULT_RECENT_SYMBOLS;
    const rows: string[][] = [];
    for (let i = 0; i < flat.length; i += 8) {
      rows.push(flat.slice(i, i + 8));
    }
    return rows;
  }

  private allRows(): SymRow[] {
    const rows: SymRow[] = [];
    for (let c = 0; c < SYMBOL_CATEGORIES.length; c++) {
      rows.push({ cat: c, items: [] });
      for (const items of (c === 0 ? this.recentRows() : SYMBOL_CATEGORIES[c])) { rows.push({ cat: c, items: items }); }
    }
    return rows;
  }

  private catStart(cat: number): number {
    let n = 0;
    for (let c = 0; c < cat; c++) { n += 1 + (c === 0 ? this.recentRows().length : SYMBOL_CATEGORIES[c].length); }
    return n;
  }

  build() {
    Column({ space: 0 }) {
      // ── Top tab row ──────────────────────────────────────────
      Row({ space: 4 }) {
        // ← back to kana
        Row() {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(18)
            .fontColor([kbTheme(this.isDark).textPrimary])
        }
        .width(48).height(40)
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).specialBg)
        .borderRadius(20)
        .onClick(() => {
          AppStorage.setOrCreate('keyboardSubMode', 'kana');
        })

        ForEach([0, 1, 2, 3, 4, 5, 6, 7], (i: number) => {
          Text(SYM_TABS[i])
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(i === 1 ? 12 : 14)
            .textAlign(TextAlign.Center)
            .width(38).height(32)
            .fontColor(this.currentTab === i ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .backgroundColor(this.currentTab === i ? kbTheme(this.isDark).accent : kbTheme(this.isDark).funcBg)
            .borderRadius(8)
            .onClick(() => {
              this.currentTab = i;
              this.scroller.scrollToIndex(this.catStart(i));
            })
        }, (i: number) => String(i))
      }
      .width('100%')
      .height(48)
      .padding({ left: 4, right: 4, top: 4 })
      .alignItems(VerticalAlign.Center)

      // ── Symbol grid: row height fills the fixed grid budget (capped —
      // see gridRowHeight above), extra slack centers as padding so
      // switching tabs never changes the panel's total height. ──
      List({ scroller: this.scroller, space: GRID_ROW_GAP }) {
        ForEach(this.allRows(), (row: SymRow) => {
          ListItem() {
            if (row.items.length === 0) {
              Text(row.cat < SYM_GENRES.length ? SYM_GENRES[row.cat] : '')
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(11)
                .fontColor(kbTheme(this.isDark).textSecondary)
                .width('100%')
                .padding({ left: 12, top: row.cat === 0 ? 2 : 8, bottom: 2 })
            } else {
            Row({ space: 4 }) {
              ForEach(row.items, (sym: string) => {
                Text(sym)
                  .fontFamily(JP_FONT_FAMILY)
                  .fontSize(sym.length > 1 ? 12 : 18)
                  .textAlign(TextAlign.Center)
                  .width(this.symKeyWidthVp)
                  .height(Math.max(28, Math.round(this.symKeyWidthVp / GOLDEN)))
                  .fontColor(kbTheme(this.isDark).textPrimary)
                  .backgroundColor(kbTheme(this.isDark).keyBg)
                  .borderRadius(8)
                  .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
                  .onClick(() => { this.insert(sym); })
              }, (sym: string) => 's' + sym)
            }
            .width('100%')
            .justifyContent(FlexAlign.Center)
            }
          }
        }, (row: SymRow, idx: number) => `${row.cat}-${idx}-${row.items.join('')}`)
      }
      .width('100%')
      .layoutWeight(1)
      .scrollBar(BarState.Off)
      .edgeEffect(EdgeEffect.Spring)
      .onScrollIndex((first: number) => {
        const rows = this.allRows();
        if (first < 0 || first >= rows.length) { return; }
        if (rows[first].cat !== this.currentTab) { this.currentTab = rows[first].cat; }
      })

      // ── Bottom function row ───────────────────────────────────
      Row({ space: 0 }) {
        // あいう — back to kana
        Text('あいう')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14).textAlign(TextAlign.Center)
          .width(68).height(44)
          .fontColor(kbTheme(this.isDark).textSecondary)
          .onClick(() => {
            AppStorage.setOrCreate('keyboardSubMode', 'kana');
          })

        // Emoji button → emoji panel
        Text('☺')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(18)
          .textAlign(TextAlign.Center)
          .width(46).height(44)
          .fontColor(kbTheme(this.isDark).textSecondary)
          .backgroundColor(kbTheme(this.isDark).funcBg)
          .borderRadius(8)
          .onClick(() => { AppStorage.setOrCreate('keyboardSubMode', 'emoji'); })

        // ⌫ backspace
        Row() {
          SymbolGlyph($r('sys.symbol.delete_left'))
            .fontSize(18)
            .fontColor([kbTheme(this.isDark).textPrimary])
        }
        .width(52).height(44)
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).specialBg)
        .borderRadius(8)
        .onClick(() => { this.h().handleBackspace(); })
      }
      .width('100%')
      .height(52)
      .padding({ left: 4, right: 4, bottom: 4 })
      .justifyContent(FlexAlign.SpaceBetween)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(kbTheme(this.isDark).panelBg)
  }
}

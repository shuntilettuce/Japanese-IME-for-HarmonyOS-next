import { InputHandler } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';

const EMOJI_TABS: string[] = ['😀', '👋', '🐶', '🍎', '🚗', '⚽', '💡', '❤️'];

const EMOJI_CATEGORIES: string[][][] = [
  // 0: 😀 顔・感情
  [
    ['😀', '😃', '😄', '😁', '😆', '😅', '🤣', '😂'],
    ['🙂', '😉', '😊', '😇', '🥰', '😍', '😘', '😗'],
    ['😋', '😛', '😜', '🤪', '😝', '🤑', '🤗', '🤭'],
    ['🤔', '🤐', '😐', '😑', '😶', '😏', '😒', '🙄'],
    ['😔', '😪', '🤤', '😴', '😷', '🤒', '🤕', '🥳'],
    ['😎', '🤓', '🧐', '😕', '🙁', '😮', '😳', '🥺'],
    ['😢', '😭', '😱', '😣', '😞', '😓', '😩', '😫'],
    ['😤', '😡', '😠', '🤬', '😈', '👿', '💀', '👻'],
  ],
  // 1: 👋 人・ジェスチャー
  [
    ['👋', '🤚', '✋', '🖐', '👌', '✌', '🤞', '🤙'],
    ['👍', '👎', '✊', '👊', '🤛', '🤜', '👏', '🙌'],
    ['🤲', '🙏', '💪', '🦾', '☝', '👆', '👇', '👉'],
    ['👦', '👧', '🧒', '👩', '👨', '🧑', '👶', '👼'],
    ['👮', '💂', '🥷', '🦸', '🦹', '🧙', '🧚', '🧛'],
    ['💆', '💇', '🚶', '🧍', '🧎', '🏃', '💃', '🕺'],
    ['😺', '😸', '😹', '😻', '😼', '😽', '🙀', '😿'],
    ['👫', '👬', '👭', '💑', '👨‍👩‍👦', '👨‍👩‍👧', '👪', '🧑‍🤝‍🧑'],
  ],
  // 2: 🐶 動物・自然
  [
    ['🐶', '🐱', '🐭', '🐹', '🐰', '🦊', '🐻', '🐼'],
    ['🐨', '🐯', '🦁', '🐮', '🐷', '🐸', '🐵', '🐔'],
    ['🐧', '🐦', '🦆', '🦅', '🦉', '🦋', '🐛', '🐌'],
    ['🐞', '🐜', '🐢', '🐍', '🦎', '🐳', '🐬', '🦈'],
    ['🐠', '🐡', '🐙', '🦑', '🦐', '🦞', '🦀', '🐊'],
    ['🌸', '🌺', '🌻', '🌹', '🌷', '🌼', '🌿', '🍀'],
    ['🍁', '🍂', '🌲', '🌳', '🌴', '🌵', '🎋', '🎍'],
    ['🌞', '🌝', '🌛', '🌚', '🌕', '🌈', '⛅', '🌊'],
  ],
  // 3: 🍎 食べ物・飲み物
  [
    ['🍎', '🍐', '🍊', '🍋', '🍌', '🍉', '🍇', '🍓'],
    ['🍒', '🍑', '🥭', '🍍', '🥥', '🥝', '🍅', '🍆'],
    ['🥑', '🥦', '🥬', '🌽', '🥕', '🍄', '🧅', '🧄'],
    ['🍞', '🥐', '🥖', '🧀', '🥚', '🍳', '🥞', '🧇'],
    ['🍔', '🍟', '🍕', '🌭', '🌮', '🌯', '🥙', '🧆'],
    ['🍣', '🍱', '🍛', '🍲', '🍜', '🍝', '🍤', '🍙'],
    ['🍦', '🍧', '🍨', '🍩', '🍪', '🎂', '🍰', '🧁'],
    ['☕', '🍵', '🧃', '🥤', '🧋', '🍺', '🍷', '🥂'],
  ],
  // 4: 🚗 乗り物・場所
  [
    ['🚗', '🚕', '🚙', '🚌', '🚎', '🏎', '🚓', '🚑'],
    ['🚒', '🚐', '🚚', '🚛', '🚜', '🏍', '🛵', '🚲'],
    ['✈', '🚀', '🛸', '🚁', '⛵', '🚢', '🚂', '🚃'],
    ['🌍', '🗺', '🗻', '🏔', '🏕', '🏖', '🏝', '🏜'],
    ['🏡', '🏢', '🏣', '🏥', '🏦', '🏨', '🗼', '🗽'],
    ['🎡', '🎢', '🎠', '⛩', '🕌', '🕍', '⛪', '🛕'],
    ['🌃', '🌆', '🌇', '🌉', '🌁', '🌄', '🌅', '🌠'],
    ['⛰', '🌋', '🏗', '🏙', '🌐', '🧭', '🚏', '⛽'],
  ],
  // 5: ⚽ スポーツ・活動
  [
    ['⚽', '🏀', '🏈', '⚾', '🎾', '🏐', '🏉', '🎱'],
    ['🏓', '🏸', '🥊', '🏹', '🎣', '🤿', '🎿', '⛷'],
    ['🏂', '🏋', '🤸', '⛹', '🏌', '🏇', '🏊', '🚵'],
    ['🎯', '🎳', '🏆', '🥇', '🥈', '🥉', '🎖', '🏅'],
    ['🎮', '🕹', '🎲', '♟', '🎭', '🎪', '🎨', '🖼'],
    ['🎬', '🎥', '📹', '📷', '🎤', '🎧', '🎵', '🎶'],
    ['🎸', '🎹', '🎺', '🎻', '🥁', '🎷', '🪕', '🎙'],
    ['🎠', '🎡', '🎢', '🎪', '🎫', '🎟', '🎗', '🏮'],
  ],
  // 6: 💡 モノ
  [
    ['💡', '🔦', '🕯', '🔋', '🔌', '💻', '🖥', '📱'],
    ['⌨', '🖱', '🖨', '📠', '📺', '📻', '📷', '📹'],
    ['📚', '📖', '📝', '✏', '🖊', '📌', '📎', '🔑'],
    ['🔓', '🔒', '🔨', '⛏', '🔧', '🔩', '⚙', '🪛'],
    ['💊', '🩺', '💉', '🌡', '🔭', '🔬', '🧪', '🧫'],
    ['⌚', '👓', '🕶', '🥽', '👜', '👝', '🎒', '💼'],
    ['💰', '💳', '📮', '🗑', '🚪', '🛋', '🚽', '🛁'],
    ['🎁', '🎀', '🎊', '🎉', '🎈', '🧨', '✉', '📦'],
  ],
  // 7: ❤️ 記号・ハート
  [
    ['❤', '🧡', '💛', '💚', '💙', '💜', '🖤', '🤍'],
    ['🤎', '💖', '💗', '💓', '💞', '💕', '💟', '💔'],
    ['💯', '💢', '💥', '💫', '💦', '💨', '🔥', '✨'],
    ['⭐', '🌟', '⚡', '🌈', '☀', '🌙', '❄', '🌊'],
    ['🔴', '🟠', '🟡', '🟢', '🔵', '🟣', '⚫', '⚪'],
    ['🔺', '🔻', '🔷', '🔶', '🔹', '🔸', '🔲', '🔳'],
    ['✅', '❎', '🆗', '🆕', '🆙', '🆒', '🆓', '🔃'],
    ['🚫', '⛔', '❌', '⭕', '❓', '❗', '‼', '⁉'],
  ],
];

const GRID_ROW_GAP = 4;
// 連続スクロールになったので、行を領域に押し込む必要がなくなった。
// 以前は全8行を 252px に収めるため行高を計算で縮めていた。
const GRID_ROW_H = 44;

// 全カテゴリを1本に繋いだ行の列。カテゴリごとにタブを切り替えるのではなく、
// 下へスクロールすれば全部の絵文字が続けて出る形にするため。
interface EmojiRow {
  cat: number;
  items: string[];
}

function buildRows(): EmojiRow[] {
  const rows: EmojiRow[] = [];
  for (let c = 0; c < EMOJI_CATEGORIES.length; c++) {
    for (const items of EMOJI_CATEGORIES[c]) {
      rows.push({ cat: c, items: items });
    }
  }
  return rows;
}

// 各カテゴリが始まる行番号。タブを押したときの飛び先。
function buildCatStart(): number[] {
  const starts: number[] = [];
  let n = 0;
  for (let c = 0; c < EMOJI_CATEGORIES.length; c++) {
    starts.push(n);
    n += EMOJI_CATEGORIES[c].length;
  }
  return starts;
}

const EMOJI_ROWS: EmojiRow[] = buildRows();
const CAT_START: number[] = buildCatStart();

@Component
export struct EmojiView {
  @StorageLink('symKeyWidthVp') symKeyWidthVp: number = 46;
  @StorageLink('isDark') isDark: boolean = false;
  @State currentTab: number = 0;
  // タブ押下でその位置へ飛ばすため、リストの位置を握っておく。
  private scroller: Scroller = new Scroller();

  private h(): InputHandler { return InputHandler.getInstance(); }

  build() {
    Column({ space: 0 }) {
      // ── Tab row ───────────────────────────────────────────────
      Row({ space: 4 }) {
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
          AppStorage.setOrCreate('keyboardSubMode', 'symbol');
        })

        ForEach([0, 1, 2, 3, 4, 5, 6, 7], (i: number) => {
          Text(EMOJI_TABS[i])
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(16)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .textAlign(TextAlign.Center)
            .width(34).height(32)
            .backgroundColor(this.currentTab === i ? kbTheme(this.isDark).accent : kbTheme(this.isDark).funcBg)
            .borderRadius(8)
            .onClick(() => {
              this.currentTab = i;
              // 押したカテゴリの先頭行へ飛ぶ。切り替えではなく移動。
              this.scroller.scrollToIndex(CAT_START[i]);
            })
        }, (i: number) => String(i))
      }
      .width('100%')
      .height(48)
      .padding({ left: 4, right: 4, top: 4 })
      .alignItems(VerticalAlign.Center)

      // ── 絵文字。全カテゴリが1本に繋がっていて、下へスクロールすれば
      //    最後まで出る。どのカテゴリを見ているかはタブ側が追従する。 ──
      List({ scroller: this.scroller, space: GRID_ROW_GAP }) {
        ForEach(EMOJI_ROWS, (row: EmojiRow, idx: number) => {
          ListItem() {
            Row({ space: 4 }) {
              ForEach(row.items, (emoji: string) => {
                Text(emoji)
                  .fontFamily(JP_FONT_FAMILY)
                  .fontSize(20)
                  .textAlign(TextAlign.Center)
                  .width(this.symKeyWidthVp)
                  .height(GRID_ROW_H)
                  .backgroundColor(kbTheme(this.isDark).keyBg)
                  .borderRadius(8)
                  .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
                  .onClick(() => {
                    this.h().commitTopWithPending();
                    this.h().insertText(emoji);
                  })
              }, (emoji: string) => 'e' + emoji)
            }
            .width('100%')
            .justifyContent(FlexAlign.Center)
          }
        }, (row: EmojiRow, idx: number) => 'r' + String(idx))
      }
      .width('100%')
      .layoutWeight(1)   // キーボードを低くしても下の段が埋もれないように、残りの高さを使う (スクロールできる)
      .scrollBar(BarState.Off)
      .edgeEffect(EdgeEffect.Spring)
      // 先頭に見えている行のカテゴリへタブを合わせる。指で送っている間も
      // 現在地が分かるようにするため。
      .onScrollIndex((first: number) => {
        if (first < 0 || first >= EMOJI_ROWS.length) { return; }
        const c = EMOJI_ROWS[first].cat;
        if (c !== this.currentTab) { this.currentTab = c; }
      })

      // ── Bottom row ────────────────────────────────────────────
      Row({ space: 0 }) {
        Text('あいう')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14).textAlign(TextAlign.Center)
          .width(68).height(44)
          .fontColor(kbTheme(this.isDark).textSecondary)
          .onClick(() => {
            AppStorage.setOrCreate('keyboardSubMode', 'kana');
          })

        Text('←記')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14).textAlign(TextAlign.Center)
          .width(60).height(44)
          .fontColor(kbTheme(this.isDark).textSecondary)
          .backgroundColor(kbTheme(this.isDark).funcBg)
          .borderRadius(8)
          .onClick(() => {
            AppStorage.setOrCreate('keyboardSubMode', 'symbol');
          })

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

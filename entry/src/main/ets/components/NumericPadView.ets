import { InputHandler, InputMode } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { MODE_KEY_FONT_SIZE, flickGlyphSize, NUM_CENTER_FONT_SIZE, NUM_CENTER_ACTIVE_FONT_SIZE, NUM_CENTER_LANDSCAPE_FONT_SIZE, BACKSPACE_FONT_SIZE } from './ModeLabel';
import { flickTriangleCommands, BUBBLE_VP } from './FlickFeedback';
import { JP_FONT_FAMILY } from './JpFont';

type FlickDir = 'center' | 'up' | 'right' | 'down' | 'left';

interface FlickKeyDef {
  center: string;
  up?: string;
  right?: string;
  down?: string;
  left?: string;
}

function getCharByDir(key: FlickKeyDef, dir: FlickDir): string {
  if (dir === 'up' && key.up !== undefined) { return key.up; }
  if (dir === 'right' && key.right !== undefined) { return key.right; }
  if (dir === 'down' && key.down !== undefined) { return key.down; }
  if (dir === 'left' && key.left !== undefined) { return key.left; }
  return key.center;
}

// Gboard-style numeric pad, reached via the あA/Aあ key's 日本語→アルファベット→
// 数字 cycle (see InputHandler.cycleInputMode) when 数字パッド is enabled in
// settings. Same 5-column (side keys + 3 digit columns) grid as FlickKeyboardView so it
// reuses flickKeyWidthVp and never fights that keyboard's width sizing.
// Symbol input from the digit-only pad was poor (記号 panel is a whole extra
// trip) -- each digit now flicks to a symbol, the same gesture
// FlickKeyboardView already teaches users for kana, so there's no new
// interaction to learn. Digits 1-9/0 match Gboard's own number pad exactly
// (confirmed against a real device screenshot: LEFT/UP/RIGHT only, no down
// direction used anywhere on Gboard's pad). -/. aren't part of that
// reference screenshot (Gboard replaces them with its own tap-cycle bracket/
// punctuation keys, a different interaction this file doesn't have yet), so
// they keep the same 3-direction shape with a reasonable symbol pick instead.
const NUM_FLICK_GRID: FlickKeyDef[][] = [
  [
    { center: '1', left: '☆', up: '♪', right: '→' },
    { center: '2', left: '¥', up: '$', right: '€' },
    { center: '3', left: '%', up: '°', right: '#' },
  ],
  [
    { center: '4', left: '○', up: '*', right: '・' },
    { center: '5', left: '+', up: '×', right: '÷' },
    { center: '6', left: '<', up: '=', right: '>' },
  ],
  [
    { center: '7', left: '「', up: '」', right: ':' },
    { center: '8', left: '〒', up: '々', right: '〆' },
    { center: '9', left: '^', up: '|', right: '\\' },
  ],
  [
    { center: '-', left: '(', up: ')', right: '_' },
    { center: '0', left: '~', up: '…', right: '@' },
    { center: '.', left: ',', up: ';', right: '/' },
  ],
];

@Component
export struct NumericPadView {
@StorageLink('flickSideKeyWidthVp') flickSideKeyWidthVp: number = 48; // 必要に応じて調整してください
  @StorageLink('flickKeyWidthVp') flickKeyWidthVp: number = 72;
  @StorageLink('keyboardHeightVp') keyboardHeightVp: number = 260;
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('keyChromeEnabled') keyChromeEnabled: boolean = true;
  // 数字パッドが有効だと「あA」キーは 日本語→アルファベット→数字 の3段階に
  // なるので、ラベルも切り替わる順に並べる (あA1 / A1あ / 1あA)。
  // 半角の A/1 は ひらがな と字形の大きさが厳密には揃わないが、
  // 全角にすると見た目が野暮ったくなるため半角のままにしている。
  @StorageLink('numericPadEnabled') numericPadEnabled: boolean = false;
  @StorageLink('isLandscape') isLandscape: boolean = false;

  @State activeRow: number = -1;
  @State activeCol: number = -1;
  @State touchStartX: number = 0;
  @State touchStartY: number = 0;
  @State flickDir: FlickDir = 'center';
  // キー 1 個の描画上の大きさ (vp)。onAreaChange で更新する (三角と浮き文字の位置に使う)。
  @State keyAreaW: number = 0;
  @State keyAreaH: number = 0;

  private readonly THRESHOLD: number = 20;

  private h(): InputHandler { return InputHandler.getInstance(); }

  private bsDelayTimer: number = -1;
  private bsRepeatTimer: number = -1;
  private curDelayTimer: number = -1;
  private curRepeatTimer: number = -1;
  private curAccelTimer: number = -1;

  private startBsRepeat(): void {
    this.h().handleBackspace();
    this.bsDelayTimer = setTimeout(() => {
      this.bsRepeatTimer = setInterval(() => {
        this.h().handleBackspace();
      }, 100);
    }, 500);
  }

  private stopBsRepeat(): void {
    clearTimeout(this.bsDelayTimer);
    clearInterval(this.bsRepeatTimer);
    this.bsDelayTimer = -1;
    this.bsRepeatTimer = -1;
  }

  // Hold-to-repeat for the ◄/► caret keys, accelerating once held past ~1s.
  private startCurRepeat(dir: string): void {
    const move = (): void => {
      if (dir === 'left') { this.h().handleCursorLeft(); }
      else { this.h().handleCursorRight(); }
    };
    move();
    this.curDelayTimer = setTimeout(() => {
      this.curRepeatTimer = setInterval(move, 110);
      this.curAccelTimer = setTimeout(() => {
        clearInterval(this.curRepeatTimer);
        this.curRepeatTimer = setInterval(move, 40);
      }, 600);
    }, 400);
  }

  private stopCurRepeat(): void {
    clearTimeout(this.curDelayTimer);
    clearInterval(this.curRepeatTimer);
    clearTimeout(this.curAccelTimer);
    this.curDelayTimer = -1;
    this.curRepeatTimer = -1;
    this.curAccelTimer = -1;
  }

  // 数字・記号も入力中の文字として組み立てる (2000 -> 2,000・二千 の候補が出る。Enter か候補で確定)
  private insert(ch: string): void {
    this.h().appendRaw(ch);
  }

  private dirFrom(dx: number, dy: number): FlickDir {
    if (Math.abs(dx) < this.THRESHOLD && Math.abs(dy) < this.THRESHOLD) { return 'center'; }
    if (Math.abs(dx) >= Math.abs(dy)) { return dx > 0 ? 'right' : 'left'; }
    return dy < 0 ? 'up' : 'down';
  }

  private isActive(row: number, col: number): boolean {
    return this.activeRow === row && this.activeCol === col;
  }

  private hintColor(row: number, col: number, dir: FlickDir): ResourceColor {
    if (this.isActive(row, col) && this.flickDir === dir) { return kbTheme(this.isDark).accentStrong; }
    return kbTheme(this.isDark).textSecondary;
  }

  // 中央の数字は、フリック中でも常に自分の数字を出す (フリック先は浮き文字に出す)
  private centerLabel(row: number, col: number, key: FlickKeyDef): string {
    return key.center;
  }

  private keyBg(row: number, col: number): ResourceColor {
    return this.keyChromeEnabled ? kbTheme(this.isDark).keyBg : 'transparent';
  }

  // キーの実寸 (三角を描く範囲)。計測前は推定値を使う。
  private keyW(): number {
    return this.keyAreaW > 0 ? this.keyAreaW : this.flickKeyWidthVp;
  }

  private keyH(): number {
    if (this.keyAreaH > 0) { return this.keyAreaH; }
    return Math.max(0, (this.keyboardHeightVp - 24) / 4);
  }

  private keyShadow(row: number, col: number): ShadowOptions {
    const filled = this.isActive(row, col) || this.keyChromeEnabled;
    return filled
      ? { radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 }
      : { radius: 0, color: 'transparent', offsetY: 0 };
  }

  private handleTouch(e: TouchEvent, row: number, col: number, key: FlickKeyDef): void {
    if (e.type === TouchType.Down) {
      this.h().hapticTick();
      const t = e.touches[0];
      this.touchStartX = t.x;
      this.touchStartY = t.y;
      this.activeRow = row;
      this.activeCol = col;
      this.flickDir = 'center';
    } else if (e.type === TouchType.Move) {
      if (this.isActive(row, col) && e.touches.length > 0) {
        const t = e.touches[0];
        this.flickDir = this.dirFrom(t.x - this.touchStartX, t.y - this.touchStartY);
      }
    } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      if (this.isActive(row, col)) {
        const tc = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
        const dir = this.dirFrom(tc.x - this.touchStartX, tc.y - this.touchStartY);
        this.insert(getCharByDir(key, dir));
      }
      this.activeRow = -1;
      this.activeCol = -1;
      this.flickDir = 'center';
    }
  }

  build() {
    Column({ space: 4 }) {
      ForEach([0, 1, 2, 3], (rowIdx: number) => {
        Row({ space: 4 }) {
          this.LeftKey(rowIdx)
          ForEach([0, 1, 2], (colIdx: number) => {
            this.KeyCell(rowIdx, colIdx, NUM_FLICK_GRID[rowIdx][colIdx])
          }, (colIdx: number) => String(colIdx))
          this.RightKey(rowIdx)
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .layoutWeight(1)
      }, (rowIdx: number) => String(rowIdx))
    }
    .width('100%')
    .height('100%')
    .padding({ bottom: 8, top: 4 })
  }

  @Builder KeyCell(rowIdx: number, colIdx: number, key: FlickKeyDef) {
    Stack() {
      if (this.isLandscape) {
        // 横向きは行が低いので方向のヒントを出さず、中央の文字だけ。
        Text(this.centerLabel(rowIdx, colIdx, key))
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(NUM_CENTER_LANDSCAPE_FONT_SIZE)
          .fontWeight(FontWeight.Medium)
          .textAlign(TextAlign.Center)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .width('100%').height('100%')
          .backgroundColor(this.keyBg(rowIdx, colIdx))
          .borderRadius(8).shadow(this.keyShadow(rowIdx, colIdx))
      } else {
        // 上・中・下の3帯。各帯は必ず中央揃えのコンテナで包む。
        Column({ space: 0 }) {
          Row() {
            Text(key.up ?? ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(10)
              .fontColor(this.hintColor(rowIdx, colIdx, 'up'))
              .textAlign(TextAlign.Center)
          }
          .width('100%').height('26%')
          .justifyContent(FlexAlign.Center)
          .alignItems(VerticalAlign.Center)

          Row({ space: 0 }) {
            Text(key.left ?? ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(10)
              .fontColor(this.hintColor(rowIdx, colIdx, 'left'))
              .width('25%').textAlign(TextAlign.Center)

            Text(this.centerLabel(rowIdx, colIdx, key))
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(this.isActive(rowIdx, colIdx) ? NUM_CENTER_ACTIVE_FONT_SIZE : NUM_CENTER_FONT_SIZE)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .fontWeight(FontWeight.Medium)
              .width('50%').textAlign(TextAlign.Center)

            Text(key.right ?? ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(10)
              .fontColor(this.hintColor(rowIdx, colIdx, 'right'))
              .width('25%').textAlign(TextAlign.Center)
          }
          .width('100%').height('44%')
          .alignItems(VerticalAlign.Center)

          Row() {
            Text(key.down ?? ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(10)
              .fontColor(this.hintColor(rowIdx, colIdx, 'down'))
              .textAlign(TextAlign.Center)
          }
          .width('100%').height('30%')
          .justifyContent(FlexAlign.Center)
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .height('100%')
        .backgroundColor(this.keyBg(rowIdx, colIdx))
        .borderRadius(8)
        .shadow(this.keyShadow(rowIdx, colIdx))
      }

      // 打っている方向の辺に出す三角 (中央なら出さない)
      if (this.isActive(rowIdx, colIdx) && this.flickDir !== 'center') {
        Path()
          .commands(flickTriangleCommands(this.flickDir, this.keyW(), this.keyH(), 8))
          .width(this.keyW())
          .height(this.keyH())
          .fill(kbTheme(this.isDark).accent)
          .opacity(0.55)
      }

      // 打とうとしている文字を、キーの真上の中央に丸く表示する (薄い黒の枠付き)
      if (this.isActive(rowIdx, colIdx)) {
        Column() {
          Text(getCharByDir(key, this.flickDir))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(Math.round(flickGlyphSize(this.flickKeyWidthVp, this.keyboardHeightVp) * 1.4))
            .fontColor(kbTheme(this.isDark).textPrimary)
            .fontWeight(FontWeight.Medium)
            .textAlign(TextAlign.Center)
            .maxLines(1)
        }
        .width(BUBBLE_VP)
        .height(BUBBLE_VP)
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
        .borderRadius(BUBBLE_VP / 2)
        .backgroundColor(kbTheme(this.isDark).keyBg)
        .border({ width: 1, color: '#33000000' })
        .align(Alignment.Top)
        .translate({ y: -(BUBBLE_VP + 6) })
      }
    }
    .width(this.flickKeyWidthVp)
    .height('100%')
    .onAreaChange((_oldArea: ESObject, newArea: ESObject) => {
      this.keyAreaW = Number(newArea.width);
      this.keyAreaH = Number(newArea.height);
    })
    .onTouch((e: TouchEvent) => { this.handleTouch(e, rowIdx, colIdx, key); })
  }

  @Builder LeftKey(rowIdx: number) {
    if (rowIdx === 0) {
      // フリックキーボードの左上枠と統一 -- 取消は押すたびに直近削除した文字を
      // 1つずつ戻す(消し過ぎた分を押した回数だけ復元する順次アンドゥ)。
      Text('取消')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14).textAlign(TextAlign.Center)
        .width(this.flickSideKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onClick(() => {
          this.h().undoLastDeletion();
        })
    } else if (rowIdx === 1) {
      Row() {
        SymbolGlyph($r('sys.symbol.chevron_left'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).textPrimary])
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) { this.startCurRepeat('left'); }
        else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
      })
    } else if (rowIdx === 2) {
      Text('記号')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14).textAlign(TextAlign.Center)
        .width(this.flickSideKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onClick(() => { AppStorage.setOrCreate('keyboardSubMode', 'symbol'); })
    } else {
      // '+' は5キーの左フリックで既に入力できるため、この枠は日本語入力に戻る
      // 「あA」キーに差し替える(数字パッドへは「あA」キーのサイクルからのみ入る
      // ため、抜けるときは常に日本語(ひらがな)へ戻す -- 入る前のinputModeが何で
      // あってもループが一貫した状態に着地するようにする)。
      Text() {
        Span('あ').fontColor(this.numericPadEnabled ? kbTheme(this.isDark).textSecondary : kbTheme(this.isDark).textPrimary)
        Span('a').fontColor(kbTheme(this.isDark).textSecondary)
        Span(this.numericPadEnabled ? '1' : '').fontColor(kbTheme(this.isDark).textPrimary)
      }
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(MODE_KEY_FONT_SIZE).textAlign(TextAlign.Center)
      .width(this.flickKeyWidthVp).height('100%')
      .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
      .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onClick(() => {
        AppStorage.setOrCreate('inputMode', InputMode.HIRAGANA);
        AppStorage.setOrCreate('keyboardSubMode', 'kana');
      })
    }
  }

  @Builder RightKey(rowIdx: number) {
    if (rowIdx === 0) {
      Row() {
        SymbolGlyph($r('sys.symbol.delete_left'))
          .fontSize(BACKSPACE_FONT_SIZE)
          .fontColor([kbTheme(this.isDark).textPrimary])
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.startBsRepeat();
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.stopBsRepeat();
        }
      })
    } else if (rowIdx === 1) {
      Row() {
        SymbolGlyph($r('sys.symbol.chevron_right'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).textPrimary])
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) { this.startCurRepeat('right'); }
        else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
      })
    } else if (rowIdx === 2) {
      Text('space')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12).textAlign(TextAlign.Center)
        .width(this.flickSideKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onClick(() => { this.h().handleSpaceKey(); })
    } else {
      Row() {
        SymbolGlyph($r('sys.symbol.arrow_right'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).onAccent])
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).accent)
      .borderRadius(8).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      // 長押しは、アプリの指定 (送信など) に関係なく改行
      .gesture(LongPressGesture({ duration: 400 }).onAction(() => { this.h().handleEnterLongPress(); }))
      .onClick(() => { this.h().handleEnterKey(); })
    }
  }
}

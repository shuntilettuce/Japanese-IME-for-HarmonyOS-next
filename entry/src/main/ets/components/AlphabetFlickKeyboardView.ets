import { InputHandler, InputMode } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';

type FlickDir = 'center' | 'up' | 'right' | 'down' | 'left';

interface FlickKeyDef {
  center: string;
  up?: string;
  right?: string;
  down?: string;
  left?: string;
  special?: string;
}

function getChar(key: FlickKeyDef, dir: FlickDir): string {
  if (dir === 'up' && key.up !== undefined) { return key.up; }
  if (dir === 'right' && key.right !== undefined) { return key.right; }
  if (dir === 'down' && key.down !== undefined) { return key.down; }
  if (dir === 'left' && key.left !== undefined) { return key.left; }
  return key.center;
}

const ALPHA_GRID: FlickKeyDef[][] = [
  [
    { center: '@', left: '-', up: '_', right: '/', down: '1', special: 'symbol' },
    { center: 'a', left: 'b', up: 'c', down: '2' },
    { center: 'd', left: 'e', up: 'f', down: '3' },
  ],
  [
    { center: 'g', left: 'h', up: 'i', down: '4' },
    { center: 'j', left: 'k', up: 'l', down: '5' },
    { center: 'm', left: 'n', up: 'o', down: '6' },
  ],
  [
    { center: 'p', left: 'q', up: 'r', right: 's', down: '7' },
    { center: 't', left: 'u', up: 'v', down: '8' },
    { center: 'w', left: 'x', up: 'y', right: 'z', down: '9' },
  ],
  [
    { center: 'a', special: 'case' },
    { center: "'", left: '"', up: ':', right: ';', down: '0' },
    { center: '.', left: ',', up: '?', down: '!' },
  ],
];

const CELL_RADIUS = 8;

@Component
export struct AlphabetFlickKeyboardView {
  @Prop inputMode: string = InputMode.HIRAGANA;
  @Prop isLandscape: boolean = false;
  @StorageLink('flickKeyWidthVp') flickKeyWidthVp: number = 72;
  @StorageLink('inputState') inputState: string = 'IDLE';
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('keyChromeEnabled') keyChromeEnabled: boolean = true;
  // 数字パッドが有効だと「あA」キーは 日本語→アルファベット→数字 の3段階に
  // なるので、ラベルも切り替わる順に並べる (あA1 / A1あ / 1あA)。
  // 半角の A/1 は ひらがな と字形の大きさが厳密には揃わないが、
  // 全角にすると見た目が野暮ったくなるため半角のままにしている。
  @StorageLink('numericPadEnabled') numericPadEnabled: boolean = false;

  @State activeRow: number = -1;
  @State activeCol: number = -1;
  @State touchStartX: number = 0;
  @State touchStartY: number = 0;
  @State flickDir: FlickDir = 'center';
  // '' | 'right' | 'left' -- which one-handed hint popup is showing.
  @State oneHandHint: string = '';

  private readonly THRESHOLD: number = 20;

  private bsDelayTimer: number = -1;
  private bsRepeatTimer: number = -1;
  private curDelayTimer: number = -1;
  private curRepeatTimer: number = -1;
  private curAccelTimer: number = -1;
  private ohHintTimer: number = -1;

  @State lastChar: string = '';

  private h(): InputHandler { return InputHandler.getInstance(); }

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

  private keyBg(row: number, col: number, key: FlickKeyDef): ResourceColor {
    if (key.special === 'case') { return kbTheme(this.isDark).specialBg; }
    if (this.isActive(row, col)) { return kbTheme(this.isDark).accentSoft; }
    return this.keyChromeEnabled ? kbTheme(this.isDark).keyBg : 'transparent';
  }

  private keyShadow(row: number, col: number, key: FlickKeyDef): ShadowOptions {
    const filled = key.special === 'case' || this.isActive(row, col) || this.keyChromeEnabled;
    return filled
      ? { radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 }
      : { radius: 0, color: 'transparent', offsetY: 0 };
  }

  private centerLabel(row: number, col: number, key: FlickKeyDef): string {
    if (key.special === 'case') { return this.caseLabel(); }
    if (this.isActive(row, col)) { return getChar(key, this.flickDir); }
    return key.center;
  }

  private caseLabel(): string { return 'a/A'; }

  private handleTouch(e: TouchEvent, row: number, col: number, key: FlickKeyDef): void {
    if (key.special === 'case') {
      if (e.type === TouchType.Up) {
        if (this.lastChar && /^[a-z]$/.test(this.lastChar)) {
          this.h().handleBackspace();
          this.h().handleKeyPress(this.lastChar.toUpperCase());   // 入力中の文字の最後の英字を大文字に (入力欄ではなく入力中の文字へ)
          this.lastChar = this.lastChar.toUpperCase();
        }
      }
      return;
    }
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
        this.onCommit(key, dir);
      }
      this.activeRow = -1;
      this.activeCol = -1;
      this.flickDir = 'center';
    }
  }

  private onCommit(key: FlickKeyDef, dir: FlickDir): void {
    if (key.special === 'case') { return; }
    let ch = getChar(key, dir);
    if (/^[a-zA-Z]$/.test(ch)) { ch = ch.toLowerCase(); }
    // 英数字も入力中の文字として組み立てて変換する (handleKeyPress が appendRaw へ。英文は空白でそのまま確定)
    this.h().handleKeyPress(ch);
    this.lastChar = /^[a-z]$/.test(ch) ? ch : '';
  }

  // Themed content for the one-handed hint popup so it reads as part of the
  // keyboard: theme surface (dark bg+white text in dark mode, light bg+dark
  // text in light mode) with the JP font, not the default system tooltip.
  @Builder OneHandHintPopup() {
    Text(this.oneHandHint === 'left' ? '左手モード' : '右手モード')
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(14)
      .fontWeight(FontWeight.Medium)
      .fontColor(kbTheme(this.isDark).textPrimary)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
  }

  build() {
    Stack() {
      Column({ space: 4 }) {
        ForEach([0, 1, 2, 3], (rowIdx: number) => {
          Row({ space: 4 }) {
            this.LeftKey(rowIdx)
            ForEach([0, 1, 2], (colIdx: number) => {
              this.KeyCell(rowIdx, colIdx, ALPHA_GRID[rowIdx][colIdx])
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
    .width('100%')
    .height('100%')
  }

  @Builder LeftKey(rowIdx: number) {
    if (rowIdx === 0) {
      Text('取消')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14).textAlign(TextAlign.Center)
        .width(this.flickKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onClick(() => {
          this.h().hapticTick();
          this.h().undoLastDeletion();
          this.lastChar = '';
        })
    } else if (rowIdx === 1) {
      Row() {
        SymbolGlyph($r('sys.symbol.chevron_left'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).textPrimary])
      }
      .width(this.flickKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.h().hapticTick();
            this.lastChar = '';
            this.startCurRepeat('left');
          }
          else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
        })
    } else if (rowIdx === 2) {
      Text('記号')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14).textAlign(TextAlign.Center)
        .width(this.flickKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        // Long-press to reveal the 左手モード hint, then flick up to switch.
        .bindPopup(this.oneHandHint === 'left', {
          builder: this.OneHandHintPopup,
          placement: Placement.Top,
          enableArrow: true,
          popupColor: kbTheme(this.isDark).specialBg,
          backgroundBlurStyle: BlurStyle.NONE,
          radius: 20,
          mask: false,
        })
        .gesture(
          GestureGroup(GestureMode.Sequence,
            LongPressGesture({ duration: 300 })
              .onAction(() => {
                this.oneHandHint = 'left';
                this.ohHintTimer = setTimeout(() => { this.oneHandHint = ''; }, 1500);
              }),
            PanGesture({ direction: PanDirection.Up, distance: 20 })
              .onActionStart(() => {
                this.h().toggleOneHanded('left');
                clearTimeout(this.ohHintTimer);
                this.oneHandHint = '';
              })
          )
        )
        .onClick(() => {
          this.h().hapticTick();
          AppStorage.setOrCreate('keyboardSubMode', 'symbol');
        })
    } else {
      Text(this.numericPadEnabled ? 'A1あ' : 'あA')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12).textAlign(TextAlign.Center)
        .width(this.flickKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onClick(() => {
          this.h().hapticTick();
          this.h().cycleInputMode();   // 入力中の文字は確定しない (数字パッドで続けて打てるように)
          this.lastChar = '';
        })
    }
  }

  @Builder RightKey(rowIdx: number) {
    if (rowIdx === 0) {
      Row() {
        SymbolGlyph($r('sys.symbol.delete_left'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).textPrimary])
      }
      .width(this.flickKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.h().hapticTick();
            this.lastChar = '';
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
      .width(this.flickKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.h().hapticTick();
            this.lastChar = '';
            this.startCurRepeat('right');
          }
          else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
        })
    } else if (rowIdx === 2) {
      Text(this.inputState === 'COMPOSING' ? '文節' : this.inputState === 'BUNSETSU' ? '変換' : '空白')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(this.inputState === 'IDLE' ? 12 : 14)
        .textAlign(TextAlign.Center)
        .width(this.flickKeyWidthVp).height('100%')
        .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        // Long-press to reveal the 右手モード hint, then flick up to switch.
        .bindPopup(this.oneHandHint === 'right', {
          builder: this.OneHandHintPopup,
          placement: Placement.Top,
          enableArrow: true,
          popupColor: kbTheme(this.isDark).specialBg,
          backgroundBlurStyle: BlurStyle.NONE,
          radius: 20,
          mask: false,
        })
        .gesture(
          GestureGroup(GestureMode.Sequence,
            LongPressGesture({ duration: 300 })
              .onAction(() => {
                this.oneHandHint = 'right';
                this.ohHintTimer = setTimeout(() => { this.oneHandHint = ''; }, 1500);
              }),
            PanGesture({ direction: PanDirection.Up, distance: 20 })
              .onActionStart(() => {
                this.h().toggleOneHanded('right');
                clearTimeout(this.ohHintTimer);
                this.oneHandHint = '';
              })
          )
        )
        .onClick(() => {
          this.h().hapticTick();
          const st = AppStorage.get<string>('inputState') ?? 'IDLE';
          if (st === 'COMPOSING') {
            this.h().handleBunsetsu();
          } else {
            this.h().handleSpaceKey();
          }
          this.lastChar = '';
        })
    } else {
      Row() {
        SymbolGlyph($r('sys.symbol.arrow_right'))
          .fontSize(16)
          .fontColor([kbTheme(this.isDark).onAccent])
      }
      .width(this.flickKeyWidthVp).height('100%')
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .backgroundColor(kbTheme(this.isDark).accent)
      .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
        // 長押しは、アプリの指定 (送信など) に関係なく改行
        .gesture(LongPressGesture({ duration: 400 }).onAction(() => {
          this.h().hapticTick();
          this.h().handleEnterLongPress();
          this.lastChar = '';
        }))
        .onClick(() => {
          this.h().hapticTick();
          const st = AppStorage.get<string>('inputState') ?? 'IDLE';
          if (st === 'BUNSETSU') {
            this.h().commitBunsetsu();
          } else if (st === 'SELECTING') {
            this.h().commitTopCandidate();
          } else if (st === 'COMPOSING') {
            this.h().commitRaw();
          } else {
            this.h().handleEnterKey();
          }
          this.lastChar = '';
        })
    }
  }

  @Builder KeyCell(rowIdx: number, colIdx: number, key: FlickKeyDef) {
    if (this.isLandscape) {
      Text(this.centerLabel(rowIdx, colIdx, key))
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(this.isActive(rowIdx, colIdx) ? 18 : 14)
        .fontColor(key.special === 'case' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
        .fontWeight(FontWeight.Medium)
        .textAlign(TextAlign.Center)
        .width(this.flickKeyWidthVp)
        .height('100%')
        .backgroundColor(this.keyBg(rowIdx, colIdx, key))
        .borderRadius(CELL_RADIUS)
        .shadow(this.keyShadow(rowIdx, colIdx, key))
        .onTouch((e: TouchEvent) => {
          this.handleTouch(e, rowIdx, colIdx, key);
        })
    } else {
      // 上・中・下の3帯。各帯は必ず中央揃えのコンテナで包む
      // (FlickKeyboardView.KeyCell と同じ理由 ── Text に height だけ与えると
      //  帯の中で上寄りになり、文字がずれて見える)。
      Column({ space: 0 }) {
        Row() {
          Text(key.up !== undefined ? key.up : ' ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(this.hintColor(rowIdx, colIdx, 'up'))
            .textAlign(TextAlign.Center)
        }
        .width('100%').height('26%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)

        Row({ space: 0 }) {
          Text(key.left !== undefined ? key.left : ' ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(this.hintColor(rowIdx, colIdx, 'left'))
            .width('25%').textAlign(TextAlign.Center)

          Text(this.centerLabel(rowIdx, colIdx, key))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(this.isActive(rowIdx, colIdx) ? 22 : 18)
            .fontColor(key.special === 'case' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
            .fontWeight(FontWeight.Medium)
            .width('50%').textAlign(TextAlign.Center)

          Text(key.right !== undefined ? key.right : ' ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(this.hintColor(rowIdx, colIdx, 'right'))
            .width('25%').textAlign(TextAlign.Center)
        }
        .width('100%').height('44%')
        .alignItems(VerticalAlign.Center)

        Row() {
          Text(key.down !== undefined ? key.down : ' ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(this.hintColor(rowIdx, colIdx, 'down'))
            .textAlign(TextAlign.Center)
        }
        .width('100%').height('30%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
      }
      .width(this.flickKeyWidthVp)
      .height('100%')
      .backgroundColor(this.keyBg(rowIdx, colIdx, key))
      .borderRadius(CELL_RADIUS)
      .shadow(this.keyShadow(rowIdx, colIdx, key))
      .onTouch((e: TouchEvent) => {
        this.handleTouch(e, rowIdx, colIdx, key);
      })
    }
  }
}

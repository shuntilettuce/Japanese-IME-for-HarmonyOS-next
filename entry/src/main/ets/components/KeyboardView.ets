import { InputHandler, InputMode, InputState } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';

const ROWS: string[][] = [
  ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'],
  // '-' always inserts ー (the long-vowel mark) via ROMAJI_TABLE — romaji kana
  // input has no other reachable key for it, since it isn't part of any
  // romaji consonant sequence. Tacked onto the middle row rather than the
  // number row so it still shares the same 10-key width as row 0. Only used
  // for kana input (see ROW1_ALPHANUMERIC below for the plain-alphabet row,
  // where a trailing '-' has no such purpose and is just clutter).
  ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-'],
  ['z', 'x', 'c', 'v', 'b', 'n', 'm'],
];

// Same middle row as ROWS[1] but without the kana-only ー shortcut key, used
// when inputMode is ALPHANUMERIC (direct alphabet input, not romaji-to-kana).
const ROW1_ALPHANUMERIC: string[] = ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'];

const NUMBER_ROW: string[] = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0'];

// Key corner radius (design tokens: "regular keys 8px").
const KEY_RADIUS = 8;

@Component
export struct KeyboardView {
  @Prop inputMode: string = InputMode.HIRAGANA;
  @Prop isLandscape: boolean = false;
  @State isShift: boolean = false;
  @State isCapsLock: boolean = false;
  // '' | 'right' | 'left' -- which one-handed hint popup is showing (armed by a
  // long-press on the space / shift key; an up-flick while it shows commits).
  @State oneHandHint: string = '';
  // Key widths pre-computed from screen density in KeyboardController
  @StorageLink('keyWidthVp') keyWidthVp: number = 36;
  @StorageLink('specialKeyWidthVp') specialKeyWidthVp: number = 44;
  @StorageLink('spaceKeyWidthVp') spaceKeyWidthVp: number = 160;
  @StorageProp('inputState') qwertyState: string = InputState.IDLE;     // 空白キーの表示 (入力中は「文節」・文節の中は「変換」)
  @StorageProp('composingText') qwertyComposing: string = '';
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('keyChromeEnabled') keyChromeEnabled: boolean = true;
  // 数字パッドが有効だと「あA」キーは 日本語→アルファベット→数字 の3段階に
  // なるので、ラベルも切り替わる順に並べる (あA1 / A1あ / 1あA)。
  // 半角の A/1 は ひらがな と字形の大きさが厳密には揃わないが、
  // 全角にすると見た目が野暮ったくなるため半角のままにしている。
  @StorageLink('numericPadEnabled') numericPadEnabled: boolean = false;

  private getHandler(): InputHandler {
    return InputHandler.getInstance();
  }

  private bsDelayTimer: number = -1;
  private bsRepeatTimer: number = -1;
  private curDelayTimer: number = -1;
  private curRepeatTimer: number = -1;
  private curAccelTimer: number = -1;
  private ohHintTimer: number = -1;

  private startBsRepeat(): void {
    this.getHandler().handleBackspace();
    this.bsDelayTimer = setTimeout(() => {
      this.bsRepeatTimer = setInterval(() => {
        this.getHandler().handleBackspace();
      }, 100);
    }, 500);
  }

  private stopBsRepeat(): void {
    clearTimeout(this.bsDelayTimer);
    clearInterval(this.bsRepeatTimer);
    this.bsDelayTimer = -1;
    this.bsRepeatTimer = -1;
  }

  // ◄/► の長押しで連続して動かす (フリック・数字パッドと同じ間隔。~1秒押し続けると速くなる)
  private startCurRepeat(dir: string): void {
    const move = (): void => {
      if (dir === 'left') { this.getHandler().handleCursorLeft(); }
      else { this.getHandler().handleCursorRight(); }
    };
    this.getHandler().hapticTick();
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
        if (!this.isLandscape) {
          Row({ space: 4 }) {
            ForEach(NUMBER_ROW, (key: string) => {
              this.RegularKey(key, this.keyWidthVp)
            }, (key: string) => key)
          }
          .width('100%')
          .justifyContent(FlexAlign.Center)
          .layoutWeight(1)
        }

        ForEach([0, 1], (rowIdx: number) => {
          Row({ space: 4 }) {
            ForEach(rowIdx === 1 && this.inputMode === InputMode.ALPHANUMERIC ? ROW1_ALPHANUMERIC : ROWS[rowIdx], (key: string) => {
              this.RegularKey(key, this.keyWidthVp)
            }, (key: string) => key)
          }
          .width('100%')
          .justifyContent(FlexAlign.Center)
          .layoutWeight(1)
        }, (rowIdx: number) => String(rowIdx))

        Row({ space: 4 }) {
          this.ShiftKey()
          ForEach(ROWS[2], (key: string) => {
            this.RegularKey(key, this.keyWidthVp)
          }, (key: string) => key)
          this.BackspaceKey()
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .layoutWeight(1)

        Row({ space: 4 }) {
          this.ModeKey()
          this.FuncKey(this.inputMode === InputMode.ALPHANUMERIC ? ',' : '、', () => {
            const ch = this.inputMode === InputMode.ALPHANUMERIC ? ',' : '、';
            const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
            if (state === InputState.IDLE) { this.getHandler().insertText(ch); }
            else { this.getHandler().handleDirectKana(ch); }
          })
          this.SpaceKey()
          this.CaretKey('left')
          this.CaretKey('right')
          this.FuncKey(this.inputMode === InputMode.ALPHANUMERIC ? '.' : '。', () => {
            const ch = this.inputMode === InputMode.ALPHANUMERIC ? '.' : '。';
            const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
            if (state === InputState.IDLE) { this.getHandler().insertText(ch); }
            else { this.getHandler().handleDirectKana(ch); }
          })
          this.EnterKey()
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .layoutWeight(1)
      }
      .width('100%')
      .height('100%')
      .padding({ bottom: 8, top: 4, left: 2, right: 2 })

    }
    .width('100%')
    .height('100%')
  }

  @Builder RegularKey(value: string, width: number) {
    // Read isShift/isCapsLock directly here (not via a by-value Builder param)
    // so the label re-renders when the shift state changes.
    Text((this.isShift || this.isCapsLock) ? value.toUpperCase() : value)
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(18)
      .fontWeight(FontWeight.Medium)
      .fontColor(kbTheme(this.isDark).textPrimary)
      .textAlign(TextAlign.Center)
      .width(width)
      .height('100%')
      .backgroundColor(this.keyChromeEnabled ? kbTheme(this.isDark).keyBg : 'transparent')
      .borderRadius(KEY_RADIUS)
      .shadow(this.keyChromeEnabled
        ? { radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 }
        : { radius: 0, color: 'transparent', offsetY: 0 })
      .onClick(() => {
        this.getHandler().hapticTick();
        const key = (this.isShift || this.isCapsLock) ? value.toUpperCase() : value;
        this.getHandler().handleKeyPress(key);
        if (this.isShift && !this.isCapsLock) {
          this.isShift = false;
        }
      })
  }

  @Builder ShiftKey() {
    Row() {
      SymbolGlyph(this.isCapsLock
        ? $r('sys.symbol.switch_majuscule_fill')
        : $r('sys.symbol.switch_majuscule'))
        .fontSize(18)
        .fontColor([this.isShift || this.isCapsLock
          ? kbTheme(this.isDark).onAccent
          : kbTheme(this.isDark).textPrimary])
    }
    .width(this.specialKeyWidthVp)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .backgroundColor(this.isShift || this.isCapsLock ? kbTheme(this.isDark).accent : kbTheme(this.isDark).specialBg)
    .borderRadius(KEY_RADIUS)
    .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      // QWERTY has no 記号 key, so the shift/caps key doubles as the left-lean
      // control: long-press to reveal the 左手モード hint, then flick up to
      // switch. A plain tap still cycles shift/caps via onClick.
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
              this.getHandler().toggleOneHanded('left');
              clearTimeout(this.ohHintTimer);
              this.oneHandHint = '';
            })
        )
      )
      .onClick(() => {
        this.getHandler().hapticTick();
        if (this.isCapsLock) {
          this.isCapsLock = false;
          this.isShift = false;
        } else if (this.isShift) {
          this.isCapsLock = true;
        } else {
          this.isShift = true;
        }
      })
  }

  @Builder BackspaceKey() {
    Row() {
      SymbolGlyph($r('sys.symbol.delete_left'))
        .fontSize(18)
        .fontColor([kbTheme(this.isDark).textPrimary])
    }
    .width(this.specialKeyWidthVp)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .backgroundColor(kbTheme(this.isDark).specialBg)
    .borderRadius(KEY_RADIUS)
    .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) { this.getHandler().hapticTick(); }
        if (event.type === TouchType.Down) {
          this.startBsRepeat();
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.stopBsRepeat();
        }
      })
  }

  // カーソルを左右に動かすキー (変換中は変換する範囲を縮める・伸ばす。文節を選んでいるときは文節を移る)
  @Builder CaretKey(dir: string) {
    Row() {
      SymbolGlyph($r(dir === 'left' ? 'sys.symbol.chevron_left' : 'sys.symbol.chevron_right'))
        .fontSize(16)
        .fontColor([kbTheme(this.isDark).textPrimary])
    }
    .width(this.keyWidthVp)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .backgroundColor(kbTheme(this.isDark).specialBg)
    .borderRadius(KEY_RADIUS)
    .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
    .onTouch((event: TouchEvent) => {
      if (event.type === TouchType.Down) { this.startCurRepeat(dir); }
      else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
    })
  }

  // 文節キーは空白キーにまとめた (ユーザー 2026-11-05): 入力中は「文節」、文節の中は「変換」(今の文節の候補を回す)。幅は文節キーの分も
  @Builder SpaceKey() {
    Text(this.qwertyState === InputState.BUNSETSU ? '変換'
      : (this.qwertyState === InputState.COMPOSING && this.qwertyComposing.length >= 2) ? '文節'
      : (this.inputMode === InputMode.HIRAGANA ? 'かな' : 'space'))
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(16)
      .fontColor(kbTheme(this.isDark).textPrimary)
      .textAlign(TextAlign.Center)
      .width(this.spaceKeyWidthVp + this.specialKeyWidthVp + 4)
      .height('100%')
      .backgroundColor(kbTheme(this.isDark).keyBg)
      .borderRadius(KEY_RADIUS)
      .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      // Long-press the space key to reveal the 右手モード hint, then flick up
      // while it shows to switch. Gating on the long-press stops a stray flick
      // from misfiring; a plain tap still inserts a space via onClick.
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
              this.getHandler().toggleOneHanded('right');
              clearTimeout(this.ohHintTimer);
              this.oneHandHint = '';
            })
        )
      )
      .onClick(() => {
        this.getHandler().hapticTick();
        this.getHandler().handleSpaceKey();
      })
  }

  @Builder ModeKey() {
    Text(this.numericPadEnabled
      ? (this.inputMode === InputMode.HIRAGANA ? 'あA1' : 'A1あ')
      : (this.inputMode === InputMode.HIRAGANA ? 'あA' : 'Aあ'))
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(14)
      .textAlign(TextAlign.Center)
      .width(this.specialKeyWidthVp)
      .height('100%')
      .backgroundColor(kbTheme(this.isDark).specialBg)
      .fontColor(kbTheme(this.isDark).textPrimary)
      .borderRadius(KEY_RADIUS)
      .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      .onClick(() => {
        this.getHandler().hapticTick();
        // 入力中の文字は確定しない (英字・数字も続けて足して一緒に変換する: ぱちんこ + CR -> パチンコCR)
        this.getHandler().cycleInputMode();
      })
  }

  @Builder FuncKey(label: string, action: () => void) {
    Text(label)
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(18)
      .fontWeight(FontWeight.Medium)
      .fontColor(kbTheme(this.isDark).textPrimary)
      .textAlign(TextAlign.Center)
      .width(this.keyWidthVp)
      .height('100%')
      .backgroundColor(this.keyChromeEnabled ? kbTheme(this.isDark).keyBg : 'transparent')
      .borderRadius(KEY_RADIUS)
      .shadow(this.keyChromeEnabled
        ? { radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 }
        : { radius: 0, color: 'transparent', offsetY: 0 })
      .onClick(action)
  }

  @Builder EnterKey() {
    Row() {
      SymbolGlyph($r('sys.symbol.arrow_right'))
        .fontSize(18)
        .fontColor([kbTheme(this.isDark).onAccent])
    }
    .width(this.specialKeyWidthVp)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .backgroundColor(kbTheme(this.isDark).accent)
    .borderRadius(KEY_RADIUS)
    .shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      // 長押しは、アプリの指定 (送信など) に関係なく改行
      .gesture(LongPressGesture({ duration: 400 }).onAction(() => {
        this.getHandler().hapticTick();
        this.getHandler().handleEnterLongPress();
      }))
      .onClick(() => {
        this.getHandler().hapticTick();
        this.getHandler().handleEnterKey();
      })
  }
}

import { InputHandler, InputMode, InputState } from '../ime/KeyboardController';
import { toKatakana } from '../ime/JapaneseConverter';
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

function getKanaByDir(key: FlickKeyDef, dir: FlickDir): string {
  if (dir === 'up' && key.up !== undefined) { return key.up; }
  if (dir === 'right' && key.right !== undefined) { return key.right; }
  if (dir === 'down' && key.down !== undefined) { return key.down; }
  if (dir === 'left' && key.left !== undefined) { return key.left; }
  return key.center;
}

// 3 center kana columns (displayed in cols 1-3 of the 5-col layout)
const FLICK_GRID: FlickKeyDef[][] = [
  [
    { center: 'あ', up: 'う', right: 'え', down: 'お', left: 'い' },
    { center: 'か', up: 'く', right: 'け', down: 'こ', left: 'き' },
    { center: 'さ', up: 'す', right: 'せ', down: 'そ', left: 'し' },
  ],
  [
    { center: 'た', up: 'つ', right: 'て', down: 'と', left: 'ち' },
    { center: 'な', up: 'ぬ', right: 'ね', down: 'の', left: 'に' },
    { center: 'は', up: 'ふ', right: 'へ', down: 'ほ', left: 'ひ' },
  ],
  [
    { center: 'ま', up: 'む', right: 'め', down: 'も', left: 'み' },
    { center: 'や', up: 'ゆ', left: '（', right: '）', down: 'よ' },
    { center: 'ら', up: 'る', right: 'れ', down: 'ろ', left: 'り' },
  ],
  [
    { center: '゛', up: '゜', down: '小', special: 'dakuten' },
    { center: 'わ', up: 'ん', right: 'ー', down: '～', left: 'を' },
    { center: '、', up: '？', right: '！', down: '…', left: '。', special: 'punct' },
  ],
];

// Flick cell corner radius (design tokens: "flick cells 8px").
const CELL_RADIUS = 8;

// キー間の見た目の隙間を広げるための余白。各キーは「当たり判定用の枠
// (従来どおりのサイズ、これがタップ/フリックを検出する)」+「その内側に
// パディングで一回り小さく描く見た目のキー本体」の二重構造にしてある。
// パディングは枠の内側の余白なので枠自体の width/height は一切変わらず、
// Row の総幅計算・境界ハンドル(boundaryX)の位置・当たり判定の広さは
// 従来と同じまま、見た目の隙間だけが広がる。フリック方向 (dirFrom) は
// タッチ開始位置からの相対デルタ (t.x - touchStartX) だけを見ているので
// 枠の内側でキー本体の位置が動いても判定には影響しない。
const FLICK_H_INSET = 1;
const FLICK_V_INSET = 1;

// フリック中央3列との幅比調整ハンドル。このぶんドラッグすると比率の
// 全域 (0.4〜2.0) を動く。
const RATIO_DRAG_RANGE_VP = 140;
const RATIO_MIN = 0.4;
const RATIO_MAX = 2.0;

@Component
export struct FlickKeyboardView {
  @Prop inputMode: string = InputMode.HIRAGANA;
  @Prop isLandscape: boolean = false;
  // 設定 → レイアウト調整 → このキーボードを見ながら調整、のときだけ true。
  // 左右特殊キー列との境界に、幅比を変えるドラッグハンドルを重ねて出す。
  @Prop adjustMode: boolean = false;
  // 縦横で別の値を持つ (StorageLink はキー名を静的にしか束縛できないため、
  // 両方を保持して isLandscape で読み書き先を切り替える)。
  @StorageLink('flickSideColumnRatioPortrait') sideRatioPortrait: number = 1.0;
  @StorageLink('flickSideColumnRatioLandscape') sideRatioLandscape: number = 1.0;
  // get アクセサではなくメソッドにしてある -- ArkTS の @Component 内では
  // @StorageLink を組み合わせた get プロパティが実行時に正しく評価されず
  // (undefined を返す)、ドラッグ量の計算が NaN に化けて「サイズ調整が
  // 効かなくなる」不具合の原因になった (LayoutAdjustBar.heightScale と同じ)。
  private sideRatio(): number {
    return this.isLandscape ? this.sideRatioLandscape : this.sideRatioPortrait;
  }
  private dragStartRatio: number = 1.0;
  @StorageLink('flickKeyWidthVp') flickKeyWidthVp: number = 72;
  // 境界ハンドルの位置計算用 (ColumnHandle 参照)。行の幅そのものには
  // 使わない (それは各キーの .width() 側で既に反映済み)。
  @StorageLink('keyboardContentVp') keyboardContentVp: number = 360;
  // 左右端(取消/◄/記号…と ►/スペース…)の特殊キー1列の幅。中央3列の
  // flickKeyWidthVp とは独立 -- 設定 → レイアウト調整の比率スライダーで
  // 両者の比を変えられる (既定は同じ幅で今までどおり)。
  @StorageLink('flickSideKeyWidthVp') flickSideKeyWidthVp: number = 72;
  // 上下左右の小さいかなヒント(十字表示)を出すか。フリック自体は
  // dirFrom() がタッチのジェスチャで判定しており、このヒントを消しても
  // 機能は一切変わらない (isLandscape のときの表示と同じ仕組みを流用)。
  @StorageLink('flickHintsEnabled') flickHintsEnabled: boolean = true;
  @StorageLink('inputState') inputState: string = 'IDLE';
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('keyChromeEnabled') keyChromeEnabled: boolean = true;
  // 数字パッドが有効だと「あA」キーは 日本語→アルファベット→数字 の3段階に
  // なるので、ラベルも切り替わる順に並べる (あA1 / A1あ / 1あA)。
  // 半角の A/1 は ひらがな と字形の大きさが厳密には揃わないが、
  // 全角にすると見た目が野暮ったくなるため半角のままにしている。
  @StorageLink('numericPadEnabled') numericPadEnabled: boolean = false;
  // フローティングでキーボード自体が小さくリサイズされると、行の実高さも
  // それに応じて縮む (このコンポーネント自身は '100%' で親に委ねている
  // だけなので、行高そのものはここでは持っていない)。フリックヒント
  // (上下左右の小さいかな) やキー中央の文字が固定フォントサイズのままだと
  // 縮んだ行の中で溢れて重なって見える (「潰れる」の実体)。
  // keyboardHeightVp は常に vp のキーボード全体高さ (KeyboardController.
  // initPanel/doResizePanelNow 参照) なので、これを基準にフォントサイズを
  // 比例縮小する。既定の 260vp 前後を基準 (scale=1.0) とする。
  @StorageLink('keyboardHeightVp') keyboardHeightVp: number = 260;

  private hintScale(): number {
    return Math.max(0.5, Math.min(1.15, this.keyboardHeightVp / 260));
  }

  @State activeRow: number = -1;
  @State activeCol: number = -1;
  @State touchStartX: number = 0;
  @State touchStartY: number = 0;
  @State flickDir: FlickDir = 'center';
  // '' | 'right' | 'left' -- which one-handed hint popup is showing (armed by a
  // long-press on the space / 記号 key; an up-flick while it shows commits).
  @State oneHandHint: string = '';

  private readonly THRESHOLD: number = 20;

  private bsDelayTimer: number = -1;
  private bsRepeatTimer: number = -1;
  private curDelayTimer: number = -1;
  private curRepeatTimer: number = -1;
  private curAccelTimer: number = -1;
  private ohHintTimer: number = -1;

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

  // Hold-to-repeat for the ◄/► caret keys, like backspace, and accelerating
  // once held past ~1s so a long trip across the line doesn't take forever.
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

  private toMode(kana: string): string {
    if (this.inputMode === InputMode.KATAKANA && /[ぁ-ん]/.test(kana)) {
      return toKatakana(kana);
    }
    return kana;
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

  private centerLabel(row: number, col: number, key: FlickKeyDef): string {
    if (key.special === 'dakuten') { return key.center; }
    if (key.special === 'punct') {
      const raw = this.isActive(row, col) ? getKanaByDir(key, this.flickDir) : key.center;
      return this.inputMode === InputMode.ALPHANUMERIC ? this.jpToAsciiPunct(raw) : raw;
    }
    if (this.isActive(row, col)) { return this.toMode(getKanaByDir(key, this.flickDir)); }
    return this.toMode(key.center);
  }

  private keyBg(row: number, col: number, key: FlickKeyDef): ResourceColor {
    if (key.special === 'dakuten') { return kbTheme(this.isDark).specialBg; }
    if (this.isActive(row, col)) { return kbTheme(this.isDark).accentSoft; }
    return this.keyChromeEnabled ? kbTheme(this.isDark).keyBg : 'transparent';
  }

  private keyShadow(row: number, col: number, key: FlickKeyDef): ShadowOptions {
    const filled = key.special === 'dakuten' || this.isActive(row, col) || this.keyChromeEnabled;
    return filled
      ? { radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 }
      : { radius: 0, color: 'transparent', offsetY: 0 };
  }

  private jpToAsciiPunct(s: string): string {
    const m: Record<string, string> = {
      '、': ',', '。': '.', '！': '!', '？': '?', '…': '...',
    };
    return m[s] ?? s;
  }

  private onCommit(key: FlickKeyDef, dir: FlickDir): void {
    if (key.special === 'dakuten') {
      this.h().handleVariantCycle();
      return;
    }
    if (key.special === 'punct') {
      const raw = getKanaByDir(key, dir);
      const punct = this.inputMode === InputMode.ALPHANUMERIC ? this.jpToAsciiPunct(raw) : raw;
      const state = AppStorage.get<string>('inputState') ?? InputState.IDLE;
      if (state === InputState.IDLE) {
        this.h().insertText(punct);
      } else {
        this.h().handleDirectKana(punct);
      }
      return;
    }
    const kana = this.toMode(getKanaByDir(key, dir));
    this.h().handleDirectKana(kana);
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
        const dx = tc.x - this.touchStartX;
        const dy = tc.y - this.touchStartY;
        const dir = this.dirFrom(dx, dy);
        // 入力ログ (デバッグビルドのみ): どのキーをどちらへ、指がどれだけ動いたか。
        // 方向判定のしきい値 (THRESHOLD) 付近の打ち間違いを後から見るため。
        this.h().noteFlick(key.center, getKanaByDir(key, dir), dir, dx, dy);
        this.onCommit(key, dir);
      }
      this.activeRow = -1;
      this.activeCol = -1;
      this.flickDir = 'center';
    }
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

  // 左右特殊キー列と中央3列の境界の、幅比調整ハンドル。行ごとに1本ずつ
  // 生やして4本を縦に並べる (Column の gap:4 を挟んで並ぶだけなので、
  // 見た目にはほぼ1本の連続した線に見える)。left/right どちらの境界かで
  // ドラッグの向きと符号を変える -- 掴んだ側の列を、引っ張った方向へ
  // 実際に伸び縮みさせる感覚に合わせてある。
  // 行の幅に子として差し込むと、その分だけ行が伸びて画面端をはみ出す
  // (実際にこれで発生した不具合)。通常のキー配置には一切手を触れず、
  // その上に絶対位置で重ねるだけにしてある -- レイアウトへの影響ゼロ。
  // 行は Row({space:4}) が [左特殊][中央×3][右特殊] を中央寄せで並べる
  // ので、位置はそこから逆算する。
  private rowStartX(): number {
    const gap = 4;
    const contentTotal = 2 * this.flickSideKeyWidthVp + 3 * this.flickKeyWidthVp + 4 * gap;
    return Math.max(0, (this.keyboardContentVp - contentTotal) / 2);
  }

  private boundaryX(side: 'left' | 'right'): number {
    const gap = 4;
    const start = this.rowStartX();
    if (side === 'left') {
      // 左特殊キーと中央1列目の間 (gap1 の中央)。
      return start + this.flickSideKeyWidthVp + gap / 2;
    }
    // 中央3列目と右特殊キーの間 (gap4 の中央)。
    return start + this.flickSideKeyWidthVp + 3 * this.flickKeyWidthVp + 3 * gap + gap / 2;
  }

  @Builder ColumnHandle(side: 'left' | 'right') {
    Row() {
      Row().width(3).height('50%').borderRadius(1.5).backgroundColor(kbTheme(this.isDark).accent)
    }
    .width(18)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .position({ x: this.boundaryX(side) - 9, y: 0 })
    .gesture(
      PanGesture({ direction: PanDirection.Horizontal })
        .onActionStart(() => {
          this.dragStartRatio = this.sideRatio();
        })
        .onActionUpdate((event: GestureEvent) => {
          // 境界線そのものを掴んで動かす感覚に合わせる: 指を動かした方向に
          // 境界を押すと、押した側の特殊キー列がその分だけ伸びる。
          // 左の境界を右へ押す(offsetX>0)→左の特殊キー列が伸びる(比率↑)。
          // 右の境界を右へ押す(offsetX>0)→右の特殊キー列は縮む(比率↓)。
          // 以前は符号が逆で、ドラッグ方向と伸縮が逆になっていた
          // (「操作が左右反転してる」の実体)。
          const dir = side === 'left' ? 1 : -1;
          const delta = dir * event.offsetX / RATIO_DRAG_RANGE_VP * (RATIO_MAX - RATIO_MIN);
          const next = Math.min(RATIO_MAX, Math.max(RATIO_MIN, this.dragStartRatio + delta));
          this.h().applyFlickSideRatioLive(next, this.isLandscape);
        })
        .onActionEnd(() => {
          this.h().saveFlickSideRatio(this.sideRatio(), this.isLandscape);
        })
    )
  }

  build() {
    Stack() {
      Column({ space: 4 }) {
        ForEach([0, 1, 2, 3], (rowIdx: number) => {
          Row({ space: 4 }) {
            this.LeftKey(rowIdx)
            ForEach([0, 1, 2], (colIdx: number) => {
              this.KeyCell(rowIdx, colIdx, FLICK_GRID[rowIdx][colIdx])
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
      // レイアウト調整中はキー自体を無効化する (文字が打ててしまうと
      // 誤操作のもと)。境界ハンドルはこの Column の外の兄弟要素なので
      // ここで無効化しても影響しない -- ドラッグは adjustMode 中こそ
      // 使いたい操作なので、キーだけを選んで止める。
      .hitTestBehavior(this.adjustMode ? HitTestMode.None : HitTestMode.Default)

      // 行の幅には一切影響しない絶対位置オーバーレイ。全4行の高さぶんを
      // 1本の縦線として重ねる (行ごとに4本出すより「1本の境界線」に見える)。
      if (this.adjustMode) {
        this.ColumnHandle('left')
        this.ColumnHandle('right')
      }
    }
    .width('100%')
    .height('100%')
  }

  // Left column keys per row
  @Builder LeftKey(rowIdx: number) {
    if (rowIdx === 0) {
      // 数字パッドの入口は「あA」キー側のサイクルに一本化した(setNumericPadEnabled
      // 参照)。この枠は常に取消を表示する。押すたびに直近削除した文字を1つずつ
      // 戻す(消し過ぎた分を押した回数だけ復元する順次アンドゥ)。
      Row() {
        Text('取消')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14).textAlign(TextAlign.Center)
          .width('100%').height('100%')
          .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
          .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onClick(() => {
        this.h().hapticTick();
        this.h().undoLastDeletion();
      })
    } else if (rowIdx === 1) {
      Row() {
        Row() {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(16)
            .fontColor([kbTheme(this.isDark).textPrimary])
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).specialBg)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.h().hapticTick();
          this.startCurRepeat('left');
        }
        else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
      })
    } else if (rowIdx === 2) {
      Row() {
        Text('記号')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14).textAlign(TextAlign.Center)
          .width('100%').height('100%')
          .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
          .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
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
      Row() {
        Text(this.numericPadEnabled
          ? (this.inputMode === InputMode.HIRAGANA ? 'あA1' : 'A1あ')
          : (this.inputMode === InputMode.HIRAGANA ? 'あA' : 'Aあ'))
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(12).textAlign(TextAlign.Center)
          .width('100%').height('100%')
          .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
          .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onClick(() => {
        this.h().hapticTick();
        // 入力中の文字は確定しない (英字・数字も続けて足して一緒に変換する: ぱちんこ + CR -> パチンコCR)
        this.h().cycleInputMode();
      })
    }
  }

  // Right column keys per row
  @Builder RightKey(rowIdx: number) {
    if (rowIdx === 0) {
      Row() {
        Row() {
          SymbolGlyph($r('sys.symbol.delete_left'))
            .fontSize(16)
            .fontColor([kbTheme(this.isDark).textPrimary])
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).specialBg)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.h().hapticTick();
          this.startBsRepeat();
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.stopBsRepeat();
        }
      })
    } else if (rowIdx === 1) {
      Row() {
        Row() {
          SymbolGlyph($r('sys.symbol.chevron_right'))
            .fontSize(16)
            .fontColor([kbTheme(this.isDark).textPrimary])
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).specialBg)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.h().hapticTick();
          this.startCurRepeat('right');
        }
        else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.stopCurRepeat(); }
      })
    } else if (rowIdx === 2) {
      Row() {
        Text(this.inputState === 'COMPOSING' ? '文節' : this.inputState === 'BUNSETSU' ? '変換' : '空白')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(this.inputState === 'IDLE' ? 12 : 14)
          .textAlign(TextAlign.Center)
          .width('100%').height('100%')
          .backgroundColor(kbTheme(this.isDark).specialBg).fontColor(kbTheme(this.isDark).textPrimary)
          .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
        // Long-press the space key to reveal the 右手モード hint, then flick up
        // while it shows to switch. Gating on the long-press stops a stray flick
        // from misfiring. A plain tap still inserts a space via onClick.
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
        })
    } else {
      Row() {
        Row() {
          SymbolGlyph($r('sys.symbol.arrow_right'))
            .fontSize(16)
            .fontColor([kbTheme(this.isDark).onAccent])
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).accent)
        .borderRadius(CELL_RADIUS).shadow({ radius: 2, color: kbTheme(this.isDark).shadowColor, offsetY: 1 })
      }
      .width(this.flickSideKeyWidthVp).height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      // 長押しは、アプリの指定 (送信など) に関係なく改行
      .gesture(LongPressGesture({ duration: 400 }).onAction(() => {
        this.h().hapticTick();
        this.h().handleEnterLongPress();
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
        })
    }
  }

  @Builder KeyCell(rowIdx: number, colIdx: number, key: FlickKeyDef) {
    if (this.isLandscape || !this.flickHintsEnabled) {
      // Landscape rows are short on height; drop the flick-direction hints and
      // just show the center glyph. Flicking itself is unaffected — dirFrom()
      // reads the touch gesture, not this label.
      Row() {
        Text(this.centerLabel(rowIdx, colIdx, key))
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(this.isActive(rowIdx, colIdx)
            ? Math.max(11, Math.round(17 * this.hintScale()))
            : Math.max(9, Math.round(14 * this.hintScale())))
          .fontColor(key.special === 'dakuten' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
          .fontWeight(FontWeight.Medium)
          .textAlign(TextAlign.Center)
          .width('100%')
          .height('100%')
          .backgroundColor(this.keyBg(rowIdx, colIdx, key))
          .borderRadius(CELL_RADIUS)
          .shadow(this.keyShadow(rowIdx, colIdx, key))
      }
      .width(this.flickKeyWidthVp)
      .height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onTouch((e: TouchEvent) => {
        this.handleTouch(e, rowIdx, colIdx, key);
      })
    } else {
      // 上・中・下の3帯。各帯は必ず中央揃えのコンテナで包む。
      // Text に height だけ与えると帯の中で上寄りに描かれ、中心文字との間隔が
      // 上下で食い違って「文字がずれている」ように見える。
      Row() {
        Column({ space: 0 }) {
          Row() {
            Text(key.up !== undefined ? this.toMode(key.up) : ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(Math.max(7, Math.round(11 * this.hintScale())))
              .fontColor(this.hintColor(rowIdx, colIdx, 'up'))
              .textAlign(TextAlign.Center)
          }
          .width('100%').height('26%')
          .justifyContent(FlexAlign.Center)
          .alignItems(VerticalAlign.Center)

          Row({ space: 0 }) {
            Text(key.left !== undefined ? this.toMode(key.left) : ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(Math.max(7, Math.round(11 * this.hintScale())))
              .fontColor(this.hintColor(rowIdx, colIdx, 'left'))
              .width('25%').textAlign(TextAlign.Center)

            Text(this.centerLabel(rowIdx, colIdx, key))
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(this.isActive(rowIdx, colIdx)
                ? Math.max(14, Math.round(22 * this.hintScale()))
                : Math.max(12, Math.round(18 * this.hintScale())))
              .fontColor(key.special === 'dakuten' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
              .fontWeight(FontWeight.Medium)
              .width('50%').textAlign(TextAlign.Center)

            Text(key.right !== undefined ? this.toMode(key.right) : ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(Math.max(7, Math.round(11 * this.hintScale())))
              .fontColor(this.hintColor(rowIdx, colIdx, 'right'))
              .width('25%').textAlign(TextAlign.Center)
          }
          .width('100%').height('44%')
          .alignItems(VerticalAlign.Center)

          Row() {
            Text(key.down !== undefined ? this.toMode(key.down) : ' ')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(Math.max(7, Math.round(11 * this.hintScale())))
              .fontColor(this.hintColor(rowIdx, colIdx, 'down'))
              .textAlign(TextAlign.Center)
          }
          .width('100%').height('30%')
          .justifyContent(FlexAlign.Center)
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .height('100%')
        .backgroundColor(this.keyBg(rowIdx, colIdx, key))
        .borderRadius(CELL_RADIUS)
        .shadow(this.keyShadow(rowIdx, colIdx, key))
      }
      .width(this.flickKeyWidthVp)
      .height('100%')
      .padding({ left: FLICK_H_INSET, right: FLICK_H_INSET, top: FLICK_V_INSET, bottom: FLICK_V_INSET })
      .onTouch((e: TouchEvent) => {
        this.handleTouch(e, rowIdx, colIdx, key);
      })
    }
  }
}

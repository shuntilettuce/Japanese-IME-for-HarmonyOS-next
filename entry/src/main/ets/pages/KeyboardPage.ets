import inputMethodEngine from '@ohos.inputMethodEngine';
import { promptAction } from '@kit.ArkUI';
import { InputHandler, InputState, InputMode, applyKeyboardWidthsForVp } from '../ime/KeyboardController';
import { CandidateBar } from '../components/CandidateBar';
import { PredictionBar } from '../components/PredictionBar';
import { KeyboardView } from '../components/KeyboardView';
import { FlickKeyboardView } from '../components/FlickKeyboardView';
import { AlphabetFlickKeyboardView } from '../components/AlphabetFlickKeyboardView';
import { SymbolView } from '../components/SymbolView';
import { EmojiView } from '../components/EmojiView';
import { NumericPadView } from '../components/NumericPadView';
import { BunsetsuBar } from '../components/BunsetsuBar';
import { ExpandedCandidateView } from '../components/ExpandedCandidateView';
import { SettingsBar } from '../components/SettingsBar';
import { LayoutAdjustBar } from '../components/LayoutAdjustBar';
import { DictEditBar } from '../components/DictEditBar';
import { SettingsStore, KeyboardSettings } from '../ime/SettingsStore';
import { InputLog, DEBUG_INPUT_LOG } from '../ime/InputLog';
import { kbTheme } from '../components/Theme';
import { JP_FONT_FAMILY, registerJpFont } from '../components/JpFont';

let _inputStartRegistered = false;

// フローティング四隅のリサイズハンドル (L字ブラケット) の一辺の長さと太さ。
// FloatingHandles 参照。
const HANDLE_LEN = 22;
const HANDLE_THICK = 4;
// フローティング中、実際のOSパネルウィンドウをカード本体より一回り大きく
// 取っておく余白 (vp)。KeyboardController.FLOATING_MARGIN_VP と同じ値。
// これが無いと、窓の外側にはみ出すL字ハンドルや影がそのまま切れて見えて
// いた (「Lのハンドル見切れてる」の実体)。
const FLOATING_MARGIN_VP = 16;
// 縁取りの色。テーマのアクセント色は主張が強すぎたので薄い水色に。
const FLOATING_BORDER_COLOR = '#8ED2FF';
// 上際の移動ハンドル(ピル)と四隅のL字ハンドルが同じ場所で同時に掴めて
// しまわないよう、ピルの当たり判定を左右それぞれこの分だけ内側へ避ける
// (「上の方のサイズ調整L字とハンドルが同時につかめてしまう」)。
const HANDLE_CORNER_CLEARANCE_VP = HANDLE_LEN + 6;

// 入力中のかな表示帯・候補バーの高さは、以前は縦横で2択の固定vp値だった
// (フローティングでキーボード自体の高さが自由に変わるようになったのに、
// これだけ固定だと小さいフローティング窓で相対的に大きすぎたり、大きい
// 窓で小さすぎたりする)。キーボード全体の高さ(vp)に対する比率で計算し、
// 極端なサイズでも読める/掴める範囲に収まるよう上下限だけクランプする。
const STRIP_HEIGHT_RATIO = 0.10;
const STRIP_MIN_VP = 14;
const STRIP_MAX_VP = 34;
// 縦向きはキーボード全体が横向きよりずっと背が高いので、同じ比率だと
// 候補バーが絶対値として大きくなりすぎた(「縦長にすると候補バー長すぎる」)。
// 上限を下げて、縦向きでも候補チップが収まる範囲でできるだけ小さく留める。
const CANDIDATE_BAR_HEIGHT_RATIO = 0.13;
const CANDIDATE_BAR_MIN_VP = 24;
const CANDIDATE_BAR_MAX_VP = 44;

@Entry
@Component
struct KeyboardPage {
  @StorageLink('inputState') inputState: string = InputState.IDLE;
  @StorageLink('inputMode') inputMode: string = InputMode.HIRAGANA;
  @StorageLink('composingText') composingText: string = '';
  @StorageLink('pendingRomaji') pendingRomaji: string = '';
  @StorageLink('showComposingPanel') showComposingPanel: boolean = true;
  @StorageLink('candidates') candidates: string[] = [];
  @StorageLink('predictions') predictions: string[] = [];
  @StorageLink('selectedCandidateIndex') selectedIndex: number = 0;
  @StorageLink('isLandscape') isLandscape: boolean = false;
  @StorageLink('kanaKeyboardLayout') kanaKeyboardLayout: string = 'QWERTY';
  @StorageLink('alphabetKeyboardLayout') alphabetKeyboardLayout: string = 'QWERTY';
  @StorageLink('zhMode') zhMode: boolean = false;
  // 記号・絵文字・設定などのパネル切替を入力ログに残す (デバッグビルドのみ)。
  @StorageLink('keyboardSubMode') @Watch('onSubModeChanged') keyboardSubMode: string = 'kana';

  private onSubModeChanged(): void {
    InputHandler.getInstance().noteSubMode(this.keyboardSubMode);
  }
  // 括弧の閉じ待ち (KeyboardController の insertText 参照)。空でなければ
  // 「この文字がまだ出ていない」ことを候補バー行に出す。
  @StorageLink('pendingBracketClose') pendingBracketClose: string = '';
  // 'manual' | 'auto'。バッジの文言を「► で閉じられる」案内にするか、
  // 従来の「次の確定で出る」案内にするかの切り替え (SettingsStore 参照)。
  @StorageLink('bracketCloseMode') bracketCloseMode: string = 'pair';
  // 辞書の登録欄をキーボードの上に出しているか (DictEditBar 参照)。
  @StorageLink('dictEditMode') dictEditMode: boolean = false;
  @StorageLink('bunsetsuTexts') bunsetsuTexts: string[] = [];
  @StorageLink('bunsetsuCurrent') bunsetsuCurrent: number = 0;
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('clipboardPreview') clipboardPreview: string = '';
  @StorageLink('oneHandedMode') oneHandedMode: boolean = false;
  @StorageLink('oneHandedSide') oneHandedSide: string = 'right';
  @StorageLink('candidateExpanded') candidateExpanded: boolean = false;
  @StorageLink('convEnd') convEnd: number = 0;
  // フローティングキーボード。ONのときだけ上際ハンドル・四隅リサイズ
  // ハンドルを重ねて出す (FloatingHandles 参照)。
  @StorageLink('floatingMode') floatingMode: boolean = false;
  @StorageLink('floatingXFrac') floatingXFrac: number = 0.2;
  @StorageLink('floatingYFrac') floatingYFrac: number = 0.12;
  @StorageLink('floatingWFrac') floatingWFrac: number = 0.6;
  @StorageLink('floatingHFrac') floatingHFrac: number = 0.35;
  // フローティング中のカード(見た目のキーボード本体)の実サイズ(vp)。
  // KeyboardController.doResizePanelNow のフローティング分岐が
  // applyKeyboardWidthsForVp/keyboardHeight にそのまま書いている値と同じ。
  // ハンドルを窓の余白(FLOATING_MARGIN_VP)ぶん正確な位置に置くために使う。
  @StorageLink('keyboardContentVp') floatingCardWVp: number = 360;
  @StorageLink('keyboardHeight') floatingCardHVp: number = 260;
  // キーボード本体の左右寄せ (horizontalOffsetVp 参照)。実際に寄せられる
  // 余白は「画面幅(生) − 幅の上限を適用した後の幅」なので、その両方を見る。
  @StorageLink('keyboardHorizontalOffset') keyboardHorizontalOffset: number = 0;
  @StorageLink('keyboardRawContentVp') keyboardRawContentVp: number = 360;
  @StorageLink('keyboardContentVp') keyboardContentVp: number = 360;
  // 常に vp のキーボード全体の高さ (FIXED時は 'keyboardHeight' が px な
  // ので使えない -- KeyboardController.doResizePanelNow/initPanel 参照)。
  // 入力中のかな表示帯・候補バーの高さを固定 vp ではなく比率で決めるのに使う
  // (「サイズ固定なのやめて、ちゃんと比率で計算しよう」)。
  @StorageLink('keyboardHeightVp') keyboardHeightVp: number = 260;
  // ドラッグ/リサイズ開始時点の値。開始時の値に対して足し引きする
  // (LayoutAdjustBar 等、このコードベースの他のドラッグ系ハンドルと同じ形)。
  private dragStartXFrac: number = 0;
  private dragStartYFrac: number = 0;
  private resizeStartXFrac: number = 0;
  private resizeStartYFrac: number = 0;
  private resizeStartWFrac: number = 0;
  private resizeStartHFrac: number = 0;
  // フローティングのドラッグ量は event.offsetX/Y (要素=このウィンドウ基準の
  // 累積オフセット) ではなく、画面絶対座標 (TouchObject.displayX/Y) の
  // 差分で計算する。ドラッグに応じてウィンドウ自体を動かしているため、
  // 要素基準の座標系はドラッグ中に動いてしまい、「原点が指を追いかけて
  // 引っ張り合う」形になって「元の位置に戻ろうとする力」のように感じられる
  // 不具合の原因だった。画面絶対座標ならウィンドウが動いても基準がブレない。
  // GestureEvent 自体の fingerList は PanGesture での可用性が不確かなので
  // (APIコメント上は LongPress/Tap 用と書かれている)、同じ要素に張った
  // .onTouch() から TouchObject.displayX/Y (screenX/Y の後継、API10〜) を
  // 直接拾って使う -- こちらは種類を問わず確実に取れる。
  private lastTouchDisplayX: number = 0;
  private lastTouchDisplayY: number = 0;
  private dragStartDisplayX: number = 0;
  private dragStartDisplayY: number = 0;
  private resizeStartDisplayX: number = 0;
  private resizeStartDisplayY: number = 0;

  // かな表示帯・候補バーの高さ (vp)。STRIP_HEIGHT_RATIO/CANDIDATE_BAR_
  // HEIGHT_RATIO 参照。
  // 入力中の文字列の帯の幅 (vp)。長い入力を帯に収めるための字間の計算に使う
  @State composingStripWidthVp: number = 0;

  // 入力中の文字列が帯からはみ出すとき、文字の横幅を縮めて全体を収める (字は重ならない)。
  // 文字列の本来の幅 (vp) と、横の縮小率 (1 = 縮めない) を返す。縮めるのは 0.4 倍まで
  private composingFit(text: string): number[] {
    const fs: number = this.isLandscape ? 12 : 16;
    const avail = this.composingStripWidthVp - 24;   // 左右の余白
    if (avail <= 0 || text.length < 2) {
      return [0, 1];
    }
    const px = this.getUIContext().getMeasureUtils().measureText({
      textContent: text, fontSize: fs, fontWeight: FontWeight.Medium, fontFamily: JP_FONT_FAMILY
    });
    const w = this.getUIContext().px2vp(px);
    if (w <= avail) {
      return [w, 1];
    }
    return [w, Math.max(0.4, avail / w)];
  }

  private stripHeightVp(): number {
    return Math.max(STRIP_MIN_VP, Math.min(STRIP_MAX_VP, this.keyboardHeightVp * STRIP_HEIGHT_RATIO));
  }

  private candidateBarHeightVp(): number {
    return Math.max(CANDIDATE_BAR_MIN_VP,
      Math.min(CANDIDATE_BAR_MAX_VP, this.keyboardHeightVp * CANDIDATE_BAR_HEIGHT_RATIO));
  }

  // 設定ボタンの長押し→上スライドでフローティング化。長押し中はヒントを
  // 出す (Shift/Spaceキーの片手モード切替と同じ「長押し→フリックで切替」
  // パターン)。
  @State floatingHint: boolean = false;
  private floatingHintTimer: number = -1;
  // 上際ハンドルのドラッグ中、「今離すと通常のドッキング状態に戻る」位置
  // にどれだけ近いか (0〜1)。0 なら非表示、1 で離すと実際にドックへ戻る。
  // カードが画面下端からはみ出し始めた瞬間から連続的に増えていくので、
  // DockPreview の帯の高さに直接使う (「下から青い範囲が這い出てくる」)。
  @State dockPreviewProgress: number = 0;

  aboutToAppear(): void {
    // The IME extension is its own process/UI root, so it needs its own
    // registration -- and it has to happen from inside the UI, not from
    // KeyboardController.onCreate (see registerJpFont's comment).
    registerJpFont(this.getUIContext());
    // 入力ログの初期化の保険 (デバッグビルドのみ)。本来は
    // InputHandler.setupInputLog が setContext から立てるが、そこまで届かない
    // 実機状態が出たことがあるので、描画される以上必ず走るここからも立てる。
    // 既に立っていれば何もしない。順番はアプリ単位 -> モジュール単位で揃える。
    if (DEBUG_INPUT_LOG) {
      let panelAppDir = '';
      try {
        panelAppDir = (getContext(this) as ESObject).getApplicationContext().filesDir as string;
      } catch (_e) { panelAppDir = ''; }
      InputLog.ensureFromPanel([panelAppDir, getContext(this).filesDir]);
    }
    // aboutToAppear runs every time this panel's content is (re)constructed —
    // a more reliable reload point than onPageShow, which per ArkUI docs is a
    // Navigation/Router page-lifecycle hook and may never fire for content
    // hosted in an InputMethodExtensionAbility's Panel (a window, not a routed
    // page). Kept unguarded (unlike the inputStart listener below) since
    // rereading a small preferences store is cheap.
    this.reloadSettingsIntoStorage();

    if (_inputStartRegistered) { return; }
    _inputStartRegistered = true;
    inputMethodEngine.getInputMethodAbility().on('inputStart', (
      kbCtrl: inputMethodEngine.KeyboardController,
      client: inputMethodEngine.InputClient
    ) => {
      InputHandler.getInstance().onInputStart(kbCtrl, client);
    });
  }

  onBackPress(): boolean {
    InputHandler.getInstance().hideKeyboard();
    return true;
  }

  // onInputStart only fires for a genuinely new input session — switching to
  // the main app to change the keyboard layout and back doesn't necessarily
  // trigger one if the panel itself just gets hidden/reshown. Kept alongside
  // aboutToAppear's call in case this panel *is* managed as a routed page on
  // some devices/OS versions after all.
  onPageShow(): void {
    console.info('shuntiIME KeyboardPage.onPageShow fired');
    this.reloadSettingsIntoStorage();
  }

  private reloadSettingsIntoStorage(): void {
    SettingsStore.load(getContext(this), true).then((s: KeyboardSettings) => {
      console.info('shuntiIME KeyboardPage settings reload applied', JSON.stringify(s));
      AppStorage.setOrCreate('kanaKeyboardLayout', s.kanaKeyboardLayout);
      AppStorage.setOrCreate('alphabetKeyboardLayout', s.alphabetKeyboardLayout);
      AppStorage.setOrCreate('oneHandedMode', s.oneHandedMode);
      AppStorage.setOrCreate('oneHandedSide', s.oneHandedSide);
      AppStorage.setOrCreate('numericPadEnabled', s.numericPadEnabled);
      AppStorage.setOrCreate('themeMode', s.themeMode);
      AppStorage.setOrCreate('keyChromeEnabled', s.keyChromeEnabled);
      AppStorage.setOrCreate('flickHintsEnabled', s.flickHintsEnabled);
      AppStorage.setOrCreate('bracketCloseMode', s.bracketCloseMode);
      // かなの表示先は決め打ち (設定は廃止、2026-10-14)。入力欄の対応状況は InputHandler が混ぜる
      InputHandler.getInstance().applyComposingDisplay(false, true);
      InputHandler.getInstance().setThemeMode(s.themeMode);
      // AI の変換を読み込む (読み込み済みなら何もしない)
      InputHandler.getInstance().setConversionEngine(s.conversionEngine);
      // 縦横別の値を両方 AppStorage に入れておき (LayoutAdjustBar/
      // FlickKeyboardView が向き問わず自分の StorageLink で読めるように)、
      // 実リサイズ/幅再計算は今表示中の向きぶんだけ即時に反映する。
      AppStorage.setOrCreate('keyboardHeightScalePortrait', s.keyboardHeightScalePortrait);
      AppStorage.setOrCreate('keyboardHeightScaleLandscape', s.keyboardHeightScaleLandscape);
      AppStorage.setOrCreate('flickSideColumnRatioPortrait', s.flickSideColumnRatioPortrait);
      AppStorage.setOrCreate('flickSideColumnRatioLandscape', s.flickSideColumnRatioLandscape);
      const isLandscape = AppStorage.get<boolean>('isLandscape') ?? false;
      InputHandler.getInstance().applyKeyboardHeightScaleLive(
        isLandscape ? s.keyboardHeightScaleLandscape : s.keyboardHeightScalePortrait, isLandscape);
      InputHandler.getInstance().applyFlickSideRatioLive(
        isLandscape ? s.flickSideColumnRatioLandscape : s.flickSideColumnRatioPortrait, isLandscape);
      InputHandler.getInstance().applyKeyboardWidthScaleLive(s.keyboardWidthScale);
      InputHandler.getInstance().applyKeyboardHorizontalOffsetLive(s.keyboardHorizontalOffset);
      InputHandler.getInstance().syncFloatingFromSettings(s);
    });
  }

  // 実際のキーボード本体を選ぶ部分。通常の入力画面と、レイアウト調整モード
  // (下の layoutAdjust 分岐、LayoutAdjustBar の帯の下にこれをそのまま表示
  // して見ながら調整できるようにする) の両方から呼ぶための @Builder。
  @Builder KeyboardArea() {
    Column() {
      if (this.keyboardSubMode === 'numeric') {
        // 'numeric' と 'layoutAdjust' は同じ keyboardSubMode の別値なので
        // この分岐にいる間は adjustMode になり得ない (無効化は不要)。
        NumericPadView()
      } else if (this.alphabetKeyboardLayout === 'FLICK' && this.inputMode === InputMode.ALPHANUMERIC && !this.zhMode) {
        // レイアウト調整中は文字が打てないようにする -- キーボードは見た目
        // どおりに見せつつ (打とうとして誤操作するより、押しても反応しない
        // ほうが分かりやすい)、タップだけ通す。FlickKeyboardView は境界
        // ハンドルを adjustMode 中も動かしたいので、自分自身でキー部分だけを
        // 無効化する (下のコメント参照)。この2つは境界ハンドルが無いので
        // ここで丸ごと無効化してよい。
        AlphabetFlickKeyboardView({
          inputMode: this.inputMode,
          isLandscape: this.isLandscape
        })
          .hitTestBehavior(this.keyboardSubMode === 'layoutAdjust' ? HitTestMode.None : HitTestMode.Default)
      } else if (this.kanaKeyboardLayout === 'FLICK' && this.inputMode !== InputMode.ALPHANUMERIC) {
        FlickKeyboardView({
          inputMode: this.inputMode,
          isLandscape: this.isLandscape,
          adjustMode: this.keyboardSubMode === 'layoutAdjust'
        })
      } else {
        KeyboardView({
          inputMode: this.inputMode,
          isLandscape: this.isLandscape
        })
          .hitTestBehavior(this.keyboardSubMode === 'layoutAdjust' ? HitTestMode.None : HitTestMode.Default)
      }
    }
    .width('100%')
    .layoutWeight(1)
    .opacity(this.keyboardSubMode === 'layoutAdjust' ? 0.6 : 1)
    .offset({ x: this.horizontalOffsetVp(), y: 0 })
    .scale(this.oneHandedMode ?
      { x: 0.78, y: 0.78, centerX: this.oneHandedSide === 'left' ? '0%' : '100%', centerY: '100%' } :
      { x: 1, y: 1 })
  }

  // 幅の上限を絞ってキーボードが画面より狭くなったぶんの余白を、どちら側へ
  // 寄せるか (keyboardHorizontalOffset: -1=左端 / 0=中央 / +1=右端)。
  // 余白そのものが無ければ 0 -- 画面いっぱいのときに動かすと画面外へ
  // はみ出してしまうため。
  private horizontalOffsetVp(): number {
    const slack = (this.keyboardRawContentVp - this.keyboardContentVp) / 2;
    if (slack <= 0) { return 0; }
    return Math.min(1, Math.max(-1, this.keyboardHorizontalOffset)) * slack;
  }

  // 「設定」ボタンの長押し→上スライドでフローティング化 (ON/OFFの
  // トグル)。Shift/Spaceキーの片手モード切替と同じ「長押しでヒントを出し、
  // そのまま上へフリックすると成立」の形。動作確認中は原因切り分けのため
  // ShiftKey/SpaceKeyの実装と同じくインライン構成にしてあり (下の2箇所)、
  // 共通化はまだしていない。

  // 「設定」ボタン長押し中に出すヒント。bindPopup の Placement.Top は
  // ボタンの上に十分な余白が無いと自動で下にひっくり返ってしまう
  // (この帯自体がパネル最上部近くにあるため、上に出したくても出せない
  // ケースがあった)。OS任せの配置をやめて、パネル最上部に固定で出す
  // 自前のオーバーレイにしてある。
  @Builder FloatingHintBanner() {
    Text(this.floatingMode ? 'フローティング解除' : 'フローティング化')
      .fontFamily(JP_FONT_FAMILY)
      .fontSize(13)
      .fontWeight(FontWeight.Medium)
      .fontColor(kbTheme(this.isDark).onAccent)
      .textAlign(TextAlign.Center)
      .backgroundColor(kbTheme(this.isDark).accent)
      .borderRadius(12)
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .position({ x: '50%', y: 6 })
      .markAnchor({ x: '50%', y: 0 })
      .zIndex(220)
      .hitTestBehavior(HitTestMode.None)
  }

  // 上際ハンドルを画面下へ引っ張っている間、下端から青く薄い帯が
  // 「這い出てくる」ように高さが連続的に伸びる案内。実際のドッキング先
  // (画面の本当の下端) は今のフローティング窓の外側にあり ArkUI
  // コンテンツはウィンドウの外を描けないため、ドッキング先そのものの
  // ゴーストは出せない代わりに、カード自身の下端からこの帯を伸ばして
  // 「下から迫ってきている」感触を出す。dockPreviewProgress (0〜1) が
  // そのまま帯の高さと濃さになり、1 (離すと実際にドックへ戻る) で
  // 案内テキストも表示される。
  @Builder DockPreview() {
    Column() {
      if (this.dockPreviewProgress > 0.55) {
        Text('離すと通常のキーボードに戻ります')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(12)
          .fontWeight(FontWeight.Medium)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .textAlign(TextAlign.Center)
          .width('100%')
          .padding({ top: 8 })
      }
    }
    .width(this.floatingCardWVp)
    .height(Math.max(4, this.dockPreviewProgress * this.floatingCardHVp * 0.7))
    .justifyContent(FlexAlign.Start)
    .backgroundColor(FLOATING_BORDER_COLOR)
    .opacity(0.2 + this.dockPreviewProgress * 0.35)
    .borderRadius({ topLeft: 16, topRight: 16, bottomLeft: 0, bottomRight: 0 })
    .position({ x: FLOATING_MARGIN_VP, y: FLOATING_MARGIN_VP + this.floatingCardHVp })
    .markAnchor({ x: 0, y: '100%' })
    .zIndex(210)
    .hitTestBehavior(HitTestMode.None)
  }

  // フローティング中だけ出す、上際のドラッグハンドルと四隅のリサイズ
  // ハンドル。パネルウィンドウ自体はカード本体より FLOATING_MARGIN_VP
  // ぶん一回り大きく確保してある (KeyboardController.FLOATING_MARGIN_VP)
  // ので、カードの実際の位置 (marginVp, marginVp) 〜
  // (marginVp+cardW, marginVp+cardH) を vp で直接計算してハンドルを置く
  // -- % 指定だとウィンドウ全体に対する割合になってしまい、カードの
  // 実際の角とずれる。通常のキーボード配置には一切影響しない絶対位置
  // オーバーレイ (FlickKeyboardView の境界ハンドルと同じ考え方)。
  @Builder FloatingHandles() {
    // 上際 = ドラッグして移動。フルwidthの帯ではなく、ボトムシートの
    // 「つまみ」のような小さいピル型にして見た目を軽くしてある
    // (「デザイン性が低い」の指摘)。掴みやすさのため当たり判定はカード
    // 幅ぶん確保しつつ、見えているのは中央の細いピルだけ。
    Row() {
      Row().width(32).height(4).borderRadius(2)
        .backgroundColor(kbTheme(this.isDark).textSecondary)
    }
    // 四隅のL字ハンドルとの当たり判定の取り合いを避けるため、左右を
    // HANDLE_CORNER_CLEARANCE_VP ぶん避けた中央部分だけを掴める範囲にする。
    .width(Math.max(40, this.floatingCardWVp - 2 * HANDLE_CORNER_CLEARANCE_VP))
    .height(20)
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .position({ x: FLOATING_MARGIN_VP + HANDLE_CORNER_CLEARANCE_VP, y: FLOATING_MARGIN_VP })
    .zIndex(200)
    .onTouch((e: TouchEvent) => {
      if (e.touches.length > 0) {
        this.lastTouchDisplayX = e.touches[0].displayX;
        this.lastTouchDisplayY = e.touches[0].displayY;
      }
    })
    .gesture(
      PanGesture({ direction: PanDirection.All })
        .onActionStart(() => {
          this.dragStartXFrac = this.floatingXFrac;
          this.dragStartYFrac = this.floatingYFrac;
          this.dragStartDisplayX = this.lastTouchDisplayX;
          this.dragStartDisplayY = this.lastTouchDisplayY;
          InputHandler.getInstance().beginFloatingDrag();
        })
        .onActionUpdate(() => {
          const dx = this.lastTouchDisplayX - this.dragStartDisplayX;
          const dy = this.lastTouchDisplayY - this.dragStartDisplayY;
          InputHandler.getInstance().moveFloatingPanelLive(
            this.dragStartXFrac, this.dragStartYFrac, dx, dy);
          // 「今離すとドックに戻る」ことをその場で分かるように、下から
          // 青い帯が這い出てくるプレビューの高さを更新する。
          this.dockPreviewProgress = InputHandler.getInstance().checkFloatingDockBack(
            this.dragStartYFrac, dy);
        })
        .onActionEnd(() => {
          InputHandler.getInstance().endFloatingDrag();
          this.dockPreviewProgress = 0;
          const dy = this.lastTouchDisplayY - this.dragStartDisplayY;
          // 画面下へ大きくはみ出す位置まで引っ張って離したら、通常の
          // ドッキング状態に戻す (「画面下にはみ出るように動かしたら通常の
          // 状態にもどるようにして」)。それ以外は普通に位置を保存するだけ。
          InputHandler.getInstance().endFloatingMoveDrag(this.dragStartYFrac, dy);
        })
    )

    // 四隅 = ホールドしてドラッグでリサイズ。対角(掴んでいない角)が画面上で
    // 動かないよう、角ごとに x/y も一緒に動かす (resizeFloatingPanelLive 側)。
    // 見た目は塗りつぶし丸ではなく、カメラのフォーカス枠のような丸縁取りの
    // L字ブラケットにしてある (「サイズ調整のハンドルも丸縁取りのL字みたいな
    // 形状に」の指摘)。色もカードの縁取りと揃えて薄い水色に。
    ForEach(['tl', 'tr', 'bl', 'br'], (corner: string) => {
      Stack() {
        // 横棒: 上の角なら上端、下の角なら下端に沿わせる。
        Row().width(HANDLE_LEN).height(HANDLE_THICK).borderRadius(HANDLE_THICK / 2)
          .backgroundColor(FLOATING_BORDER_COLOR)
          .position({ x: 0, y: (corner === 'tl' || corner === 'tr') ? 0 : HANDLE_LEN - HANDLE_THICK })
        // 縦棒: 左の角なら左端、右の角なら右端に沿わせる。
        Row().width(HANDLE_THICK).height(HANDLE_LEN).borderRadius(HANDLE_THICK / 2)
          .backgroundColor(FLOATING_BORDER_COLOR)
          .position({ x: (corner === 'tl' || corner === 'bl') ? 0 : HANDLE_LEN - HANDLE_THICK, y: 0 })
      }
        .width(HANDLE_LEN).height(HANDLE_LEN)
        .position({
          x: (corner === 'tl' || corner === 'bl')
            ? FLOATING_MARGIN_VP - HANDLE_THICK
            : FLOATING_MARGIN_VP + this.floatingCardWVp + HANDLE_THICK,
          y: (corner === 'tl' || corner === 'tr')
            ? FLOATING_MARGIN_VP - HANDLE_THICK
            : FLOATING_MARGIN_VP + this.floatingCardHVp + HANDLE_THICK,
        })
        .markAnchor({
          x: corner === 'tl' || corner === 'bl' ? 0 : '100%',
          y: corner === 'tl' || corner === 'tr' ? 0 : '100%',
        })
        .zIndex(200)
        .onTouch((e: TouchEvent) => {
          if (e.touches.length > 0) {
            this.lastTouchDisplayX = e.touches[0].displayX;
            this.lastTouchDisplayY = e.touches[0].displayY;
          }
        })
        .gesture(
          PanGesture({ direction: PanDirection.All })
            .onActionStart(() => {
              this.resizeStartXFrac = this.floatingXFrac;
              this.resizeStartYFrac = this.floatingYFrac;
              this.resizeStartWFrac = this.floatingWFrac;
              this.resizeStartHFrac = this.floatingHFrac;
              this.resizeStartDisplayX = this.lastTouchDisplayX;
              this.resizeStartDisplayY = this.lastTouchDisplayY;
              InputHandler.getInstance().beginFloatingDrag();
            })
            .onActionUpdate(() => {
              InputHandler.getInstance().resizeFloatingPanelLive(
                corner as 'tl' | 'tr' | 'bl' | 'br',
                this.resizeStartXFrac, this.resizeStartYFrac,
                this.resizeStartWFrac, this.resizeStartHFrac,
                this.lastTouchDisplayX - this.resizeStartDisplayX,
                this.lastTouchDisplayY - this.resizeStartDisplayY);
            })
            .onActionEnd(() => {
              InputHandler.getInstance().endFloatingDrag();
              InputHandler.getInstance().saveFloatingRect();
            })
        )
    }, (corner: string) => corner)
  }

  build() {
    Stack() {
      // 入力欄内表示に非対応の欄でキーボード内に戻ったときの案内 (トースト) は廃止 (ユーザー 2026-10-13「もう消していい」)
      Column() {
        // Symbol/Emoji/Settings views take the full panel (no preview bar).
        // Numeric keeps the preview bar — without it the pad's rows stretch to
        // fill the whole panel height and end up uncomfortably tall.
        if (this.keyboardSubMode === 'symbol') {
          SymbolView()
        } else if (this.keyboardSubMode === 'emoji') {
          EmojiView()
        } else if (this.keyboardSubMode === 'settings') {
          SettingsBar()
        } else if (this.keyboardSubMode === 'layoutAdjust') {
          // 他の設定画面と違ってキーボード全体を占有しない -- 帯だけを
          // 上に出し、実物のキーボードをそのまま下に残して見ながら
          // 調整できるようにする (LayoutAdjustBar のコメント参照)。
          Column() {
            LayoutAdjustBar({ isLandscape: this.isLandscape })
            // 本番はかな表示帯+候補バーの合計ぶんだけヘッダーが占有される
            // (比率計算)。LayoutAdjustBar 自身もヘッダー相当の帯なので、
            // その高さぶんは既に埋め合わせ済み -- ここでダミー帯を
            // 「かな+候補の合計」までまるごと足すと、LayoutAdjustBar の
            // 高さと二重取りになってキー側が本番より狭くなってしまう
            // (「今度は狭くなってしまった」の実体)。LayoutAdjustBar の
            // 高さを差し引いた残りだけをダミー帯として補う。
            Row()
              .width('100%')
              .height(Math.max(0,
                this.stripHeightVp() + this.candidateBarHeightVp() - (this.isLandscape ? 30 : 44)))
              .backgroundColor(kbTheme(this.isDark).previewBg)
            this.KeyboardArea()
          }
          .width('100%')
          .height('100%')
        } else {
          // 辞書の登録欄。キーボードは下にそのまま残す (全面を占有する設定
          // パネル内では打つキーが無く入力できないため、ここに出している)。
          if (this.dictEditMode) {
            DictEditBar()
          }
          // Preview bar
          if (this.inputState === InputState.BUNSETSU) {
            // Two-row preview: segment chips on top, candidates below
            Column() {
              BunsetsuBar({
                segments: this.bunsetsuTexts,
                currentIndex: this.bunsetsuCurrent
              })
              CandidateBar({
                candidates: this.candidates,
                selectedIndex: this.selectedIndex,
                showExpand: true,
                barHeightVp: this.candidateBarHeightVp()
              })
            }
            .width('100%')
            .backgroundColor(kbTheme(this.isDark).previewBg)
            .borderWidth({ bottom: 1 })
            .borderColor(kbTheme(this.isDark).divider)
          } else {
            Column() {
              // Composing kana strip — always visible to prevent layout shifts.
              // Landscape has much less vertical room to spare for the keys below,
              // so this strip and the candidate row underneath both run compact.
              //
              // 入力欄プレビューが効いているときは、この行ごと出さない。
              // かなは入力欄側に出ているので、ここに空の帯を残す意味が無く、
              // その分キーボードを低くできる。行が消えると「設定」の置き場が
              // 無くなるので、候補バーの右端へ移す (下の previewInField 分岐)。
              // ただし変換範囲を ◀▶ で変えている最中だけは、どこまでを変換
              // するかが入力欄側では分からないのでこの行を出す。
              // 提案は、この帯が無い (入力欄にプレビューを出す) ときは候補の帯の中に出す (CandidateBar)。帯を出し入れすると
              // キーボードの高さが変わって打っている最中にキーがずれる
              if (this.showComposingPanel
                  || (this.convEnd > 0 && this.convEnd < this.composingText.length)) {
              Row() {
                Row() {
                  if (this.composingText.length > 0 || this.pendingRomaji.length > 0) {
                    if (this.convEnd > 0 && this.convEnd < this.composingText.length) {
                      Text(this.composingText.slice(0, this.convEnd))
                        .fontFamily(JP_FONT_FAMILY)
                        .fontSize(this.isLandscape ? 12 : 16)
                        .fontWeight(FontWeight.Medium)
                        .fontColor(kbTheme(this.isDark).textPrimary)
                        .maxLines(1)
                        .decoration({ type: TextDecorationType.Underline, color: kbTheme(this.isDark).accent })
                      Text(this.composingText.slice(this.convEnd))
                        .fontFamily(JP_FONT_FAMILY)
                        .fontSize(this.isLandscape ? 12 : 16)
                        .fontColor(kbTheme(this.isDark).textSecondary)
                        .maxLines(1)
                    } else {
                      Text(this.composingText + this.pendingRomaji)
                        .fontFamily(JP_FONT_FAMILY)
                        .fontSize(this.isLandscape ? 12 : 16)
                        .fontWeight(FontWeight.Medium)
                        .fontColor(kbTheme(this.isDark).textPrimary)
                        .maxLines(1)
                        .textOverflow({ overflow: TextOverflow.Clip })
                        .padding({ left: 12, right: 8 })
                        .border((this.composingText + this.pendingRomaji).length > 0 ?
                          { width: { bottom: 2 }, color: kbTheme(this.isDark).accent } :
                          { width: { bottom: 0 } })
                        // はみ出すときは本来の幅で並べてから横だけ縮める (左端を基準に)
                        .width(this.composingFit(this.composingText + this.pendingRomaji)[1] < 1
                          ? this.composingFit(this.composingText + this.pendingRomaji)[0] + 24 : '100%')
                        .layoutWeight(this.composingFit(this.composingText + this.pendingRomaji)[1] < 1 ? 0 : 1)
                        .scale({ x: this.composingFit(this.composingText + this.pendingRomaji)[1], y: 1, centerX: 0, centerY: '50%' })
                    }
                  }
                  Blank()
                }
                .layoutWeight(1)
                .height('100%')
                .alignItems(VerticalAlign.Center)
                .padding({ left: 12 })
                .onAreaChange((_old: Area, now: Area) => { this.composingStripWidthVp = Number(now.width) - 12; })
                .clip(true)

                Row() {
                  SymbolGlyph($r('sys.symbol.gearshape'))
                    .fontSize(this.isLandscape ? 12 : 18)
                    .fontColor([kbTheme(this.isDark).textSecondary])
                }
                .width(this.isLandscape ? 24 : 36)
                .height('100%')
                .justifyContent(FlexAlign.Center)
                .alignItems(VerticalAlign.Center)
                // 見た目のサイズは変えず、当たり判定だけ広げる (帯自体が
                // 低い/狭いことがあるので、パディングを増やすと今度は
                // ここが原因で潰れて見えかねない)。
                .responseRegion({ x: '-20%', y: '-60%', width: '140%', height: '220%' })
                .onClick(() => {
                  AppStorage.setOrCreate('keyboardSubMode', 'settings');
                })
                .gesture(
                  GestureGroup(GestureMode.Sequence,
                    LongPressGesture({ duration: 300 })
                      .onAction(() => {
                        this.floatingHint = true;
                        this.floatingHintTimer = setTimeout(() => { this.floatingHint = false; }, 1500);
                      }),
                    PanGesture({ direction: PanDirection.Up, distance: 20 })
                      .onActionStart(() => {
                        InputHandler.getInstance().toggleFloatingMode();
                        clearTimeout(this.floatingHintTimer);
                        this.floatingHint = false;
                      })
                  )
                )
              }
              .width('100%')
              .height(this.stripHeightVp())
              .backgroundColor(kbTheme(this.isDark).panelBg)
              .alignItems(VerticalAlign.Center)
              }

              // Candidate bar row + layout toggle
              Row() {
                Row() {
                  if (this.candidates.length > 0) {
                    CandidateBar({
                      candidates: this.candidates,
                      selectedIndex: this.selectedIndex,
                      showExpand: true,
                      barHeightVp: this.candidateBarHeightVp()
                    })
                  } else if (this.composingText.length === 0 && this.pendingRomaji.length === 0 &&
                    this.clipboardPreview.length > 0) {
                    // 未貼り付けのクリップボードがある限り、次の語の予測より先に出す。
                    // チップは貼るか、クリップボードの中身が変わるまで消えない
                    // (dismissedClipSig)ので、「まだ貼っていないコピーが1件ある」
                    // 状態と 1:1 に対応する。
                    //
                    // 以前は予測がこの枝より先にあり、確定直後はほぼ必ず予測が
                    // 出るため、貼り付けチップに一度も到達しなかった。
                    //
                    // Gboard-style: centered paste suggestion when nothing is composing,
                    // rather than left-aligned like an actual conversion candidate.
                    Text(this.clipboardPreview)
                      .fontFamily(JP_FONT_FAMILY)
                      .fontSize(this.isLandscape ? 12 : 16)
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                      // Cap how wide the chip can grow instead of letting it stretch to
                      // fill the row — short clips hug their text, long ones truncate
                      // with "…" at the cap instead of a background that runs on past
                      // where the (ellipsized) text actually ends.
                      .constraintSize({ maxWidth: '90%' })
                      .padding(this.isLandscape ?
                        { left: 8, right: 8, top: 2, bottom: 2 } :
                        { left: 12, right: 12, top: 6, bottom: 6 })
                      .borderRadius(this.isLandscape ? 11 : 17)
                      .backgroundColor(kbTheme(this.isDark).candidateBg)
                      .fontColor(kbTheme(this.isDark).textSecondary)
                      .onClick(() => {
                        const result = InputHandler.getInstance().pasteClipboard();
                        if (result === 'extendAction') {
                          // Fire-and-forget request to the focused field's own
                          // paste handler (see pasteClipboard's comment) --
                          // there's no reliable signal for whether it actually
                          // had anything to paste, so this can only say the
                          // request went out, not that it definitely worked.
                          promptAction.showToast({
                            message: '貼り付け操作を送信しました（アプリによっては反映されないことがあります）',
                          });
                        } else if (result === 'none') {
                          promptAction.showToast({
                            message: '画像・ファイルはキーボードから直接貼り付けできません。貼り付け先アプリ自体の貼り付け機能をご利用ください',
                          });
                        }
                      })
                  } else if (this.composingText.length === 0 && this.pendingRomaji.length === 0 &&
                    this.predictions.length > 0) {
                    // Idle right after a commit: offer the likely next words to tap.
                    PredictionBar({ predictions: this.predictions, barHeightVp: this.candidateBarHeightVp() })
                  }
                }
                .layoutWeight(1)
                .height(this.candidateBarHeightVp())
                .justifyContent(FlexAlign.Center)
                .alignItems(VerticalAlign.Center)

                // 括弧を開いた直後、閉じがまだ出ていないことを示すバッジ。
                // 見た目に何も変化が無いと「閉じ括弧が出ない」と誤解される
                // (実際に不具合報告があった)。手動モードでは閉じるのが利用者の
                // 操作待ちになるので、文言でも ► を案内し、バッジ自体も
                // 「ここを押せば今すぐ閉じる」ボタンとして使えるようにしてある
                // (► キーが無いレイアウトからの逃げ道も兼ねる)。長押しは
                // 「開き括弧だけ使いたかった」場合に保留を捨てる。
                if (this.pendingBracketClose.length > 0) {
                  Text(this.pendingBracketClose + ' ►で閉じる')
                    .fontFamily(JP_FONT_FAMILY)
                    .fontSize(this.isLandscape ? 10 : 12)
                    .fontColor(kbTheme(this.isDark).onAccent)
                    .backgroundColor(kbTheme(this.isDark).accent)
                    .borderRadius(10)
                    .padding({ left: 8, right: 8, top: 3, bottom: 3 })
                    .margin({ right: 6 })
                    .onClick(() => {
                      InputHandler.getInstance().flushPendingBracketClose();
                    })
                    .gesture(
                      LongPressGesture({ duration: 400 })
                        .onAction(() => {
                          const dropped = this.pendingBracketClose;
                          InputHandler.getInstance().cancelPendingBracketClose();
                          promptAction.showToast({ message: dropped + ' の自動挿入を取り消しました' });
                        })
                    )
                }

                // かな表示の行を出していないときは「設定」の置き場がここしか
                // 無いので、候補バーの右端に寄せる。
                if (!this.showComposingPanel
                    && !(this.convEnd > 0 && this.convEnd < this.composingText.length)) {
                  Row() {
                    SymbolGlyph($r('sys.symbol.gearshape'))
                      .fontSize(this.isLandscape ? 12 : 18)
                      .fontColor([kbTheme(this.isDark).textSecondary])
                  }
                  .width(this.isLandscape ? 24 : 36)
                  .height('100%')
                  .justifyContent(FlexAlign.Center)
                  .alignItems(VerticalAlign.Center)
                  .responseRegion({ x: '-20%', y: '-60%', width: '140%', height: '220%' })
                  .onClick(() => {
                    AppStorage.setOrCreate('keyboardSubMode', 'settings');
                  })
                  .gesture(
                    GestureGroup(GestureMode.Sequence,
                      LongPressGesture({ duration: 300 })
                        .onAction(() => {
                          this.floatingHint = true;
                          this.floatingHintTimer = setTimeout(() => { this.floatingHint = false; }, 1500);
                        }),
                      PanGesture({ direction: PanDirection.Up, distance: 20 })
                        .onActionStart(() => {
                          InputHandler.getInstance().toggleFloatingMode();
                          clearTimeout(this.floatingHintTimer);
                          this.floatingHint = false;
                        })
                    )
                  )
                }
              }
              .width('100%')
              .height(this.candidateBarHeightVp())
              .backgroundColor(kbTheme(this.isDark).previewBg)
              .alignItems(VerticalAlign.Center)
            }
            .width('100%')
          }

          // Keyboard — fills all height remaining after the preview bar.
          // One-handed mode shrinks+docks this via a scale transform anchored to
          // an edge, rather than touching the pixel-tuned computeKeyWidths /
          // applyKeyboardWidthsForVp system: each inner view still measures and
          // lays out its keys at full intended size, then this wrapper visually
          // (and for hit-testing) scales the whole thing down toward one side.
          this.KeyboardArea()
        }
      }
      .width(this.floatingMode ? this.floatingCardWVp : '100%')
      .height(this.floatingMode ? this.floatingCardHVp : '100%')
      // フローティング中はウィンドウ自体をカードより一回り大きく取ってあり
      // (KeyboardController の FLOATING_MARGIN_VP)、四隅ハンドルや影がその
      // 余白へはみ出せるようにしてある。カード本体はその中央に絶対配置で
      // 収める。固定時は今までどおり通常フロー ('100%'、position無し)。
      .position({
        x: this.floatingMode ? FLOATING_MARGIN_VP : 0,
        y: this.floatingMode ? FLOATING_MARGIN_VP : 0,
      })
      .backgroundColor(kbTheme(this.isDark).panelBg)
      // フローティング中だけ、画面下端に張り付いた通常のキーボードとは
      // 見た目でも区別が付くよう角丸+縁取り+影を付ける (「デザインが馴染
      // まない」の指摘)。縁取りはテーマのアクセント色だと主張が強すぎたので
      // 薄い水色に変更。固定時は今までどおり装飾無し。
      .borderRadius(this.floatingMode ? 18 : 0)
      .clip(this.floatingMode)
      .border(this.floatingMode
        ? { width: 1.5, color: FLOATING_BORDER_COLOR }
        : { width: 0 })
      .shadow(this.floatingMode
        ? { radius: 18, color: kbTheme(this.isDark).shadowColor, offsetX: 0, offsetY: 8 }
        : { radius: 0, color: 'transparent', offsetX: 0, offsetY: 0 })
      .onAreaChange((_oldArea: Area, newArea: Area) => {
        // Confirmed via on-device diagnostics: this device's IME panel framework
        // silently shrinks the panel to a fraction of the width requested via
        // panel.resize() (~76% observed, with no corresponding cutout reported —
        // a software policy, not a hardware inset), so it isn't queryable up
        // front. Correct key widths from what this root view actually measures,
        // and also record the (measured / raw guess) ratio as a learned scale
        // factor — see KeyboardController.computeKeyWidths for why: this panel
        // is reused across show/hide, so a later resize may not be followed by
        // a fresh onAreaChange, and the learned scale lets that next guess
        // start out already corrected instead of reverting to the raw width.
        // フローティング中はこのColumnの幅を floatingCardWVp という
        // 「狙って設定した」vp値そのものにしてあり、画面全幅を前提にした
        // FIXED時の学習ロジック (lastRawFullVp との比較) を通すと無関係な
        // 値同士を比べて widthScaleLandscape/Portrait を壊してしまう。
        // フローティング中は素通りする。
        if (this.floatingMode) { return; }
        const measuredVp = Number(newArea.width);
        if (measuredVp > 0) {
          const isTabletClass = AppStorage.get<boolean>('isTabletClass') ?? false;
          applyKeyboardWidthsForVp(measuredVp, isTabletClass, this.isLandscape);
          if (!isTabletClass) {
            const rawFullVp = AppStorage.get<number>('lastRawFullVp') ?? 0;
            if (rawFullVp > 0) {
              const ratio = Math.min(1, Math.max(0.3, measuredVp / rawFullVp));
              const isLand = AppStorage.get<boolean>('lastRawFullVpIsLandscape') ?? this.isLandscape;
              AppStorage.setOrCreate(isLand ? 'widthScaleLandscape' : 'widthScalePortrait', ratio);
            }
          }
        }
      })

      // Expanded candidate list — a top-level Stack sibling of the base panel
      // above, NOT nested inside the keyboard's own Column. That placement
      // matters for two reasons: (1) the keyboard Column only spans the area
      // below the preview/candidate-bar strip, so nesting it there capped the
      // grid to that same sliver instead of the fuller panel; (2) the keyboard
      // Column is what one-handed mode's .scale() transform shrinks and docks
      // to a corner — sitting outside it means the expanded grid always renders
      // at full size and full position regardless of one-handed mode. Full
      // width/height so it fully covers (not just overlaps) everything below,
      // preview strip included.
      if (this.candidateExpanded && this.candidates.length > 0) {
        ExpandedCandidateView({
          candidates: this.candidates,
          selectedIndex: this.selectedIndex
        })
        .width('100%')
        .height('100%')
      }

      // フローティング中だけの上際ドラッグ・四隅リサイズハンドル。通常の
      // レイアウトには一切影響しない絶対位置オーバーレイなので、他の
      // 分岐と関係なく最後に重ねてよい。
      if (this.floatingMode) {
        this.FloatingHandles()
      }
      if (this.dockPreviewProgress > 0) {
        this.DockPreview()
      }
      if (this.floatingHint) {
        this.FloatingHintBanner()
      }
    }
    .width('100%')
    .height('100%')
  }
}

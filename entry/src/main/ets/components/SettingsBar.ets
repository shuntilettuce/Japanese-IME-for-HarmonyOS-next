import { InputHandler } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { promptAction } from '@kit.ArkUI';
import { JP_FONT_FAMILY } from './JpFont';
import { DEBUG_INPUT_LOG } from '../ime/InputLog';
import { ClipItem } from '../ime/ClipHistory';

// Segmented-control / card metrics (design tokens: settings cards 16px,
// segmented-control track 20px / inner pill 20px).
const CARD_RADIUS = 16;
const TRACK_RADIUS = 20;
const PILL_RADIUS = 20;
// 全消去に必要な連打回数。誤操作で到達しない回数にしてある。
const RESET_ALL_TAPS = 5;

// 設定項目が増えて縦一列のスクロールでは見づらくなったため、左にカテゴリ
// 一覧・右に選んだカテゴリの中身、という2ペイン構成にしてある
// (CategoryNav/MenuView 参照)。
interface CategoryDef {
  id: string;
  label: string;
}

const CATEGORIES: CategoryDef[] = [
  { id: 'clipboard', label: 'クリップボード' },
  { id: 'dict', label: '辞書' },
  { id: 'input', label: '入力' },
  { id: 'visual', label: 'ビジュアル調整' },
  { id: 'repair', label: '修復' },
  { id: 'other', label: 'その他' },
  { id: 'support', label: '開発を応援' },
];
const CATEGORY_COL_WIDTH = 96;

// Dedicated settings sub-mode, entered via keyboardSubMode === 'settings'
// (the ⚙ button in KeyboardPage). Takes the full IME panel the same way
// SymbolView/EmojiView do (header row + Scroll content) rather than
// floating a narrow card over the keyboard -- the previous floating-card
// version was too cramped once enough menu items accumulated.
@Component
export struct SettingsBar {
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('clipboardHistory') clipboardHistory: string[] = [];
  @StorageLink('clipboardPinned') clipboardPinned: string[] = [];   // 固定した文 (履歴を消しても残る)
  @StorageLink('clipItems') clipItems: ClipItem[] = [];   // 保存する履歴 (文・画像・動画・ファイル。ClipHistory)
  @State clipPopupId: number = -1;     // 長押しで見本を出している履歴 (-1 = 出していない)
  @State clipNotice: string = '';      // 貼れなかったときの一言
  @StorageLink('oneHandedMode') oneHandedMode: boolean = false;
  @StorageLink('oneHandedSide') oneHandedSide: string = 'right';
  @StorageLink('kanaKeyboardLayout') kanaKeyboardLayout: string = 'QWERTY';
  @StorageLink('alphabetKeyboardLayout') alphabetKeyboardLayout: string = 'QWERTY';
  @StorageLink('numericPadEnabled') numericPadEnabled: boolean = false;
  @StorageLink('aiLearn') aiLearn: boolean = false;
  @StorageLink('hapticEnabled') hapticEnabled: boolean = false;
  // キーボードのライト／ダーク。'system' = 端末に合わせる。
  @StorageLink('themeMode') themeMode: string = 'system';
  @StorageLink('keyChromeEnabled') keyChromeEnabled: boolean = true;
  @StorageLink('flickHintsEnabled') flickHintsEnabled: boolean = true;
  @StorageLink('bracketCloseMode') bracketCloseMode: string = 'pair';
  // タブレット/フォルダブルの展開画面かどうか (KeyboardController が画面の
  // 短辺から判定)。幅の上限調整は普通のスマホでは効かないので、この端末
  // 区分のときだけ出す。
  @StorageLink('isTabletClass') isTabletClass: boolean = false;
  @StorageLink('keyboardWidthScale') keyboardWidthScale: number = 1.0;
  @StorageLink('keyboardHorizontalOffset') keyboardHorizontalOffset: number = 0;
  @StorageLink('floatingMode') floatingMode: boolean = false;
  // 入力中のかなの表示先 (独立した2トグル、組み合わせは4通り)。
  @StorageLink('composingInPanel') composingInPanel: boolean = true;
  @StorageLink('composingInField') composingInField: boolean = true;
  // 入力欄側に実際に出せているか (設定ONでも入力欄が非対応なら false)。
  @StorageLink('previewInField') previewInField: boolean = false;
  // この端末では拡張プロセスからの振動が許可されていない、と分かった状態。
  @StorageLink('hapticUnavailable') hapticUnavailable: boolean = false;
  @StorageLink('clipboardUnavailable') clipboardUnavailable: boolean = false;
  // 設定メニューの2ペイン構成 (CategoryNav/MenuView 参照) で、いま右側に
  // 表示しているカテゴリ。CATEGORIES の id のいずれか。
  @State selectedCategory: string = 'clipboard';
  private menuScroller: Scroller = new Scroller();
  private sectionTop: Map<string, number> = new Map<string, number>();   // 各種類の見出しの位置 (縦に送る量)
  @State showHistory: boolean = false;
  @State showDict: boolean = false;
  // ユーザー辞書の中の下位画面。'' = 入口のメニュー / 'suggest' = 登録候補 /
  // 'registered' = 登録済み。どちらも件数が多くなるので画面を分けている。
  @State dictSub: string = '';
  // ユーザー辞書はキーボード側で完結させる (本体アプリの辞書画面から登録しても
  // 拡張には届かない ── InputHandler.registerUserWord のコメント参照)。
  // "よみ\t表記\t回数" / "よみ\t表記" の文字列配列。構造体配列だと @StorageLink の
  // 更新検知が効きにくいため。
  @StorageLink('dictSuggestions') dictSuggestions: string[] = [];
  @StorageLink('dictRegistered') dictRegistered: string[] = [];
  // 候補の長押しで削除した語 ("よみ	表記")。
  @StorageLink('suppressedRows') suppressedRows: string[] = [];
  // 自由入力の欄の中身と、いまどちらの欄に打鍵が入るか。
  // 学習リセットは2回押しで確認する。
  @State confirmResetLearning: boolean = false;
  // 入力ログ (デバッグビルドのみ)。件数/サイズと書き出しの進み具合。
  @StorageLink('inputLogSummary') inputLogSummary: string = '';
  @State confirmClearInputLog: boolean = false;
  // 全消去は RESET_ALL_TAPS 回の連打を要求する。手が止まると0に戻す。
  @State resetAllTaps: number = 0;
  private resetAllTimer: number = -1;

  // Re-read the system clipboard whenever the settings panel opens, so the
  // クリップボード履歴 count/list is current -- a copy made in the host app
  // while the keyboard stayed up otherwise wasn't reflected until the keyboard
  // was closed and reopened (AppGallery reviewer feedback).
  aboutToAppear(): void {
    InputHandler.getInstance().refreshClipboard();
  }

  private close(): void {
    // 自由入力を掴んだままパネルを閉じると、以降ホストアプリに文字が入らなく
    // なる。抜ける経路では必ず解除する。
    InputHandler.getInstance().stopCapture();
    AppStorage.setOrCreate('keyboardSubMode', 'kana');
    this.showHistory = false;
    this.showDict = false;
    this.dictSub = '';
  }

  // Opening the クリップボード履歴 view re-reads the clipboard first, so the
  // most recent copy is always shown without closing/reopening the keyboard.
  private openHistory(): void {
    InputHandler.getInstance().refreshClipboard();
    this.showHistory = true;
  }

  // 開くたびに最新を配り直す。登録候補は打つたびに増えるので、
  // 開いた瞬間の内容で作らないと古いままになる。
  private openDict(): void {
    InputHandler.getInstance().publishDictRows();
    this.dictSub = '';
    this.showDict = true;
  }


  build() {
    Column({ space: 0 }) {
      // ── Header: symmetric 56px gutters so the title stays centered
      // regardless of which side has a control (design tokens). ──
      Row() {
        Row() {
          if (this.showHistory || this.showDict) {
            Row({ space: 2 }) {
              SymbolGlyph($r('sys.symbol.chevron_left'))
                .fontSize(16)
                .fontWeight(FontWeight.Medium)
                .fontColor([kbTheme(this.isDark).accent])
              Text('戻る')
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(14)
                .fontWeight(FontWeight.Medium)
                .fontColor(kbTheme(this.isDark).accent)
            }
            .height('100%')
            .alignItems(VerticalAlign.Center)
            .onClick(() => {
              InputHandler.getInstance().stopCapture();
              // 辞書の下位画面にいるときは、まず辞書の入口へ戻る。
              if (this.showDict && this.dictSub.length > 0) {
                this.dictSub = '';
                return;
              }
              this.showHistory = false;
              this.showDict = false;
            })
          }
        }
        .width(56)
        .height('100%')
        .justifyContent(FlexAlign.Start)
        .alignItems(VerticalAlign.Center)

        Text(this.showHistory
          ? 'クリップボード履歴'
          : (this.showDict
            ? (this.dictSub === 'suggest' ? '登録候補'
              : (this.dictSub === 'registered' ? '登録済み'
                : (this.dictSub === 'suppressed' ? '削除した候補' : 'ユーザー辞書')))
            : '設定'))
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)

        Row() {
          if (this.showHistory) {
            Text('削除')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(12)
              .fontColor(kbTheme(this.isDark).textSecondary)
              .onClick(() => {
                InputHandler.getInstance().clearClipboardHistory();
              })
          } else if (!this.showDict) {
            Row() {
              SymbolGlyph($r('sys.symbol.xmark'))
                .fontSize(14)
                .fontColor([kbTheme(this.isDark).textSecondary])
            }
            .width(28).height(28)
            .justifyContent(FlexAlign.Center)
            .alignItems(VerticalAlign.Center)
            .backgroundColor(kbTheme(this.isDark).funcBg)
            .borderRadius(8)
            .onClick(() => { this.close(); })
          }
        }
        .width(56)
        .justifyContent(FlexAlign.End)
      }
      .width('100%')
      .height(48)
      .padding({ left: 14, right: 14 })
      .alignItems(VerticalAlign.Center)

      Divider().color(kbTheme(this.isDark).divider)

      // ── Content (scrolls if it doesn't fit the panel height) ──
      if (this.showHistory || this.showDict) {
        Scroll() {
          Column() {
            if (this.showHistory) {
              this.HistoryView()
            } else if (this.showDict && this.dictSub === 'suggest') {
              this.DictSuggestView()
            } else if (this.showDict && this.dictSub === 'registered') {
              this.DictRegisteredView()
            } else if (this.showDict && this.dictSub === 'suppressed') {
              this.SuppressedView()
            } else {
              this.DictView()
            }
          }
          .padding(16)
        }
        .width('100%')
        .layoutWeight(1)
        .align(Alignment.Top)
        .scrollBar(BarState.Auto)
        .edgeEffect(EdgeEffect.Spring)
      } else {
        // メニュー入口だけは左カテゴリ一覧・右カテゴリの中身という2ペイン
        // 構成 (MenuView 参照)。ここは自分でスクロール領域を左右別々に
        // 持つので、上のような共通の Scroll には乗せない。
        this.MenuView()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(kbTheme(this.isDark).panelBg)
  }


  // Segmented-control options are written inline (not as a shared @Builder
  // taking an "active" boolean parameter) — ArkUI's fine-grained update
  // didn't reliably re-invoke a parameterized child @Builder when only the
  // underlying @StorageLink changed (the toggle's highlighted state stuck
  // until the whole panel was torn down and rebuilt, i.e. closed/reopened).
  // Each option below reads its @StorageLink field directly so the
  // dependency is unambiguous.

  // 左のカテゴリ一覧。選択中は accent 塗り、それ以外は透明地に控えめな文字色。
  @Builder CategoryNav() {
    Scroll() {
      Column({ space: 4 }) {
        ForEach(CATEGORIES, (cat: CategoryDef) => {
          Text(cat.label)
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(this.selectedCategory === cat.id ? FontWeight.Bold : FontWeight.Medium)
            .fontColor(this.selectedCategory === cat.id
              ? kbTheme(this.isDark).onAccent
              : kbTheme(this.isDark).textSecondary)
            .backgroundColor(this.selectedCategory === cat.id
              ? kbTheme(this.isDark).accent
              : 'transparent')
            .textAlign(TextAlign.Start)
            .width('100%')
            .borderRadius(10)
            .padding({ left: 12, right: 6, top: 10, bottom: 10 })
            .onClick(() => {
              this.selectedCategory = cat.id;
              // その種類の見出しへ送る (全部の種類を 1 本につないで縦に送れるようにした。ユーザー 2026-10-14)
              const y = this.sectionTop.get(cat.id);
              if (y !== undefined) { this.menuScroller.scrollTo({ xOffset: 0, yOffset: y, animation: { duration: 250 } }); }
            })
        }, (cat: CategoryDef) => cat.id)
      }
      .width('100%')
      .padding({ top: 12, bottom: 12, left: 8 })
    }
    .width(CATEGORY_COL_WIDTH)
    .height('100%')
    .align(Alignment.Top)
    .scrollBar(BarState.Off)
    .backgroundColor(kbTheme(this.isDark).cardBg)
  }

  // 右側、選んだカテゴリの中身だけを出す。カテゴリごとの実体は
  // ClipboardSection〜SupportSection の各 @Builder。
  @Builder MenuView() {
    Row() {
      this.CategoryNav()

      Divider()
        .vertical(true)
        .color(kbTheme(this.isDark).divider)

      // 全部の種類を見出しつきで 1 本につなぐ。見えている所に合わせて左の一覧の選択が動く
      Scroll(this.menuScroller) {
        Column() {
          ForEach(CATEGORIES, (cat: CategoryDef) => {
              Column() {
                Text(cat.label)
                  .fontFamily(JP_FONT_FAMILY)
                  .fontSize(16)
                  .fontWeight(FontWeight.Bold)
                  .fontColor(kbTheme(this.isDark).textPrimary)
                  .width('100%')
                  .padding({ top: 8, bottom: 10 })
                if (cat.id === 'clipboard') {
                  this.ClipboardSection()
                } else if (cat.id === 'dict') {
                  this.DictSection()
                } else if (cat.id === 'input') {
                  this.InputSection()
                } else if (cat.id === 'visual') {
                  this.VisualSection()
                } else if (cat.id === 'repair') {
                  this.RepairSection()
                  if (DEBUG_INPUT_LOG) {
                    this.InputLogSection()
                  }
                } else if (cat.id === 'other') {
                  this.OtherSection()
                } else {
                  this.SupportSection()
                }
              }
              .width('100%')
              .padding({ bottom: 20 })
              .onAreaChange((_o: Area, n: Area) => { this.sectionTop.set(cat.id, Number(n.position.y)); })
          }, (cat: CategoryDef) => cat.id)
        }
        .padding(16)
      }
      .layoutWeight(1)
      .height('100%')
      .align(Alignment.Top)
      .scrollBar(BarState.Auto)
      .onDidScroll(() => {
        const y = this.menuScroller.currentOffset().yOffset + 24;
        let cur = CATEGORIES[0].id;
        for (const c of CATEGORIES) {
          const t = this.sectionTop.get(c.id);
          if (t !== undefined && t <= y) { cur = c.id; }
        }
        if (cur !== this.selectedCategory) { this.selectedCategory = cur; }
      })
      .edgeEffect(EdgeEffect.Spring)
    }
    .width('100%')
    .layoutWeight(1)
    .alignItems(VerticalAlign.Top)
  }

  @Builder ClipboardSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('クリップボード履歴')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          // 読み出しに一度でも失敗していれば非対応と分かる
          // (KeyboardController.refreshClipboardPreview 参照)。
          if (this.clipboardUnavailable) {
            Text('この端末では利用できません（OSがキーボードからのクリップボード読み取りを許可していません）')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(11)
              .fontColor(kbTheme(this.isDark).textSecondary)
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Row({ space: 6 }) {
          Text(String(this.clipItems.length))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontColor(kbTheme(this.isDark).textSecondary)
          Text('›')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .enabled(!this.clipboardUnavailable)
      .onClick(() => { this.openHistory(); })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
    .opacity(this.clipboardUnavailable ? 0.4 : 1)
  }

  @Builder DictSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('ユーザー辞書')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .width('100%')
          Text('打った語をここから登録できます')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => { this.openDict(); })


    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  @Builder InputSection() {
    Column() {
      // Switches immediately (same process as the keyboard itself) — more
      // reliable than the main app's copy of this same toggle, which has to
      // round-trip through shared preferences for the extension to pick it up.
      Row() {
        Text('かな入力レイアウト')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)

        Row({ space: 2 }) {
          Text('QWERTY')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.kanaKeyboardLayout === 'QWERTY' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.kanaKeyboardLayout === 'QWERTY' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setKanaKeyboardLayout('QWERTY'); })

          Text('フリック')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.kanaKeyboardLayout === 'FLICK' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.kanaKeyboardLayout === 'FLICK' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setKanaKeyboardLayout('FLICK'); })
        }
        .backgroundColor(kbTheme(this.isDark).funcBg)
        .borderRadius(TRACK_RADIUS)
        .padding(2)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })

      Row() {
        Text('アルファベット入力レイアウト')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)

        Row({ space: 2 }) {
          Text('QWERTY')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.alphabetKeyboardLayout === 'QWERTY' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.alphabetKeyboardLayout === 'QWERTY' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setAlphabetKeyboardLayout('QWERTY'); })

          Text('フリック')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.alphabetKeyboardLayout === 'FLICK' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.alphabetKeyboardLayout === 'FLICK' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setAlphabetKeyboardLayout('FLICK'); })
        }
        .backgroundColor(kbTheme(this.isDark).funcBg)
        .borderRadius(TRACK_RADIUS)
        .padding(2)
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })

      Divider().color(kbTheme(this.isDark).divider)

      // 打鍵の手応え。既定はオフ (勝手に振動すると煩わしいため)。
      // オンにした瞬間に一度鳴らして、効きが分かるようにしている。
      Row() {
        Column({ space: 2 }) {
          Text('キー入力時の振動')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text(this.hapticUnavailable
            ? 'この端末では利用できません（OSがキーボードからの振動を許可していません）'
            : 'キーを押したときに短く振動します')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.hapticEnabled })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .enabled(!this.hapticUnavailable)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setHapticEnabled(isOn);
          })
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })
      // 非対応端末では触れないようにグレーアウトする。説明文 (上の
      // hapticUnavailable 分岐) だけだと、触れてしまうぶん「効かない設定」に
      // 見えて紛らわしかった。
      .opacity(this.hapticUnavailable ? 0.4 : 1)

      Divider().color(kbTheme(this.isDark).divider)

      // Off by default -- see SettingsStore's numericPadEnabled comment. The
      // 「あA」キーは常に日本語⇔アルファベットを切り替えるが、これがオンだと
      // アルファベット状態からもう一度押すことで数字パッドまで進むようになる
      // (InputHandler.cycleInputMode参照)。取消キーは常時表示のため文言不要。
      Row() {
        Column({ space: 2 }) {
          Text('数字パッド')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('オンにすると「あA」キーで日本語→アルファベット→数字と切り替わります')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.numericPadEnabled })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setNumericPadEnabled(isOn);
          })
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text('AI学習')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('確定した文から、この端末の中で変換を育てます')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.aiLearn })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setAiLearnEnabled(isOn);
          })
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })

      Divider().color(kbTheme(this.isDark).divider)




      // 開き括弧を入れたあと、閉じ括弧をいつ出すか。どちらのモードでも
      // 「開きだけ先に出して閉じは保留する」仕組み自体は共通で、違うのは
      // 保留を解く引き金だけ (KeyboardController の BRACKET_PAIRS 参照)。
      Row() {
        Text('括弧の閉じ方')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)

        Row({ space: 2 }) {
          Text('組で')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.bracketCloseMode !== 'manual' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.bracketCloseMode !== 'manual' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setBracketCloseMode('pair'); })

          Text('手動')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.bracketCloseMode === 'manual' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.bracketCloseMode === 'manual' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setBracketCloseMode('manual'); })

        }
        .backgroundColor(kbTheme(this.isDark).funcBg)
        .borderRadius(TRACK_RADIUS)
        .padding(2)
      }
      .width('100%')
      .padding({ top: 12, bottom: 4 })

      Text(this.bracketCloseMode !== 'manual'
        ? '開きと閉じを一緒に入れて、カーソルを括弧の中に置きます'
        : '► キー（フリック・数字パッド）か、候補バーの待機バッジのタップで閉じます。括弧の中で何度確定しても勝手には閉じません')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(10)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .padding({ bottom: 12 })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  // キーボードの見た目・大きさ・位置に関わる調整をまとめた区分。
  // 「入力」(かな入力レイアウト・変換エンジン等、打鍵の挙動そのもの)とは
  // 分けてある。レイアウト調整・片手モードはどちらも「文字の変換や
  // 打鍵の仕方」ではなく「キーボードの形をどう見せるか」の調整なので
  // ここに移した。
  @Builder VisualSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('レイアウト調整')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .width('100%')
          Text('キーボードを見ながら縦の長さや幅の比率を調整できます')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(11)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(15)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 13, bottom: 13 })
      .onClick(() => { AppStorage.setOrCreate('keyboardSubMode', 'layoutAdjust'); })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('片手モード')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.oneHandedMode })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setOneHandedMode(isOn, this.oneHandedSide);
          })
      }
      .width('100%')
      .padding({ top: 13, bottom: this.oneHandedMode ? 8 : 13 })

      if (this.oneHandedMode) {
        Row({ space: 8 }) {
          Text('左寄せ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.oneHandedSide === 'left' ? kbTheme(this.isDark).accent : kbTheme(this.isDark).funcBg)
            .fontColor(this.oneHandedSide === 'left' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setOneHandedMode(true, 'left'); })

          Text('右寄せ')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.oneHandedSide === 'right' ? kbTheme(this.isDark).accent : kbTheme(this.isDark).funcBg)
            .fontColor(this.oneHandedSide === 'right' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setOneHandedMode(true, 'right'); })
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .padding({ bottom: 14 })
      }

      Divider().color(kbTheme(this.isDark).divider)

      // 設定ボタンの長押し→上スライドでも切り替えられる (KeyboardPage
      // 参照)。ジェスチャに気づかない人向けの確実な入口としてここにも置く。
      Row() {
        Column({ space: 2 }) {
          Text('フローティングキーボード')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('画面下端に固定せず、好きな位置・大きさで浮かせて使えます。設定ボタンの長押し→上スライドでも切り替えられます')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(11)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.floatingMode })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setFloatingModeExplicit(isOn);
          })
      }
      .width('100%')
      .padding({ top: 13, bottom: 13 })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text('フリックの十字ヒント')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('上下左右の小さいかな表示を消します。フリック自体はそのまま使えます')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(11)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.flickHintsEnabled })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setFlickHintsEnabled(isOn);
          })
      }
      .width('100%')
      .padding({ top: 13, bottom: 13 })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('外観')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)

        Row({ space: 2 }) {
          Text('自動')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.themeMode === 'system' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.themeMode === 'system' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setThemeMode('system'); })

          Text('ライト')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.themeMode === 'light' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.themeMode === 'light' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setThemeMode('light'); })

          Text('ダーク')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(PILL_RADIUS)
            .backgroundColor(this.themeMode === 'dark' ? kbTheme(this.isDark).accent : 'transparent')
            .fontColor(this.themeMode === 'dark' ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textSecondary)
            .onClick(() => { InputHandler.getInstance().setThemeMode('dark'); })
        }
        .backgroundColor(kbTheme(this.isDark).funcBg)
        .borderRadius(TRACK_RADIUS)
        .padding(3)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text('キーの外枠')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('キーの背景色を有効にする')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(11)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.keyChromeEnabled })
          .width(44)
          .height(26)
          .selectedColor(kbTheme(this.isDark).accent)
          .onChange((isOn: boolean) => {
            InputHandler.getInstance().setKeyChromeEnabled(isOn);
          })
      }
      .width('100%')
      .padding({ top: 13, bottom: 13 })

      // タブレット/フォルダブルの展開画面でだけ効く調整。普通のスマホでは
      // 画面が最大幅の上限に届かないので出しても意味が無く隠してある。
      // 実機を持たずに検証できないぶん、この調整は「開発側があらかじめ
      // 正解のサイズに決め打つ」のを諦めて利用者自身に追い込んでもらう
      // ための項目 (KeyboardController.applyKeyboardWidthsForVp 参照)。
      if (this.isTabletClass) {
        Divider().color(kbTheme(this.isDark).divider)

        Row() {
          Column({ space: 2 }) {
            Text('キーボード幅の上限')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(14)
              .fontColor(kbTheme(this.isDark).textPrimary)
            Text('タブレット/フォルダブルの展開画面での最大幅を調整します')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(11)
              .fontColor(kbTheme(this.isDark).textSecondary)
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Row({ space: 10 }) {
            Text('−')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(16)
              .fontWeight(FontWeight.Bold)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .width(28).height(28)
              .textAlign(TextAlign.Center)
              .backgroundColor(kbTheme(this.isDark).funcBg)
              .borderRadius(14)
              .onClick(() => {
                const next = Math.max(0.5, Math.round((this.keyboardWidthScale - 0.05) * 100) / 100);
                InputHandler.getInstance().saveKeyboardWidthScale(next);
              })

            Text(`${Math.round(this.keyboardWidthScale * 100)}%`)
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(13)
              .fontColor(kbTheme(this.isDark).textSecondary)
              .width(44)
              .textAlign(TextAlign.Center)

            Text('＋')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(16)
              .fontWeight(FontWeight.Bold)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .width(28).height(28)
              .textAlign(TextAlign.Center)
              .backgroundColor(kbTheme(this.isDark).funcBg)
              .borderRadius(14)
              .onClick(() => {
                const next = Math.min(1.6, Math.round((this.keyboardWidthScale + 0.05) * 100) / 100);
                InputHandler.getInstance().saveKeyboardWidthScale(next);
              })
          }
        }
        .width('100%')
        .padding({ top: 13, bottom: 13 })

        Divider().color(kbTheme(this.isDark).divider)

        // 幅の上限で狭くなったぶんの余白を、左右どちらへ寄せるか。大画面では
        // 中央に置くと両手が届かないことがあるため (「左右に空白を入れれる
        // ように」)。幅が画面いっぱいのままなら寄せる余白が無く効かない。
        Row() {
          Column({ space: 2 }) {
            Text('キーボードの左右位置')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(14)
              .fontColor(kbTheme(this.isDark).textPrimary)
            Text('幅の上限で空いた余白を、左右どちらに寄せるか決めます')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(11)
              .fontColor(kbTheme(this.isDark).textSecondary)
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Row({ space: 10 }) {
            Text('◀')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(14)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .width(28).height(28)
              .textAlign(TextAlign.Center)
              .backgroundColor(kbTheme(this.isDark).funcBg)
              .borderRadius(14)
              .onClick(() => {
                const next = Math.max(-1, Math.round((this.keyboardHorizontalOffset - 0.2) * 100) / 100);
                InputHandler.getInstance().saveKeyboardHorizontalOffset(next);
              })

            Text(this.keyboardHorizontalOffset === 0 ? '中央'
              : `${this.keyboardHorizontalOffset < 0 ? '左' : '右'}${Math.round(Math.abs(this.keyboardHorizontalOffset) * 100)}%`)
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(13)
              .fontColor(kbTheme(this.isDark).textSecondary)
              .width(52)
              .textAlign(TextAlign.Center)

            Text('▶')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(14)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .width(28).height(28)
              .textAlign(TextAlign.Center)
              .backgroundColor(kbTheme(this.isDark).funcBg)
              .borderRadius(14)
              .onClick(() => {
                const next = Math.min(1, Math.round((this.keyboardHorizontalOffset + 0.2) * 100) / 100);
                InputHandler.getInstance().saveKeyboardHorizontalOffset(next);
              })
          }
        }
        .width('100%')
        .padding({ top: 13, bottom: 13 })
      }
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  // 本体アプリの「修復」はアプリ側のストアしか消せず、キーボードの学習・設定・
  // 辞書には届かない (別サンドボックス)。復旧手段はここに置く。
  @Builder RepairSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text(this.confirmResetLearning ? 'もう一度押すとリセットします' : '学習をリセット')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .width('100%')
          Text('変換の学習・次単語予測・記号の履歴を消します')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        // 1回目は確認、2回目で実行。
        if (!this.confirmResetLearning) {
          this.confirmResetLearning = true;
          return;
        }
        this.confirmResetLearning = false;
        InputHandler.getInstance().resetLearningData();
        promptAction.showToast({ message: '学習をリセットしました' });
      })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text(this.resetAllTaps > 0
            ? `あと${RESET_ALL_TAPS - this.resetAllTaps}回押すと全消去します`
            : 'すべてリセット')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).deleteRed)
            .width('100%')
          Text(`${RESET_ALL_TAPS}回連続で押すと実行。学習に加えユーザー辞書・削除した候補・設定も消えます`)
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      // 全消去は取り返しがつかないので、意図しないと成立しない操作にする。
      // 2回押しでは続けて押せば起きうるし、長押しも持ち替えの拍子に成立しうる。
      // 5回連打を要求し、残り回数を出して「今これを消そうとしている」と分かる
      // ようにする。手が止まれば (2.5秒) カウントは0に戻る。
      .onClick(() => {
        clearTimeout(this.resetAllTimer);
        this.resetAllTaps += 1;
        if (this.resetAllTaps >= RESET_ALL_TAPS) {
          this.resetAllTaps = 0;
          InputHandler.getInstance().resetAllData();
          promptAction.showToast({ message: 'すべてリセットしました' });
          return;
        }
        this.resetAllTimer = setTimeout(() => { this.resetAllTaps = 0; }, 2500);
      })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  // 入力ログ (デバッグビルドのみ。リリースビルドでは DEBUG_INPUT_LOG が false
  // になり、この項目ごと出ない)。取り出しは USB デバッグ経由なので、ここには
  // 閲覧画面を置かず、状態の確認と書き出し・消去だけを置く。
  @Builder InputLogSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('入力ログ (デバッグビルドのみ)')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .width('100%')
          Text(this.inputLogSummary.length > 0 ? this.inputLogSummary : '—')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text('USBへ書き出す')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).accent)
            .width('100%')
          Text('先にPCで node tools/pull_input_log.js を起動してから押す')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => { InputHandler.getInstance().exportInputLog(); })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text(this.confirmClearInputLog ? 'もう一度押すと消去します' : '入力ログを消去')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).deleteRed)
          .layoutWeight(1)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        // 書き出し前に消すと取り返しがつかないので2回押しにする。
        if (!this.confirmClearInputLog) {
          this.confirmClearInputLog = true;
          return;
        }
        this.confirmClearInputLog = false;
        InputHandler.getInstance().clearInputLog();
      })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
    .margin({ top: 14 })
    .onAppear(() => { InputHandler.getInstance().publishInputLogSummary(); })
  }

  @Builder OtherSection() {
    Column() {
      Row() {
        Text('アプリを開く')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        InputHandler.getInstance().openMainApp();
        this.close();
      })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('レビューを書く')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        const opened = InputHandler.getInstance().openAppGalleryReview();
        if (!opened) {
          promptAction.showToast({ message: 'まだAppGalleryに公開されていません' });
        }
        this.close();
      })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('開発を支援 / ソースコード（GitHub）')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        InputHandler.getInstance().openGithubRepo();
        this.close();
      })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  @Builder SupportSection() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('Buy Me a Coffee')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('リンクを共有 → 決済できる端末で開いて応援（缶コーヒー1杯から）')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })
      .onClick(() => {
        InputHandler.getInstance().openBuyMeACoffeeLink();
        this.close();
      })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Column({ space: 2 }) {
          Text('GitHub Sponsors')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
          Text('リンクを共有 → GitHubで月額/単発の支援（要アカウント）')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 12, bottom: 12 })
      .onClick(() => {
        InputHandler.getInstance().openGithubSponsorsLink();
        this.close();
      })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  // 履歴の 1 行: 押すと貼る、右の 📌 で固定する・外す (固定した文の欄)
  @Builder ClipRow(item: string, pinned: boolean, onPaste: () => void) {
    Row() {
      Text(item)
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14)
        .fontColor(kbTheme(this.isDark).textPrimary)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .layoutWeight(1)
        .padding({ top: 14, bottom: 14 })
        .onClick(() => { onPaste(); })
      Text('📌')
        .fontSize(14)
        .opacity(pinned ? 1 : 0.3)
        .padding({ left: 12, right: 4, top: 14, bottom: 14 })
        .onClick(() => { InputHandler.getInstance().togglePin(item); })
    }
    .width('100%')
  }

  private clipLabel(c: ClipItem): string {
    if (c.kind === 'text') { return c.text.replace(/\n/g, ' '); }
    const head = c.kind === 'image' ? '画像' : (c.kind === 'video' ? '動画' : 'ファイル');
    return c.name.length > 0 && c.kind !== 'image' ? `${head}  ${c.name}` : head;
  }

  private pasteItem(c: ClipItem): void {
    if (InputHandler.getInstance().pasteClipItem(c)) {
      this.clipNotice = '';
      this.clipPopupId = -1;
      this.close();
    } else {
      this.clipNotice = '貼り付けられませんでした';
    }
  }

  // 長押しの見本 (画面は替えずにポップアップ。ユーザー 2026-11-05): 大きな見本と 貼り付け・固定 (文だけ)・消去
  @Builder ClipPopup() {
    Column({ space: 10 }) {
      ForEach(this.clipItems.filter((c: ClipItem) => c.id === this.clipPopupId), (c: ClipItem) => {
        if (c.thumb.length > 0) {
          Image('file://' + c.thumb)
            .objectFit(ImageFit.Contain)
            .width(260).height(200)
            .borderRadius(8)
        } else {
          Scroll() {
            Text(this.clipLabel(c).length > 0 && c.kind === 'text' ? c.text : this.clipLabel(c))
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(14)
              .fontColor(kbTheme(this.isDark).textPrimary)
              .width('100%')
          }
          .width(260)
          .constraintSize({ maxHeight: 200 })
        }
        if (c.kind !== 'text') {
          Text(this.clipLabel(c))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(11)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
        Row({ space: 8 }) {
          Text('貼り付け')
            .fontFamily(JP_FONT_FAMILY).fontSize(13)
            .fontColor(kbTheme(this.isDark).onAccent)
            .backgroundColor(kbTheme(this.isDark).accentStrong)
            .borderRadius(8).padding({ left: 14, right: 14, top: 8, bottom: 8 })
                        .onClick(() => { this.pasteItem(c); })
          if (c.kind === 'text') {
            Text('固定')
              .fontFamily(JP_FONT_FAMILY).fontSize(13)
              .fontColor(this.clipboardPinned.indexOf(c.text) >= 0 ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
              .backgroundColor(this.clipboardPinned.indexOf(c.text) >= 0 ? kbTheme(this.isDark).accentStrong : kbTheme(this.isDark).candidateBg)
              .borderRadius(8).padding({ left: 14, right: 14, top: 8, bottom: 8 })
              .onClick(() => {
                InputHandler.getInstance().togglePin(c.text);
                this.clipPopupId = -1;
              })
          }
          Text('消去')
            .fontFamily(JP_FONT_FAMILY).fontSize(13)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .backgroundColor(kbTheme(this.isDark).candidateBg)
            .borderRadius(8).padding({ left: 14, right: 14, top: 8, bottom: 8 })
            .onClick(() => {
              InputHandler.getInstance().removeClipItem(c.id);
              this.clipPopupId = -1;
            })
        }
      }, (c: ClipItem) => `pop-${c.id}`)
    }
    .padding(12)
    .alignItems(HorizontalAlign.Center)
  }

  // 保存する履歴の 1 行: 画像は見本、押すと貼る、長押しで見本のポップアップ
  @Builder ClipItemRow(c: ClipItem) {
    Row({ space: 10 }) {
      if (c.thumb.length > 0) {
        Image('file://' + c.thumb)
          .objectFit(ImageFit.Cover)
          .width(44).height(44)
          .borderRadius(6)
      } else if (c.kind !== 'text') {
        Text(c.kind === 'video' ? '▶' : '…')
          .fontSize(16)
          .fontColor(kbTheme(this.isDark).textSecondary)
          .textAlign(TextAlign.Center)
          .width(44).height(44)
          .borderRadius(6)
          .backgroundColor(kbTheme(this.isDark).candidateBg)
      }
      Text(this.clipLabel(c))
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14)
        .fontColor(kbTheme(this.isDark).textPrimary)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .layoutWeight(1)
      if (c.kind === 'text' && this.clipboardPinned.indexOf(c.text) >= 0) {
        Text('固定')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(11)
          .fontColor(kbTheme(this.isDark).accentStrong)
      }
    }
    .width('100%')
    .padding({ top: c.kind === 'text' ? 14 : 6, bottom: c.kind === 'text' ? 14 : 6 })
    .onClick(() => { this.pasteItem(c); })
    .gesture(LongPressGesture({ duration: 450 }).onAction(() => {
      InputHandler.getInstance().hapticTick();
      this.clipPopupId = c.id;
    }))
    .bindPopup(this.clipPopupId === c.id, {
      builder: this.ClipPopup,
      placement: Placement.Top,
      enableArrow: true,
      popupColor: kbTheme(this.isDark).cardBg,
      backgroundBlurStyle: BlurStyle.NONE,
      radius: 12,
      mask: { color: 0x33000000 },
      onStateChange: (e) => { if (!e.isVisible && this.clipPopupId === c.id) { this.clipPopupId = -1; } },
    })
  }

  @Builder HistoryView() {
    if (this.clipboardPinned.length > 0) {
      Text('固定した文')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(11)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .padding({ left: 4, bottom: 4 })
      List() {
        ForEach(this.clipboardPinned, (item: string) => {
          ListItem() {
            this.ClipRow(item, true, () => {
              InputHandler.getInstance().pastePinned(item);
              this.close();
            })
          }
        }, (item: string) => `pin-${item}`)
      }
      .width('100%')
      .backgroundColor(kbTheme(this.isDark).cardBg)
      .borderRadius(CARD_RADIUS)
      .padding({ left: 14, right: 14 })
      .divider({ strokeWidth: 1, color: kbTheme(this.isDark).divider })
      .margin({ bottom: 12 })
    }
    if (this.clipNotice.length > 0) {
      Text(this.clipNotice)
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(11)
        .fontColor(kbTheme(this.isDark).accentStrong)
        .width('100%')
        .padding({ left: 4, bottom: 6 })
    }
    if (this.clipItems.length === 0) {
      Text('履歴がありません')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 48, bottom: 48 })
    } else {
      Text('タップで貼り付け・長押しで見本')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(11)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .padding({ left: 4, bottom: 4 })
      List() {
        ForEach(this.clipItems.filter((c: ClipItem) => InputHandler.getInstance().isCurrentClip(c)), (c: ClipItem) => {
          ListItem() {
            this.ClipItemRow(c)
          }
        }, (c: ClipItem) => `clip-${c.id}-${c.thumb}`)
      }
      .width('100%')
      .backgroundColor(kbTheme(this.isDark).cardBg)
      .borderRadius(CARD_RADIUS)
      .padding({ left: 14, right: 14 })
      .divider({ strokeWidth: 1, color: kbTheme(this.isDark).divider })
    }
  }

  // ユーザー辞書。上段が「登録候補」(打った語のうち辞書に無いものを
  // UnknownWordLearner が拾ったもの)、下段が「登録済み」。
  // 読みと表記を手で打ち直させないので、パネル内に文字入力欄が要らない。
  // ユーザー辞書の入口。「登録候補」と「登録済み」はどちらも件数が多くなるので、
  // 1画面に積まず、それぞれ別画面に開く。
  @Builder DictView() {
    Column() {
      Row() {
        Column({ space: 2 }) {
          Text('任意の語を登録')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textPrimary)
            .width('100%')
          Text('この画面を閉じて、キーボードの上に入力欄を出します')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(10)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .width('100%')
        }
        .layoutWeight(1)
        Text('›')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textSecondary)
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        this.showDict = false;
        InputHandler.getInstance().beginDictEdit();
      })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('登録候補')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Row({ space: 6 }) {
          Text(String(this.dictSuggestions.length))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontColor(kbTheme(this.isDark).textSecondary)
          Text('›')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => { this.dictSub = 'suggest'; })

      Divider().color(kbTheme(this.isDark).divider)

      Row() {
        Text('登録済み')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Row({ space: 6 }) {
          Text(String(this.dictRegistered.length))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontColor(kbTheme(this.isDark).textSecondary)
          Text('›')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => { this.dictSub = 'registered'; })

      Divider().color(kbTheme(this.isDark).divider)

      // 候補の長押し削除で消した語。復元もここで行う。
      Row() {
        Text('削除した候補')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(14)
          .fontColor(kbTheme(this.isDark).textPrimary)
          .layoutWeight(1)
        Row({ space: 6 }) {
          Text(String(this.suppressedRows.length))
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(12)
            .fontColor(kbTheme(this.isDark).textSecondary)
          Text('›')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textSecondary)
        }
      }
      .width('100%')
      .padding({ top: 14, bottom: 14 })
      .onClick(() => {
        InputHandler.getInstance().publishSuppressedRows();
        this.dictSub = 'suppressed';
      })
    }
    .backgroundColor(kbTheme(this.isDark).cardBg)
    .borderRadius(CARD_RADIUS)
    .padding({ left: 14, right: 14 })
  }

  // 登録候補 (打った語のうち辞書に無いものを UnknownWordLearner が拾ったもの)。
  @Builder DictSuggestView() {
    // 見出しと操作を横に並べる箇所では SectionLabel を使わない。
    // あれは width('100%') なので、Row に入れると右の操作を画面外へ押し出す。
    Row() {
      Text('打った語のうち辞書に無いもの')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .letterSpacing(0.6)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .padding({ left: 4 })
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
      if (this.dictSuggestions.length > 0) {
        Text('すべて消去')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(12)
          .fontColor(kbTheme(this.isDark).deleteRed)
          .padding({ left: 8 })
          .onClick(() => { InputHandler.getInstance().clearUnknownWords(); })
      }
    }
    .width('100%')
    .padding({ bottom: 8 })
    .alignItems(VerticalAlign.Center)

    if (this.dictSuggestions.length === 0) {
      Text('候補はありません\n変換できない語を打って確定すると、ここに出ます')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 20, bottom: 20 })
        .margin({ bottom: 22 })
    } else {
      Column() {
        ForEach(this.dictSuggestions, (row: string) => {
          Row({ space: 8 }) {
            Column({ space: 2 }) {
              Text(row.split('\t')[1])
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(16)
                .fontWeight(FontWeight.Medium)
                .fontColor(kbTheme(this.isDark).textPrimary)
                .width('100%')
              Text(`${row.split('\t')[0]}   組み立て ${row.split('\t')[2]}回`)
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(10)
                .fontColor(kbTheme(this.isDark).textSecondary)
                .width('100%')
            }
            .layoutWeight(1)
            Text('登録')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(12)
              .fontWeight(FontWeight.Bold)
              .fontColor(kbTheme(this.isDark).onAccent)
              .backgroundColor(kbTheme(this.isDark).accent)
              .borderRadius(8)
              .padding({ left: 14, right: 14, top: 6, bottom: 6 })
              .onClick(() => {
                const p = row.split('\t');
                InputHandler.getInstance().registerUserWord(p[0], p[1]);
                promptAction.showToast({ message: `${p[1]} を登録しました` });
              })
            Text('✕')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(12)
              .width(28).height(28)
              .textAlign(TextAlign.Center)
              .fontColor(kbTheme(this.isDark).textSecondary)
              .onClick(() => {
                const p = row.split('\t');
                InputHandler.getInstance().dismissUnknownWord(p[0], p[1]);
              })
          }
          .width('100%')
          .padding({ top: 10, bottom: 10 })
        }, (row: string) => row)
      }
      .backgroundColor(kbTheme(this.isDark).cardBg)
      .borderRadius(CARD_RADIUS)
      .padding({ left: 14, right: 14 })
    }
  }

  // 候補の長押しで削除した語。ここから元に戻せる。
  @Builder SuppressedView() {
    Row() {
      Text('変換候補から消した語')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .letterSpacing(0.6)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .padding({ left: 4 })
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
      if (this.suppressedRows.length > 0) {
        Text('すべて戻す')
          .fontFamily(JP_FONT_FAMILY)
          .fontSize(12)
          .fontColor(kbTheme(this.isDark).accent)
          .padding({ left: 8 })
          .onClick(() => { InputHandler.getInstance().clearAllSuppressed(); })
      }
    }
    .width('100%')
    .padding({ bottom: 8 })
    .alignItems(VerticalAlign.Center)

    if (this.suppressedRows.length === 0) {
      Text('ありません\n候補を長押しして削除すると、ここに出ます')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 20, bottom: 20 })
    } else {
      Column() {
        ForEach(this.suppressedRows, (row: string) => {
          Row({ space: 8 }) {
            Column({ space: 2 }) {
              Text(row.split('\t')[1])
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(16)
                .fontColor(kbTheme(this.isDark).textPrimary)
                .width('100%')
              Text(row.split('\t')[0])
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(10)
                .fontColor(kbTheme(this.isDark).textSecondary)
                .width('100%')
            }
            .layoutWeight(1)
            Text('戻す')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(12)
              .fontWeight(FontWeight.Bold)
              .fontColor(kbTheme(this.isDark).onAccent)
              .backgroundColor(kbTheme(this.isDark).accent)
              .borderRadius(8)
              .padding({ left: 14, right: 14, top: 6, bottom: 6 })
              .onClick(() => {
                const p = row.split('\t');
                InputHandler.getInstance().restoreSuppressedWord(p[0], p[1]);
              })
          }
          .width('100%')
          .padding({ top: 10, bottom: 10 })
        }, (row: string) => row)
      }
      .backgroundColor(kbTheme(this.isDark).cardBg)
      .borderRadius(CARD_RADIUS)
      .padding({ left: 14, right: 14 })
    }
  }

  // 登録済みのユーザー辞書。
  @Builder DictRegisteredView() {
    if (this.dictRegistered.length === 0) {
      Text('まだありません')
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(12)
        .fontColor(kbTheme(this.isDark).textSecondary)
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 20, bottom: 20 })
    } else {
      Column() {
        ForEach(this.dictRegistered, (row: string) => {
          Row({ space: 8 }) {
            Column({ space: 2 }) {
              Text(row.split('\t')[1])
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(16)
                .fontColor(kbTheme(this.isDark).textPrimary)
                .width('100%')
              Text(row.split('\t')[0])
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(10)
                .fontColor(kbTheme(this.isDark).textSecondary)
                .width('100%')
            }
            .layoutWeight(1)
            Text('削除')
              .fontFamily(JP_FONT_FAMILY)
              .fontSize(12)
              .fontColor(kbTheme(this.isDark).deleteRed)
              .padding({ left: 10, right: 4, top: 6, bottom: 6 })
              .onClick(() => {
                const p = row.split('\t');
                InputHandler.getInstance().removeUserWord(p[0], p[1]);
              })
          }
          .width('100%')
          .padding({ top: 10, bottom: 10 })
        }, (row: string) => row)
      }
      .backgroundColor(kbTheme(this.isDark).cardBg)
      .borderRadius(CARD_RADIUS)
      .padding({ left: 14, right: 14 })
    }
  }

}

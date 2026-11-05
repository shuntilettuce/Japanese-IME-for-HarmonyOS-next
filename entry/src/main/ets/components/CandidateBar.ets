import { InputHandler } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';
import { promptAction } from '@kit.ArkUI';

@Component
export struct CandidateBar {
  @Prop candidates: string[] = [];
  @Prop selectedIndex: number = 0;
  @Prop showExpand: boolean = false;
  // 呼び出し側 (KeyboardPage) が比率計算した実際のバー高さ (vp)。以前は
  // ここが isLandscape の2択固定 (34/22) だったため、外側のバーが比率で
  // 縮んだときにチップだけ元の固定サイズのままはみ出て潰れて見えた。
  // チップ・フォント・矢印ボタンをすべてこの値から逆算する。
  @Prop barHeightVp: number = 48;
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('isLandscape') isLandscape: boolean = false;
  // まだ LEARN_THRESHOLD 未満の未知語(準候補)。通常候補と見分けが付くよう
  // 点線の縁取りだけ足す (UnknownWordLearner.quasiCandidates 参照)。
  @StorageLink('quasiCandidateSurfaces') quasiCandidateSurfaces: string[] = [];
  // 提案 (打ち間違いの直し・日付・住所・絵文字…)。候補の帯の 1 位のすぐ後ろに、青枠で見分けられるようにして並べる
  // (ひらがなの帯の中に分けて出すのはやめた。ユーザー 2026-10-15)
  @StorageLink('suggestList') suggestList: string[] = [];
  @StorageLink('suggestKinds') suggestKinds: number[] = [];
  @StorageLink('typoTop') typoTop: string = '';   // 1 位にした打ち間違いの直し (“ ” で囲んで見せる)
  @StorageLink('suggestFrom') suggestFrom: string = '';
  // Which chip is showing its 削除 popup (-1 = none), and the text that chip
  // holds. Kept as state rather than passed into the popup builder because a
  // parameterised @Builder inside ForEach can capture a stale row -- the same
  // reason KeyboardView drives its one-handed hint popup from state.
  @State menuIndex: number = -1;
  @State menuCandidate: string = '';
  // 横スクロールの位置を入力ログに残すため (タップした候補がスクロールしないと
  // 見えない位置だったかの手がかり)。
  private scroller: Scroller = new Scroller();

  private closeMenu(): void {
    this.menuIndex = -1;
    this.menuCandidate = '';
  }

  private chipHeightVp(): number {
    return Math.max(18, this.barHeightVp - 6);
  }

  private chipFontSize(): number {
    return Math.max(10, Math.min(16, this.chipHeightVp() * 0.42));
  }

  @Builder DeleteMenu() {
    Row({ space: 6 }) {
      SymbolGlyph($r('sys.symbol.trash'))
        .fontSize(14)
        .fontColor([kbTheme(this.isDark).textPrimary])
      Text(`「${this.menuCandidate}」を候補から削除`)
        .fontFamily(JP_FONT_FAMILY)
        .fontSize(14)
        .fontWeight(FontWeight.Medium)
        .fontColor(kbTheme(this.isDark).textPrimary)
    }
    .padding({ left: 14, right: 14, top: 10, bottom: 10 })
    .onClick(() => {
      const removed = InputHandler.getInstance().suppressCandidate(this.menuIndex, this.menuCandidate);
      this.closeMenu();
      if (removed.length > 0) {
        promptAction.showToast({ message: `「${removed}」を候補から削除しました（アプリの「削除した候補」から元に戻せます）` });
      }
    })
  }

  private suggestInBar(): boolean {
    return this.suggestList.length > 0 && this.candidates.length > 0;
  }

  private isTypoSugg(i: number): boolean {
    return i < this.suggestKinds.length && this.suggestKinds[i] === 1;
  }

  private suggestLabel(t: string, i: number): string {
    const k = i < this.suggestKinds.length ? this.suggestKinds[i] : 0;
    if (k === 1) { return `${this.suggestFrom}→${t}`; }
    if (k === 6 && t.startsWith(this.suggestFrom)) { return t.slice(this.suggestFrom.length); }
    return t;
  }

  // 帯の地の色を透明にした形 (もっと出すボタンの左のぼかし用)
  private clearOf(c: ResourceColor): string {
    const t = String(c);
    return /^#[0-9A-Fa-f]{6}$/.test(t) ? '#00' + t.slice(1) : 'rgba(0,0,0,0)';
  }

  build() {
    Stack({ alignContent: Alignment.End }) {
      Scroll(this.scroller) {
        Row({ space: 6 }) {
          ForEach(this.candidates, (candidate: string, index: number) => {
            if (index >= 2 && index - 2 < this.suggestList.length && this.suggestInBar()) {
              // 提案: 候補と交互に 1 つずつ (1 位・2 位の後ろから)。ひと固めにすると惜しい 2 位以降が遠くなった (ユーザー 2026-10-14)。
              // 縁と字を強調色に (✦ は青枠で足りるので外した、2026-10-14)。押すとその提案で確定 (学習はしない)
              Row({ space: 3 }) {
                // 地味に (派手すぎると指摘、2026-10-14): 打ち間違いの直しは普通の字を “ ” で囲む、ほか (日付など) は強調色の字だけ
                Text(this.isTypoSugg(index - 2) ? `“${this.suggestList[index - 2]}”` : this.suggestLabel(this.suggestList[index - 2], index - 2))
                  .fontFamily(JP_FONT_FAMILY)
                  .fontSize(this.chipFontSize())
                  .fontColor(kbTheme(this.isDark).textPrimary)   // 字は普通の候補と同じ、青枠だけで提案と分かる (2026-10-14)
              }
              .justifyContent(FlexAlign.Center)
              .alignItems(VerticalAlign.Center)
              .height(this.chipHeightVp())
              .padding({ left: this.isLandscape ? 10 : 18, right: this.isLandscape ? 10 : 18 })
              .backgroundColor(kbTheme(this.isDark).candidateBg)   // 地も普通の候補と同じ (2026-10-14)
              // 枠は残す (消すと余計に浮いた。ユーザー 2026-10-14)
              .borderRadius(this.chipHeightVp() / 2)
              .border({ width: 1, color: kbTheme(this.isDark).accent })
              .onClick(() => { InputHandler.getInstance().commitSuggestion(index - 2); })
            }
            Row() {
              Text(this.typoTop.length > 0 && candidate === this.typoTop ? `“${candidate}”` : candidate)
                .fontFamily(JP_FONT_FAMILY)
                .fontSize(this.chipFontSize())
                .fontColor(index === this.selectedIndex ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
            }
            .justifyContent(FlexAlign.Center)
            .alignItems(VerticalAlign.Center)
            .height(this.chipHeightVp())
            .padding({ left: this.isLandscape ? 10 : 18, right: this.isLandscape ? 10 : 18 })
            .borderRadius(this.chipHeightVp() / 2)
            .backgroundColor(index === this.selectedIndex ? kbTheme(this.isDark).accent : kbTheme(this.isDark).candidateBg)
            .border(this.quasiCandidateSurfaces.includes(candidate) ? {
              width: 1, color: kbTheme(this.isDark).textSecondary, style: BorderStyle.Dotted,
            } : { width: 0 })
            .bindPopup(this.menuIndex === index, {
              builder: this.DeleteMenu,
              placement: Placement.Top,
              enableArrow: true,
              popupColor: kbTheme(this.isDark).cardBg,
              backgroundBlurStyle: BlurStyle.NONE,
              radius: 20,
              // Masked: deleting is destructive enough to deserve a deliberate
              // second tap, and tapping the mask dismisses without deleting.
              mask: true,
              onStateChange: (e: PopupStateChangeParam) => {
                if (!e.isVisible && this.menuIndex === index) {
                  // 削除を押した場合は先に closeMenu 済みで menuIndex が -1 になって
                  // いるので、ここに来るのは削除せずに閉じたときだけ。
                  InputHandler.getInstance().noteCandidateMenu('cancel', index);
                  this.closeMenu();
                }
              },
            })
            // Long-press to offer deletion; a plain tap still commits via
            // onClick below, the same tap/hold split the space and shift keys use.
            .gesture(
              LongPressGesture({ duration: 400 })
                .onAction(() => {
                  this.menuCandidate = candidate;
                  this.menuIndex = index;
                  InputHandler.getInstance().noteCandidateMenu('open', index);
                })
            )
            .onClick(() => {
              InputHandler.getInstance().commitCandidate(index);
            })
          }, (candidate: string, index: number) => `${index}-${candidate}-${candidate === this.typoTop ? 'T' : ''}-${this.suggestInBar() && index >= 2 ? (this.suggestList[index - 2] ?? "") : ""}`)
        }
        .padding({ left: 4, right: 4 })
      }
      .scrollable(ScrollDirection.Horizontal)
      .onScrollStop(() => {
        InputHandler.getInstance().noteCandidateScroll(this.scroller.currentOffset().xOffset);
      })
      .scrollBar(BarState.Off)
      .width('100%')
      .height(this.barHeightVp)
      // もっと出すボタンの下に隠れる分、右に余白 (最後の候補もボタンの左まで送れる)
      .padding({ right: this.showExpand && this.candidates.length > 0 ? this.chipHeightVp() + 20 : 0 })

      if (this.showExpand && this.candidates.length > 0) {
        // 自前の「▼」ではなく HarmonyOS Symbol を使う (PR #58)。高さと
        // グリフサイズは固定値ではなく実際のバー高さから逆算する。
        // 候補の上に重ねて、左をぼかして帯に溶け込ませる (ボタンの所で候補がぷつっと切れて見えた。ユーザー 2026-10-14)
        Row() {
        Row() {
          SymbolGlyph($r('sys.symbol.chevron_down'))
            .fontSize(Math.max(9, Math.min(14, this.chipHeightVp() * 0.36)))
            .fontColor([kbTheme(this.isDark).textSecondary])
        }
        // 候補の札と同じ丸い札にして帯に馴染ませる (四角い濃い地で浮いていた。ユーザー 2026-10-14)
        .width(this.chipHeightVp())
        .height(this.chipHeightVp())
        .margin({ left: 2, right: 6 })
        .borderRadius(this.chipHeightVp() / 2)
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).candidateBg)
          .onClick(() => {
            InputHandler.getInstance().noteCandidateListToggle(true);
            AppStorage.setOrCreate('candidateExpanded', true);
          })
        }
        .height(this.barHeightVp)
        .padding({ left: 18 })
        .justifyContent(FlexAlign.End)
        .alignItems(VerticalAlign.Center)
        .linearGradient({
          angle: 90,
          colors: [[this.clearOf(kbTheme(this.isDark).previewBg), 0.0], [kbTheme(this.isDark).previewBg, 0.45]],
        })
        .hitTestBehavior(HitTestMode.Transparent)
      }
    }
    .width('100%')
    .height(this.barHeightVp)
    .backgroundColor(kbTheme(this.isDark).previewBg)
  }
}

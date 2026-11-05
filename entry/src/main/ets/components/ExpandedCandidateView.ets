import { InputHandler } from '../ime/KeyboardController';
import { kbTheme } from './Theme';
import { JP_FONT_FAMILY } from './JpFont';
import { promptAction } from '@kit.ArkUI';

@Component
export struct ExpandedCandidateView {
  @Prop candidates: string[] = [];
  @Prop selectedIndex: number = 0;
  @StorageLink('isDark') isDark: boolean = false;
  @StorageLink('quasiCandidateSurfaces') quasiCandidateSurfaces: string[] = [];
  @StorageLink('oneHandedMode') oneHandedMode: boolean = false;
  @StorageLink('oneHandedSide') oneHandedSide: string = 'right';
  // Same long-press-to-delete affordance as the candidate bar; see CandidateBar
  // for why the popup reads its text from state instead of a builder argument.
  @State menuIndex: number = -1;
  @State menuCandidate: string = '';

  private closeMenu(): void {
    this.menuIndex = -1;
    this.menuCandidate = '';
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

  build() {
    // One-handed mode docks the panel itself to that side (thumb-reachable),
    // the same intent as the keyboard leaning that way — but via actual width
    // (Blank() pushing it over) rather than a transform scale, so the grid
    // text renders at normal proportions instead of squished. Blank() on the
    // side the panel should NOT touch: right-handed -> panel hugs the right
    // edge, so Blank() goes on the left (and vice versa for left-handed).
    Row() {
      if (this.oneHandedMode && this.oneHandedSide === 'right') {
        Blank()
      }
      Column() {
        Row() {
          Text('候補一覧')
            .fontFamily(JP_FONT_FAMILY)
            .fontSize(14)
            .fontColor(kbTheme(this.isDark).textSecondary)
            .layoutWeight(1)
          Row() {
            SymbolGlyph($r('sys.symbol.xmark'))
              .fontSize(16)
              .fontColor([kbTheme(this.isDark).textSecondary])
          }
          .width(40).height(36)
          .justifyContent(FlexAlign.Center)
          .alignItems(VerticalAlign.Center)
            .onClick(() => {
              InputHandler.getInstance().noteCandidateListToggle(false);
              AppStorage.setOrCreate('candidateExpanded', false);
            })
        }
        .width('100%').height(36).padding({ left: 12, right: 4 }).alignItems(VerticalAlign.Center)
        .backgroundColor(kbTheme(this.isDark).panelBg)

        Scroll() {
          Grid() {
            ForEach(this.candidates, (candidate: string, index: number) => {
              GridItem() {
                Text(candidate)
                  .fontFamily(JP_FONT_FAMILY)
                  .fontSize(18)
                  .fontWeight(index === this.selectedIndex ? FontWeight.Bold : FontWeight.Normal)
                  .width('100%').textAlign(TextAlign.Center)
                  .padding({ top: 12, bottom: 12 }).borderRadius(8)
                  .backgroundColor(index === this.selectedIndex ? kbTheme(this.isDark).accent : kbTheme(this.isDark).candidateBg)
                  .fontColor(index === this.selectedIndex ? kbTheme(this.isDark).onAccent : kbTheme(this.isDark).textPrimary)
                  .border(this.quasiCandidateSurfaces.includes(candidate) ? {
                    width: 1, color: kbTheme(this.isDark).textSecondary, style: BorderStyle.Dotted,
                  } : { width: 0 })
              }
              .bindPopup(this.menuIndex === index, {
                builder: this.DeleteMenu,
                placement: Placement.Top,
                enableArrow: true,
                popupColor: kbTheme(this.isDark).cardBg,
                backgroundBlurStyle: BlurStyle.NONE,
                radius: 20,
                mask: true,
                onStateChange: (e: PopupStateChangeParam) => {
                  if (!e.isVisible && this.menuIndex === index) {
                    InputHandler.getInstance().noteCandidateMenu('cancel', index);
                    this.closeMenu();
                  }
                },
              })
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
                AppStorage.setOrCreate('candidateExpanded', false);
              })
            }, (candidate: string, index: number) => `${index}-${candidate}`)
          }
          // Fewer columns when docked narrower so tap targets stay a
          // comfortable size instead of cramming 4 columns into ~78% width.
          .columnsTemplate(this.oneHandedMode ? '1fr 1fr 1fr' : '1fr 1fr 1fr 1fr')
          .rowsGap(6).columnsGap(6)
          .padding({ left: 8, right: 8, top: 8, bottom: 8 })
        }
        .width('100%').layoutWeight(1).backgroundColor(kbTheme(this.isDark).panelBg).scrollBar(BarState.Off)
      }
      .width(this.oneHandedMode ? '78%' : '100%')
      .height('100%')
      .backgroundColor(kbTheme(this.isDark).panelBg)

      if (this.oneHandedMode && this.oneHandedSide === 'left') {
        Blank()
      }
    }
    .width('100%').height('100%')
  }
}

import dataPreferences from '@ohos.data.preferences';
import type common from '@ohos.app.ability.common';
// 設定の変更を入力ログに残す (デバッグビルドのみ。InputLog.ets 参照)。
import { InputLog } from './InputLog';

// Keyboard-wide settings persisted via @ohos.data.preferences, shared between
// the main app (settings UI) and the IME extension — same idiom as
// UserDictStore: the main app writes, the extension calls load(ctx, true) to
// force a fresh disk read past its own process's in-memory cache.
export interface KeyboardSettings {
  // Split so kana (ひらがな) and alphabet (アルファベット) input can each use
  // QWERTY or FLICK independently -- see load()'s legacyLayout migration note.
  kanaKeyboardLayout: string;
  alphabetKeyboardLayout: string;
  oneHandedMode: boolean;
  oneHandedSide: string;
  // 変換は AI (shuntelligence、AiConverter) だけ。前に保存した値に関わらず 'ai' (mozc・hybrid・独自辞書は 1.6.2 で撤去)
  conversionEngine: string;
  // Whether the "あA"/"Aあ" mode-switch key cycles 日本語→アルファベット→数字
  // (numeric pad) or just 日本語⇔アルファベット -- see InputHandler.cycleInputMode
  // and KeyboardView.ets/FlickKeyboardView.ets's ModeKey/LeftKey. Off by
  // default: most users reach digits fine from QWERTY/the number row.
  numericPadEnabled: boolean;
  // AI学習 (既定オフ): 確定した文から、変換の点に上乗せする小さいモデルを端末の中で育てる
  aiLearn: boolean;
  // キーを押したときに短く振動させるか。
  hapticEnabled: boolean;
  // 長い入力の扱い。false = 保留 (既定。前の方は候補を持たせて入力中のまま)、true = オート確定 (Mac の IME のように前から確定)
  // 提案 (打ち間違いの直し・日付・住所) を候補の帯に混ぜず、ひらがなの帯に分けて出す (既定オン)
  separateSuggest: boolean;
  // 英数字キーボードで英字も入力中の文字として組み立てて変換する (iphone -> iPhone)。オフは英字をそのまま入れる (数字は組み立てる)
  alphaCompose: boolean;
  // 差別的・不適切な語も候補に出す (既定オン)
  // キーボードのライト／ダーク。'system' は端末のカラーモードに従う（既定）。
  // 'light' / 'dark' はキーボードだけ固定する（ホストアプリの色は変えない）。
  themeMode: string;
  // 通常キー（英数・かな）の背景／影。既定はオン（現状どおり）。オフにすると
  // 文字キーだけ背景が消え、シフト／スペース／切り替え／エンター等は残る。
  keyChromeEnabled: boolean;
  // 入力中のかなをどこに出すか。2つは独立していて、組み合わせは4通り。
  //   panel=true  field=false : 従来どおりキーボード内だけに出す
  //   panel=false field=true  : 入力欄だけに出す (キーボードが1行ぶん低くなる)
  //   panel=true  field=true  : 両方に出す
  //   panel=false field=false : どちらにも出さない (設定画面で警告を出す)
  // field は入力欄側が setPreviewText に対応している場合のみ実際に効く。
  composingInPanel: boolean;
  composingInField: boolean;
  // 設定 → レイアウト調整。どちらも既定 1.0 で今までどおりの見た目になる。
  // keyboardHeightScale: キーボード全体の縦の長さの倍率 (0.7〜1.3、横向きは下限0.4)。
  // flickSideColumnRatio: フリック5列の「左右特殊キー1列 / 中央かなキー1列」
  // の幅比 (0.4〜2.0)。QWERTY側の特殊キー幅には影響しない。
  // 縦向き/横向きで持ちたい値が違う(横は画面が横長で背が低くなりがち、特殊キー
  // 列の見え方も違う)ため、Portrait/Landscape で別々に保持する。
  keyboardHeightScalePortrait: number;
  keyboardHeightScaleLandscape: number;
  flickSideColumnRatioPortrait: number;
  flickSideColumnRatioLandscape: number;
  // タブレット/フォルダブルの展開画面など、画面の短辺が600vp超の端末だけに
  // 効く「キーボードの最大幅」の倍率 (既定 1.0 = 620vp、KeyboardController の
  // MAX_KB_VP)。端末を実際に持っていないと画面比率が分からず、開発側からは
  // 一律の見た目に固定できない --「タブレット/フォルダブルで幅が合わない」
  // という声に対して、その場で自分で追い込めるようにするための調整項目。
  // 通常のスマホ (短辺600vp以下) では常に無効 (最大幅の制約自体が働かない)。
  keyboardWidthScale: number;
  // キーボード本体を左右どちらへ寄せるか。-1 = 左端いっぱい、0 = 中央
  // (従来どおり)、+1 = 右端いっぱい。幅の上限を絞って画面より狭くなった
  // ぶんの余白を、どちら側に寄せるかを決める。タブレット/フォルダブルの
  // 展開画面では中央に置くと両手が届かず、左右どちらかに寄せた方が打ち
  // やすいことがあるため (「左右に空白を入れれるように」)。余白が無い
  // (キーボードが画面幅いっぱい) ときは何も起きない。
  keyboardHorizontalOffset: number;
  // フリックキーの上下左右に出す小さいかなヒント(十字表示)。フリック自体は
  // タッチのジェスチャで判定しており、このヒントを消しても機能は一切
  // 変わらない (FlickKeyboardView の isLandscape 分岐と同じ仕組みを流用)。
  // 使い慣れた人には邪魔になるという要望向けのオフスイッチ。既定はオン。
  flickHintsEnabled: boolean;
  // 括弧の入れ方 (KeyboardController の BRACKET_PAIRS 参照)。
  //   'pair'   = 既定。開きと閉じを一緒に入れて、カーソルを中へ
  //   'manual' = 開きだけ入れて閉じは保留。► キー・待機バッジのタップ・送信直前に、その位置へ閉じを出す
  //   前に保存した 'auto' は 'pair' として扱う (ユーザー 2026-10-13)
  bracketCloseMode: string;
  // フローティングキーボード。画面下端に固定せず、任意の位置・大きさで
  // 浮かせて使えるモード。位置・大きさは画面の縦横比に依存させたくない
  // (回転・折りたたみで画面サイズ自体が変わるため) ので、絶対 vp ではなく
  // 画面に対する割合 (0〜1) で持つ。KeyboardController.floatingRectVp が
  // 表示のたびにその時点の画面サイズへ変換する。
  floatingMode: boolean;
  floatingXFrac: number;
  floatingYFrac: number;
  floatingWFrac: number;
  floatingHFrac: number;
}

const DEFAULTS: KeyboardSettings = {
  kanaKeyboardLayout: 'QWERTY',
  alphabetKeyboardLayout: 'QWERTY',
  oneHandedMode: false,
  oneHandedSide: 'right',
  conversionEngine: 'ai',
  numericPadEnabled: false,
  aiLearn: false,
  hapticEnabled: false,
  separateSuggest: true,
  alphaCompose: true,
  themeMode: 'system',
  keyChromeEnabled: true,
  composingInPanel: true,
  composingInField: true,
  keyboardHeightScalePortrait: 1.0,
  keyboardHeightScaleLandscape: 1.0,
  flickSideColumnRatioPortrait: 1.0,
  flickSideColumnRatioLandscape: 1.0,
  keyboardWidthScale: 1.0,
  keyboardHorizontalOffset: 0,
  flickHintsEnabled: true,
  bracketCloseMode: 'pair',
  floatingMode: false,
  // 既定は画面上寄り、幅60%・高さ35%程度の常識的なサイズ。設定ボタンの
  // 長押し→上スライドで出すジェスチャの向き(上へ引き出す)に合わせて
  // 上寄りにしてある(「フローティング、上に出せない?」の指摘)。
  floatingXFrac: 0.2,
  floatingYFrac: 0.12,
  floatingWFrac: 0.6,
  floatingHFrac: 0.35,
};

export class SettingsStore {
  private static readonly STORE: string = 'shunti_settings';

  static async load(context: common.Context, forceReload: boolean = false): Promise<KeyboardSettings> {
    try {
      if (forceReload) {
        await dataPreferences.removePreferencesFromCache(context, SettingsStore.STORE);
      }
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      // Legacy single 'keyboardLayout' key from before kana/alphabet were
      // split -- read it as the fallback default for both new keys, so an
      // existing install's FLICK/QWERTY choice carries over unchanged until
      // the user picks them independently (at which point the new keys win).
      const legacyLayout = await prefs.get('keyboardLayout', DEFAULTS.kanaKeyboardLayout);
      const kanaLayout = await prefs.get('kanaKeyboardLayout', legacyLayout);
      const alphabetLayout = await prefs.get('alphabetKeyboardLayout', legacyLayout);
      const oneHanded = await prefs.get('oneHandedMode', DEFAULTS.oneHandedMode);
      const side = await prefs.get('oneHandedSide', DEFAULTS.oneHandedSide);
      const numericPad = await prefs.get('numericPadEnabled', DEFAULTS.numericPadEnabled);
      const aiLearn = await prefs.get('aiLearn', DEFAULTS.aiLearn);
      const haptic = await prefs.get('hapticEnabled', DEFAULTS.hapticEnabled);
      const separateSuggest = await prefs.get('separateSuggest', DEFAULTS.separateSuggest);
      const alphaCompose = await prefs.get('alphaCompose', DEFAULTS.alphaCompose);
      const themeMode = await prefs.get('themeMode', DEFAULTS.themeMode);
      const keyChrome = await prefs.get('keyChromeEnabled', DEFAULTS.keyChromeEnabled);
      const inPanel = await prefs.get('composingInPanel', DEFAULTS.composingInPanel);
      const inField = await prefs.get('composingInField', DEFAULTS.composingInField);
      // 旧・縦横共用キーからの移行: 新キーが無い端末では旧値を両方向の初期値に
      // 引き継ぐ。新キーが一度でも保存されればそちらが優先される。
      const legacyHeightScale = await prefs.get('keyboardHeightScale', DEFAULTS.keyboardHeightScalePortrait);
      const legacyFlickRatio = await prefs.get('flickSideColumnRatio', DEFAULTS.flickSideColumnRatioPortrait);
      const heightScaleP = await prefs.get('keyboardHeightScalePortrait', legacyHeightScale);
      const heightScaleL = await prefs.get('keyboardHeightScaleLandscape', legacyHeightScale);
      const flickRatioP = await prefs.get('flickSideColumnRatioPortrait', legacyFlickRatio);
      const flickRatioL = await prefs.get('flickSideColumnRatioLandscape', legacyFlickRatio);
      const widthScale = await prefs.get('keyboardWidthScale', DEFAULTS.keyboardWidthScale);
      const hOffset = await prefs.get('keyboardHorizontalOffset', DEFAULTS.keyboardHorizontalOffset);
      const flickHints = await prefs.get('flickHintsEnabled', DEFAULTS.flickHintsEnabled);
      const bracketMode = await prefs.get('bracketCloseMode', DEFAULTS.bracketCloseMode);
      const floating = await prefs.get('floatingMode', DEFAULTS.floatingMode);
      const floatX = await prefs.get('floatingXFrac', DEFAULTS.floatingXFrac);
      const floatY = await prefs.get('floatingYFrac', DEFAULTS.floatingYFrac);
      const floatW = await prefs.get('floatingWFrac', DEFAULTS.floatingWFrac);
      const floatH = await prefs.get('floatingHFrac', DEFAULTS.floatingHFrac);
      console.info('shuntiIME SettingsStore.load ok', JSON.stringify({ kanaLayout, alphabetLayout, oneHanded, side, numericPad, themeMode, keyChrome, forceReload }));
      return {
        kanaKeyboardLayout: String(kanaLayout),
        alphabetKeyboardLayout: String(alphabetLayout),
        oneHandedMode: oneHanded === true,
        oneHandedSide: String(side),
        // 変換方式は AI だけ (独自辞書・統計データ・ハイブリッドの旧エンジンは 1.6.1 の次で外した)。前に別のものを選んで
        // 保存していた人も AI にする
        conversionEngine: DEFAULTS.conversionEngine,
        numericPadEnabled: numericPad === true,
        aiLearn: aiLearn === true,
        hapticEnabled: haptic === true,
        separateSuggest: separateSuggest !== false,
        alphaCompose: alphaCompose !== false,
        themeMode: String(themeMode),
        keyChromeEnabled: keyChrome !== false,
        composingInPanel: inPanel !== false,
        composingInField: inField !== false,
        keyboardHeightScalePortrait: Number(heightScaleP),
        keyboardHeightScaleLandscape: Number(heightScaleL),
        flickSideColumnRatioPortrait: Number(flickRatioP),
        flickSideColumnRatioLandscape: Number(flickRatioL),
        keyboardWidthScale: Number(widthScale),
        keyboardHorizontalOffset: Number(hOffset),
        flickHintsEnabled: flickHints !== false,
        bracketCloseMode: String(bracketMode),
        floatingMode: floating === true,
        floatingXFrac: Number(floatX),
        floatingYFrac: Number(floatY),
        floatingWFrac: Number(floatW),
        floatingHFrac: Number(floatH),
      };
    } catch (e) {
      console.error('shuntiIME SettingsStore.load error', JSON.stringify(e));
      return {
        kanaKeyboardLayout: DEFAULTS.kanaKeyboardLayout,
        alphabetKeyboardLayout: DEFAULTS.alphabetKeyboardLayout,
        oneHandedMode: DEFAULTS.oneHandedMode,
        oneHandedSide: DEFAULTS.oneHandedSide,
        conversionEngine: DEFAULTS.conversionEngine,
        numericPadEnabled: DEFAULTS.numericPadEnabled,
        aiLearn: DEFAULTS.aiLearn,
        hapticEnabled: DEFAULTS.hapticEnabled,
        separateSuggest: DEFAULTS.separateSuggest,
        alphaCompose: DEFAULTS.alphaCompose,
        themeMode: DEFAULTS.themeMode,
        keyChromeEnabled: DEFAULTS.keyChromeEnabled,
        composingInPanel: DEFAULTS.composingInPanel,
        composingInField: DEFAULTS.composingInField,
        keyboardHeightScalePortrait: DEFAULTS.keyboardHeightScalePortrait,
        keyboardHeightScaleLandscape: DEFAULTS.keyboardHeightScaleLandscape,
        flickSideColumnRatioPortrait: DEFAULTS.flickSideColumnRatioPortrait,
        flickSideColumnRatioLandscape: DEFAULTS.flickSideColumnRatioLandscape,
        keyboardWidthScale: DEFAULTS.keyboardWidthScale,
        keyboardHorizontalOffset: DEFAULTS.keyboardHorizontalOffset,
        flickHintsEnabled: DEFAULTS.flickHintsEnabled,
        bracketCloseMode: DEFAULTS.bracketCloseMode,
        floatingMode: DEFAULTS.floatingMode,
        floatingXFrac: DEFAULTS.floatingXFrac,
        floatingYFrac: DEFAULTS.floatingYFrac,
        floatingWFrac: DEFAULTS.floatingWFrac,
        floatingHFrac: DEFAULTS.floatingHFrac,
      };
    }
  }

  static async saveKanaKeyboardLayout(context: common.Context, layout: string): Promise<void> {
    InputLog.recordSetting('kanaKeyboardLayout', String(layout));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('kanaKeyboardLayout', layout);
      await prefs.flush();
      console.info('shuntiIME SettingsStore.saveKanaKeyboardLayout ok', layout);
    } catch (e) {
      console.error('shuntiIME SettingsStore.saveKanaKeyboardLayout error', JSON.stringify(e));
    }
  }

  static async saveAlphabetKeyboardLayout(context: common.Context, layout: string): Promise<void> {
    InputLog.recordSetting('alphabetKeyboardLayout', String(layout));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('alphabetKeyboardLayout', layout);
      await prefs.flush();
      console.info('shuntiIME SettingsStore.saveAlphabetKeyboardLayout ok', layout);
    } catch (e) {
      console.error('shuntiIME SettingsStore.saveAlphabetKeyboardLayout error', JSON.stringify(e));
    }
  }

  static async saveOneHanded(context: common.Context, enabled: boolean, side: string): Promise<void> {
    InputLog.recordSetting('oneHanded', JSON.stringify([enabled, side]));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('oneHandedMode', enabled);
      await prefs.put('oneHandedSide', side);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveNumericPadEnabled(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('numericPadEnabled', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('numericPadEnabled', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveAiLearn(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('aiLearn', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('aiLearn', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveHapticEnabled(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('hapticEnabled', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('hapticEnabled', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveKeyChromeEnabled(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('keyChromeEnabled', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('keyChromeEnabled', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveThemeMode(context: common.Context, mode: string): Promise<void> {
    InputLog.recordSetting('themeMode', String(mode));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('themeMode', mode);
      await prefs.flush();
      console.info('shuntiIME SettingsStore.saveThemeMode ok', mode);
    } catch (e) {
      console.error('shuntiIME SettingsStore.saveThemeMode error', JSON.stringify(e));
    }
  }

  static async saveKeyboardHeightScale(context: common.Context, scale: number, isLandscape: boolean): Promise<void> {
    InputLog.recordSetting('keyboardHeightScale', JSON.stringify([scale, isLandscape]));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put(isLandscape ? 'keyboardHeightScaleLandscape' : 'keyboardHeightScalePortrait', scale);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveFlickSideRatio(context: common.Context, ratio: number, isLandscape: boolean): Promise<void> {
    InputLog.recordSetting('flickSideRatio', JSON.stringify([ratio, isLandscape]));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put(isLandscape ? 'flickSideColumnRatioLandscape' : 'flickSideColumnRatioPortrait', ratio);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveKeyboardWidthScale(context: common.Context, scale: number): Promise<void> {
    InputLog.recordSetting('keyboardWidthScale', String(scale));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('keyboardWidthScale', scale);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveKeyboardHorizontalOffset(context: common.Context, offset: number): Promise<void> {
    InputLog.recordSetting('keyboardHorizontalOffset', String(offset));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('keyboardHorizontalOffset', offset);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveFlickHintsEnabled(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('flickHintsEnabled', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('flickHintsEnabled', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveBracketCloseMode(context: common.Context, mode: string): Promise<void> {
    InputLog.recordSetting('bracketCloseMode', String(mode));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('bracketCloseMode', mode);
      await prefs.flush();
    } catch (_e) {}
  }

  static async saveFloatingMode(context: common.Context, enabled: boolean): Promise<void> {
    InputLog.recordSetting('floatingMode', String(enabled));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('floatingMode', enabled);
      await prefs.flush();
    } catch (_e) {}
  }

  // ドラッグ/リサイズのたびに呼ばれる想定なので4値まとめて1回で書く
  // (個別4メソッドに分けると保存のたびに4回 flush することになる)。
  static async saveFloatingRect(context: common.Context,
      xFrac: number, yFrac: number, wFrac: number, hFrac: number): Promise<void> {
    InputLog.recordSetting('floatingRect', JSON.stringify([xFrac, yFrac, wFrac, hFrac]));
    try {
      const prefs = await dataPreferences.getPreferences(context, SettingsStore.STORE);
      await prefs.put('floatingXFrac', xFrac);
      await prefs.put('floatingYFrac', yFrac);
      await prefs.put('floatingWFrac', wFrac);
      await prefs.put('floatingHFrac', hFrac);
      await prefs.flush();
    } catch (_e) {}
  }
}

// Reads colorMode off a Configuration object directly -- specifically the one
// handed to onConfigurationUpdate(newConfig), NOT a re-read of context.config.
// That distinction matters: nothing in the ability lifecycle contract
// guarantees context.config is kept mutated in place as the OS setting
// changes after the ability/extension was created -- it's confirmed live only
// at the ability/extension's own initial creation. Re-reading context.config
// later (whether from onConfigurationUpdate or from an onForeground/
// inputStart-style recheck) was the actual bug behind light/dark not
// tracking a live OS toggle: the recheck ran, but kept returning whatever
// color mode was active back at startup. The Configuration object
// onConfigurationUpdate is called with is the one value guaranteed to be the
// new state; isDarkFromResourceManager below is the fix for every other
// recheck point, which can't rely on that callback firing.
export function isDarkFromConfiguration(config: ESObject): boolean {
  try {
    const cm = config['colorMode'] as number;
    // ConfigurationConstant.ColorMode: COLOR_MODE_DARK=0, COLOR_MODE_LIGHT=1.
    return cm === 0;
  } catch (_e) {
    return false;
  }
}

// Reads the OS color mode via resourceManager.getConfigurationSync() instead
// of context.config -- unlike context.config (a snapshot, only guaranteed
// live at ability/extension creation -- see isDarkFromConfiguration's
// comment), resourceManager's Configuration re-resolves the current system
// setting on every call. This is the fix for isDark staying stuck until a
// full app/keyboard restart: calling this from every "about to show UI
// again" point (inputStart, onForeground) gets the live value without
// depending on onConfigurationUpdate actually firing, which HarmonyOS
// doesn't reliably deliver to a long-lived, mostly-idle IME extension.
export function isDarkFromResourceManager(context: ESObject): boolean {
  try {
    const cm = context['resourceManager'].getConfigurationSync()['colorMode'] as number;
    return cm === 0;
  } catch (_e) {
    return false;
  }
}

// Combines the user's テーマ setting with the live OS color mode.
// 'system' (and any unknown value) follows the OS; 'light'/'dark' pin the
// keyboard regardless of what the rest of the device is doing.
export function resolveIsDark(themeMode: string, osIsDark: boolean): boolean {
  if (themeMode === 'dark') { return true; }
  if (themeMode === 'light') { return false; }
  return osIsDark;
}

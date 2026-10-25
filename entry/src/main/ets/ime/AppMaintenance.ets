import dataPreferences from '@ohos.data.preferences';
import type common from '@ohos.app.ability.common';

// In-app "repair" for the case where a corrupt persisted store makes the app
// or keyboard crash on launch and the only known workaround is reinstalling
// (which just wipes the app's data directory). This clears the same
// @ohos.data.preferences stores from inside the app, so a user can recover
// without reinstalling and without losing the app itself.
//
// Two scopes are offered (see the two exported functions): a light reset that
// keeps the user's registered dictionary, and a full reset equivalent to a
// reinstall's data wipe. The loaders that read these stores are all
// individually hardened (UserDictStore/UserPosStore/ConjugationEngine), so
// this is a recovery convenience, not the only line of defense.
export class AppMaintenance {
  // Learned conversions, next-word/phrase habits, symbol-usage frequency: the
  // "it learned my habits" state. Safe to clear on a whim; regenerates with use.
  private static readonly LEARNING_STORES: string[] = [
    'shunti_learning',
    'shunti_nextword',
    'shunti_symbol_freq',
    'shunti_flow',
    'shunti_unknownword',
    'shunti_suppressed',
  ];

  // Everything the app persists. Order doesn't matter; each is deleted
  // independently and failures are swallowed (a store that never existed
  // yet still "succeeds" as far as the user's recovery goal).
  private static readonly ALL_STORES: string[] = [
    'shunti_learning',
    'shunti_nextword',
    'shunti_symbol_freq',
    'shunti_userdict',
    'shunti_userpos',
    'shunti_globaldict',
    'shunti_settings',
    'shunti_consent',
    'shunti_flow',
    'shunti_unknownword',
    'shunti_suppressed',
    'shunti_clip_pins',
  ];

  private static async clear(context: common.Context, names: string[]): Promise<void> {
    for (const name of names) {
      try {
        // Drop the in-process cache first so a later getPreferences can't
        // resurrect the just-deleted contents from memory.
        await dataPreferences.removePreferencesFromCache(context, name);
      } catch (_e) {}
      try {
        await dataPreferences.deletePreferences(context, name);
      } catch (_e) {}
    }
  }

  // Light repair: clear learning/frequency state only. Keeps the user's
  // registered words and settings.
  static async resetLearning(context: common.Context): Promise<void> {
    await AppMaintenance.clear(context, AppMaintenance.LEARNING_STORES);
  }

  // Full repair: wipe every app store (registered words, POS metadata,
  // settings, learning, consent). Equivalent to a reinstall's data reset,
  // but keeps the app installed. The caller is expected to confirm first.
  static async resetAll(context: common.Context): Promise<void> {
    await AppMaintenance.clear(context, AppMaintenance.ALL_STORES);
  }
}

// 候補の削除 (candidate suppression): a per-reading list of surfaces the user
// has explicitly removed via long-press → 削除, filtered out of the candidate
// list before it is shown.
//
// A suppression list rather than a deletion from any one source, because a
// candidate can arrive from any of DICTIONARY, dict.json, global_dict.json, the
// mozc engine, the user dictionary, conversion learning or unknown-word
// learning -- most of which are read-only bundled data with nothing to delete
// from. Filtering at the point of display is the only mechanism that works
// uniformly for all of them, and it stays correct when the underlying
// dictionaries are updated later.
//
// Deleting a candidate is expected to be permanent-feeling but never
// destructive: the learned records for the pair are purged separately by the
// caller (so a boost can't resurrect it), while the bundled dictionaries are
// untouched and the whole list can be cleared again from the settings panel.

import dataPreferences from '@ohos.data.preferences';

const MAX_READINGS = 5000;   // 以前は 500 (2026-10-24)
const MAX_PER_READING = 20;

// Shared by both processes: the IME extension writes here as candidates are
// deleted, and the main app's 削除した候補 screen reads/rewrites the same store
// to restore them. Exported so neither side can drift onto a different name.
export const SUPPRESSED_STORE: string = 'shunti_suppressed';
export const SUPPRESSED_KEY: string = 'v1';

export class SuppressedCandidates {
  // reading -> surfaces the user removed for that reading
  private static map: Record<string, string[]> = {};

  static setSuppressed(data: Record<string, string[]>): void {
    SuppressedCandidates.map = (data === undefined || data === null) ? {} : data;
  }

  static getSuppressed(): Record<string, string[]> {
    return SuppressedCandidates.map;
  }

  static clear(): void {
    SuppressedCandidates.map = {};
  }

  // How many readings currently have at least one removed candidate -- shown in
  // the settings panel so the reset entry can say whether there's anything to
  // reset.
  static count(): number {
    return Object.keys(SuppressedCandidates.map).length;
  }

  // Remove `surface` from `reading`'s candidates from now on. Returns false when
  // the request was refused: an empty pair, a list already at its cap, or an
  // attempt to suppress the reading's own kana -- that one is the guaranteed
  // fallback in filter(), so allowing it removed would let a reading end up with
  // nothing to commit at all.
  static suppress(reading: string, surface: string): boolean {
    if (reading.length === 0 || surface.length === 0) { return false; }
    if (surface === reading) { return false; }
    let list = SuppressedCandidates.map[reading];
    if (list === undefined) {
      if (Object.keys(SuppressedCandidates.map).length >= MAX_READINGS) { return false; }
      list = [];
      SuppressedCandidates.map[reading] = list;
    }
    if (list.indexOf(surface) >= 0) { return true; }
    if (list.length >= MAX_PER_READING) { return false; }
    list.push(surface);
    return true;
  }

  // Drop every suppressed surface from `cands`. If that would leave nothing to
  // show, the reading's own kana is returned instead of the original list --
  // committing what was typed is always possible, and a deleted candidate must
  // never reappear just because it happened to be the last one standing.
  static filter(reading: string, cands: string[]): string[] {
    const list = SuppressedCandidates.map[reading];
    if (list === undefined || list.length === 0) { return cands; }
    const kept = cands.filter((c: string) => list.indexOf(c) < 0);
    if (kept.length > 0) { return kept; }
    return [reading];
  }

  // ---- plain-map helpers for the main app's 削除した候補 screen ----
  // That screen edits a map loaded straight from disk (a different process
  // from the IME extension, so it must not touch the static above).

  // Restore one deleted candidate. Returns the updated map; a reading left with
  // nothing suppressed is dropped so `count()` stays truthful.
  static restore(
    map: Record<string, string[]>,
    reading: string,
    surface: string
  ): Record<string, string[]> {
    const list = map[reading];
    if (list === undefined) { return map; }
    const next = list.filter((s: string) => s !== surface);
    const out: Record<string, string[]> = {};
    for (const r of Object.keys(map)) {
      if (r !== reading) { out[r] = map[r]; }
    }
    if (next.length > 0) { out[reading] = next; }
    return out;
  }

  // Why a read failure was empty, or '' when the last read succeeded. The
  // screen showed a bare "削除した候補はありません" for every outcome -- store
  // unreadable, wrong path, genuinely nothing deleted -- which made "I can't
  // see my deleted candidates" impossible to tell apart from "there are none".
  static lastError: string = '';

  // Read the deletion list fresh from disk. forceReload drops the in-process
  // cache so writes made by the other process (app ⇄ IME) are picked up --
  // same contract as UserDictStore.load.
  static async load(
    context: ESObject,
    forceReload: boolean = false
  ): Promise<Record<string, string[]>> {
    SuppressedCandidates.lastError = '';
    try {
      if (forceReload) {
        await dataPreferences.removePreferencesFromCache(context, SUPPRESSED_STORE);
      }
      const prefs = await dataPreferences.getPreferences(context, SUPPRESSED_STORE);
      const raw = await prefs.get(SUPPRESSED_KEY, '{}');
      const parsed = JSON.parse(String(raw)) as Record<string, string[]>;
      const clean: Record<string, string[]> = {};
      if (parsed === null || typeof parsed !== 'object') { return clean; }
      for (const k of Object.keys(parsed)) {
        const v = parsed[k];
        if (Array.isArray(v) && v.length > 0) {
          const surfaces = v.filter((s: string) => typeof s === 'string' && s.length > 0);
          if (surfaces.length > 0) { clean[k] = surfaces; }
        }
      }
      return clean;
    } catch (e) {
      SuppressedCandidates.lastError = JSON.stringify(e);
      return {};
    }
  }

  static async save(context: ESObject, map: Record<string, string[]>): Promise<void> {
    try {
      const prefs = await dataPreferences.getPreferences(context, SUPPRESSED_STORE);
      await prefs.put(SUPPRESSED_KEY, JSON.stringify(map));
      await prefs.flush();
    } catch (_e) {}
  }
}

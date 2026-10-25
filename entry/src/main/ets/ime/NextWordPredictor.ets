// Next-word prediction (次単語予測): after a word is committed, offer the
// words most likely to come next so the user can tap instead of typing --
// the always-full suggestion strip a modern IME (Gboard等) shows the moment
// you finish a word. Three sources, merged in priority order:
//
//   1. LEARNED   - the user's own word→word transitions, recorded on every
//                  consecutive commit. Highest priority: it's the most direct
//                  signal of how THIS user actually writes. Capped, persisted
//                  alongside the conversion learning store.
//   2. SEED      - a curated table of high-value fixed continuations
//                  (ありがとう→ございます, よろしく→お願いします, 私→は…) so the
//                  feature is useful from the very first use, before any
//                  learning has accumulated (cold start).
//   3. PARTICLES - a universal fallback: after ANY content word (one ending in
//                  kanji or katakana, i.e. a noun/verb-stem, never a particle
//                  or okurigana tail) the case particles は/を/が/に/… are the
//                  overwhelmingly likely next tap in Japanese. This is what
//                  gives coverage over the long tail of nouns the seed can't
//                  enumerate.
//
// Deliberately keyed on plain committed surfaces (no POS tagging, no model):
// cheap, fully on-device, and it personalises with use. Only word-length
// commits (<= MAX_KEY_LEN) are learned/keyed, so a whole committed sentence
// never becomes a junk bigram key.

const MAX_KEY_LEN = 8;
const MAX_LEARNED_KEYS = 5000;   // 以前は 800 (2026-10-24)
const MAX_LEARNED_VALS_PER_KEY = 12;
const MAX_PREDICTIONS = 6;

// Case/topic particles offered after any content word, in rough
// everyday-frequency order. These are the universal-fallback predictions.
const GENERIC_PARTICLES: string[] = ['は', 'を', 'が', 'に', 'の', 'で', 'と', 'も'];

// Curated fixed continuations. Keyed on the exact committed surface. Kept to
// genuinely high-frequency, unambiguous next words -- greeting/set-phrase
// tails and the most common particle preferences for very common words. The
// generic-particle fallback already covers "noun → particle" broadly, so this
// table is for cases where a SPECIFIC word (not a bare particle) is the likely
// next tap, or where a word's preferred particle order differs from generic.
const SEED_NEXT: Record<string, string[]> = {
  // greeting / set-phrase continuations (you've committed the head, tap the tail)
  'ありがとう': ['ございます', 'ございました'],
  'おはよう': ['ございます'],
  'よろしく': ['お願いします', 'お願いいたします'],
  'お疲れ': ['様です', '様でした'],
  'おつかれ': ['様です', '様でした'],
  'お世話': ['になっております', 'になります'],
  'こんにち': ['は'],
  'こんばん': ['は'],
  'すみません': ['が', 'でした'],
  '申し訳': ['ございません', 'ありません'],
  'もうしわけ': ['ございません', 'ありません'],
  'いただき': ['ます', 'ました'],
  'よろしくお願い': ['します', 'いたします', '申し上げます'],
  // pronouns / very common nouns → preferred particles
  '私': ['は', 'が', 'の', 'も'],
  '僕': ['は', 'が', 'の', 'も'],
  '俺': ['は', 'が', 'も'],
  'あなた': ['は', 'が', 'の', 'も'],
  '彼': ['は', 'が', 'と', 'の'],
  '彼女': ['は', 'が', 'と', 'の'],
  'これ': ['は', 'を', 'が', 'で'],
  'それ': ['は', 'を', 'が', 'で'],
  'あれ': ['は', 'を', 'が'],
  'ここ': ['に', 'で', 'は', 'から'],
  'そこ': ['に', 'で', 'は'],
  // time nouns → topic は leads
  '今日': ['は', 'の', 'も', 'から'],
  '明日': ['は', 'の', 'から', 'まで'],
  '昨日': ['は', 'の', 'から'],
  '今': ['は', 'から', 'まで', 'すぐ'],
  '来週': ['は', 'の', 'から', 'まで'],
  '先週': ['は', 'の', 'から'],
  '今週': ['は', 'の', 'から', 'まで'],
  '来月': ['は', 'の', 'から', 'まで'],
  '今年': ['は', 'の', 'も'],
  // common everyday nouns
  '会社': ['に', 'で', 'は', 'の'],
  '学校': ['に', 'で', 'は', 'の'],
  '仕事': ['が', 'は', 'を', 'で'],
  '会議': ['は', 'が', 'を', 'で'],
  '電車': ['に', 'で', 'が', 'は'],
  '天気': ['が', 'は', 'も'],
  '時間': ['が', 'は', 'を', 'に'],
  '問題': ['が', 'は', 'を', 'も'],
  'ご飯': ['を', 'は', 'が'],
  // common verb renyou-stems → continuations
  '食べ': ['たい', 'ます', 'て', 'ました'],
  '行き': ['たい', 'ます', 'ました', 'ましょう'],
  '見': ['て', 'たい', 'ます', 'ました'],
  '帰り': ['ます', 'たい', 'ました'],
  '買い': ['たい', 'ます', 'に'],
  '飲み': ['たい', 'ます', 'に'],
  'し': ['て', 'ます', 'ました', 'たい'],
  // frequent adverb/連体詞 openers
  'ちょっと': ['待って', 'だけ'],
  'もう': ['少し', 'すぐ', 'いい'],
  'とても': ['いい', '良い'],
};

interface Scored {
  word: string;
  count: number;
}

// Persisted shape. v:2 carries both the 1-word (uni) and 2-word (bi) contexts;
// a bare Record (no `v`) is a pre-bigram save and loads as `uni` only.
interface LearnedStore {
  v: number;
  uni: Record<string, Record<string, number>>;
  bi: Record<string, Record<string, number>>;
}

// Unit-separator (U+001F) between the two context words in a bigram key;
// never occurs in committed text, so 映画+SEP+を can't collide with any
// other way of splitting the concatenation.
const BI_SEP = '';

export class NextWordPredictor {
  // 1-word context: prev-surface -> (next -> count).
  private static learned: Record<string, Record<string, number>> = {};
  // 2-word context: "prev2\x1fprev1" -> (next -> count). Sentence-level flow
  // (映画を→見る, not just を→…) lives here and outranks the 1-word guess.
  private static learnedBi: Record<string, Record<string, number>> = {};

  // Reserved unigram-store key (never a real committed surface, so it can't
  // collide with any word and it persists for free with the rest of `learned`).
  // Under it we count how often each word STARTS a sentence, so after the user
  // finishes one sentence the strip can offer their habitual next-sentence
  // openers (口癖 openers: てか/なんか/とりあえず…) instead of going blank.
  private static readonly START_KEY = '\u0002';

  static setLearned(data: LearnedStore | Record<string, Record<string, number>>): void {
    if (data === undefined || data === null) {
      NextWordPredictor.learned = {};
      NextWordPredictor.learnedBi = {};
      return;
    }
    const store = data as LearnedStore;
    if (store.v === 2 && store.uni !== undefined) {
      NextWordPredictor.learned = store.uni ?? {};
      NextWordPredictor.learnedBi = store.bi ?? {};
    } else {
      // pre-bigram flat map (unigram only) -> migrate in place
      NextWordPredictor.learned = data as Record<string, Record<string, number>>;
      NextWordPredictor.learnedBi = {};
    }
  }

  static getLearned(): LearnedStore {
    return { v: 2, uni: NextWordPredictor.learned, bi: NextWordPredictor.learnedBi };
  }

  private static isContentTail(surface: string): boolean {
    if (surface.length === 0) { return false; }
    const code = surface.charCodeAt(surface.length - 1);
    // CJK unified ideographs (kanji) or katakana -- a noun/verb-stem tail, not
    // a hiragana particle/okurigana. Case particles are the natural next tap.
    if (code >= 0x4e00 && code <= 0x9fff) { return true; }
    if (code >= 0x30a0 && code <= 0x30ff) { return true; }
    return false;
  }

  private static bump(store: Record<string, Record<string, number>>, key: string, next: string): void {
    let entry = store[key];
    if (entry === undefined) {
      const all = Object.keys(store);
      if (all.length >= MAX_LEARNED_KEYS) {
        // 上限では、いちばん使われていない語を追い出す (前は黙って学習を止めた。2026-10-25)
        let minKey = all[0];
        let minSum = Number.MAX_VALUE;
        for (const k of all) {
          let sum = 0;
          const e = store[k];
          for (const w of Object.keys(e)) { sum += e[w]; }
          if (sum < minSum) { minSum = sum; minKey = k; }
        }
        const kept: Record<string, Record<string, number>> = {};
        for (const k of all) { if (k !== minKey) { kept[k] = store[k]; } }
        if (store === NextWordPredictor.learned) { NextWordPredictor.learned = kept; } else { NextWordPredictor.learnedBi = kept; }
        store = kept;
      }
      entry = {};
      store[key] = entry;
    }
    entry[next] = (entry[next] ?? 0) + 1;
    const keys = Object.keys(entry);
    if (keys.length > MAX_LEARNED_VALS_PER_KEY) {
      let minWord = keys[0];
      for (const k of keys) { if (entry[k] < entry[minWord]) { minWord = k; } }
      const newEntry: Record<string, number> = {};
      for (const k of keys) {
        if (k !== minWord) {
          newEntry[k] = entry[k];
        }
      }
      store[key] = newEntry;
    }
  }

  // Record that `next` was committed after the context (prev2, prev1). prev2 may
  // be '' (sentence start / unknown): then only the 1-word transition is stored.
  // Only short word-sized surfaces are kept, so a whole sentence never becomes a
  // junk key.
  static record(prev2: string, prev1: string, next: string): void {
    if (prev1.length === 0 || next.length === 0) { return; }
    if (prev1.length > MAX_KEY_LEN || next.length > MAX_KEY_LEN) { return; }
    if (prev1 !== next) {
      NextWordPredictor.bump(NextWordPredictor.learned, prev1, next);
    }
    if (prev2.length > 0 && prev2.length <= MAX_KEY_LEN) {
      NextWordPredictor.bump(NextWordPredictor.learnedBi, prev2 + BI_SEP + prev1, next);
    }
  }

  // True when `surface` ends a sentence, so the next committed word begins a new
  // one (and is a sentence opener rather than a continuation).
  static isSentenceEnd(surface: string): boolean {
    if (surface.length === 0) { return false; }
    const last = surface[surface.length - 1];
    return last === '。' || last === '！' || last === '？'
      || last === '!' || last === '?' || last === '.' || last === '\n';
  }

  // Record that `word` began a sentence (口癖 opener). Kept under the reserved
  // START_KEY in the same unigram store, so it's capped and persisted like any
  // other learned transition. A punctuation-tailed surface is never an opener.
  static recordOpener(word: string): void {
    if (word.length === 0 || word.length > MAX_KEY_LEN) { return; }
    if (NextWordPredictor.isSentenceEnd(word)) { return; }
    NextWordPredictor.bump(NextWordPredictor.learned, NextWordPredictor.START_KEY, word);
  }

  // LEARNED-ONLY next surfaces for a context (no seed, no generic particles),
  // most-used first, 2-word context ahead of 1-word. This is the signal used to
  // rerank *conversion* candidates by real usage: unlike the hand-curated
  // CONTEXT_HINTS table (one entry per homophone), it generalises to every word
  // pair the user has actually written, so 映画を→見る, 資料を→作る etc. all get
  // disambiguated once the user has typed them, with no per-word coding.
  static learnedNext(prev1: string, prev2?: string): string[] {
    if (prev1.length === 0) { return []; }
    const out: string[] = [];
    const push = (w: string): void => {
      if (w.length > 0 && out.indexOf(w) < 0) { out.push(w); }
    };
    if (prev2 !== undefined && prev2.length > 0) {
      NextWordPredictor.rankedInto(NextWordPredictor.learnedBi[prev2 + BI_SEP + prev1], push);
    }
    NextWordPredictor.rankedInto(NextWordPredictor.learned[prev1], push);
    return out;
  }

  private static rankedInto(entry: Record<string, number> | undefined, push: (w: string) => void): void {
    if (entry === undefined) { return; }
    const scored: Scored[] = [];
    for (const w of Object.keys(entry)) { scored.push({ word: w, count: entry[w] }); }
    scored.sort((a: Scored, b: Scored) => b.count - a.count);
    for (const s of scored) { push(s.word); }
  }

  private static sortedScored(entry: Record<string, number> | undefined): Scored[] {
    if (entry === undefined) { return []; }
    const scored: Scored[] = [];
    for (const w of Object.keys(entry)) { scored.push({ word: w, count: entry[w] }); }
    scored.sort((a: Scored, b: Scored) => b.count - a.count);
    return scored;
  }

  // The single next word after (prev2, prev1) if it's a clear personal habit:
  // seen at least `minCount` times, AND at least twice as often as the next
  // runner-up (so a coin-flip between two equally common continuations never
  // gets treated as "the" habit). Bigram context preferred over unigram when
  // both have entries. Returns null if neither source clears the bar.
  private static dominantNext(prev1: string, prev2: string, minCount: number): string | null {
    let scored: Scored[] = [];
    if (prev2.length > 0) { scored = NextWordPredictor.sortedScored(NextWordPredictor.learnedBi[prev2 + BI_SEP + prev1]); }
    if (scored.length === 0) { scored = NextWordPredictor.sortedScored(NextWordPredictor.learned[prev1]); }
    if (scored.length === 0 || scored[0].count < minCount) { return null; }
    if (scored.length > 1 && scored[1].count * 2 > scored[0].count) { return null; }
    if (scored[0].word === prev1) { return null; }
    return scored[0].word;
  }

  // Greedily extends (prev2, prev1) through the learned graph as long as each
  // next step is a clear personal habit (dominantNext), up to maxWords long.
  // This is how a recurring personal phrase or sign-off -- 了解しましたやります,
  // マジで助かる, an habitual sentence-ending -- surfaces as ONE multi-word
  // completion once the user has typed it enough times, rather than requiring
  // a tap per word. Returns [] if even the first step isn't yet a habit (too
  // new / too varied); the caller falls back to ordinary single-word
  // predictions in that case. Loop-safe: stops if a word would repeat within
  // the chain (a real personal phrase never repeats a word immediately).
  static phraseCompletion(prev1: string, prev2: string, maxWords: number = 4, minCount: number = 2): string[] {
    const chain: string[] = [];
    let w1 = prev2;
    let w2 = prev1;
    for (let i = 0; i < maxWords; i++) {
      const next = NextWordPredictor.dominantNext(w2, w1, minCount);
      if (next === null || chain.indexOf(next) >= 0 || next === prev1 || next === prev2) { break; }
      chain.push(next);
      w1 = w2;
      w2 = next;
    }
    return chain;
  }

  // Predicted next words given the last one or two committed words, best first.
  // 2-word learned context leads (映画を→見る), then 1-word learned, then the
  // curated seed, then the universal particle fallback. prev2 optional.
  static predict(prev1: string, prev2?: string): string[] {
    if (prev1.length === 0) { return []; }
    const out: string[] = [];
    const push = (w: string): void => {
      if (w.length > 0 && w !== prev1 && out.indexOf(w) < 0 && out.length < MAX_PREDICTIONS) {
        out.push(w);
      }
    };
    // 1. 2-word learned context (highest signal)
    if (prev2 !== undefined && prev2.length > 0) {
      NextWordPredictor.rankedInto(NextWordPredictor.learnedBi[prev2 + BI_SEP + prev1], push);
    }
    // 2. 1-word learned context
    NextWordPredictor.rankedInto(NextWordPredictor.learned[prev1], push);
    // 3. curated seed
    const seed = SEED_NEXT[prev1];
    if (seed !== undefined) { for (const w of seed) { push(w); } }
    // 4. universal particle fallback after a content word
    if (NextWordPredictor.isContentTail(prev1)) {
      for (const p of GENERIC_PARTICLES) { push(p); }
    }
    return out;
  }
}

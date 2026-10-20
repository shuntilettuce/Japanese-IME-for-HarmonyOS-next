// Date/time predictive conversion (日付・時刻変換): typing きょう offers today's
// date, いま the current time, あした tomorrow's date, ことし this year — the
// "smart" reading→live-value expansion Japanese IMEs (ATOK, Google日本語入力)
// are expected to have and Gboard's generic engine does not do well. Includes
// the 和暦 (Reiwa era) form, which is distinctly useful for Japanese documents.
//
// Pure function of (reading, now): the current time is injected, never read
// inside, so the whole thing is deterministically unit-testable. Results are
// inserted as ALTERNATIVES (the normal conversion きょう→今日 stays candidate[0]);
// they are ephemeral values, so the caller must not feed them to conversion
// learning (see isNonLearningCandidate in KeyboardController).
//
// PC 版 (desktop/core/datetime.cpp) も同じ決まり (両方同じ中身にする)。

interface NumberPiece {
  k: string;
  kind: string;
  v: number;
}

interface Sokuon {
  k: string;
  v: number;
  unit: number;
}

interface OffsetEntry {
  k: string;
  off: number;
}

interface EraEntry {
  k: string;
  base: number;
}

interface SpecialDay {
  k: string;
  d: number;
}

interface MonthDayResult {
  year: number;
  month: number;
  day: number;
  hasYear: boolean;
}

const DOW: string[] = ['日', '月', '火', '水', '木', '金', '土'];
const DOW_FULL: string[] = ['日曜日', '月曜日', '火曜日', '水曜日', '木曜日', '金曜日', '土曜日'];

// 数の読み (かな)。長い方から (貪欲に当てる)
const NUMBER_PIECES: NumberPiece[] = [
  { k: 'きゅう', kind: 'd', v: 9 }, { k: 'じゅう', kind: 's', v: 10 }, { k: 'ひゃく', kind: 's', v: 100 },
  { k: 'びゃく', kind: 's', v: 100 }, { k: 'ぴゃく', kind: 's', v: 100 }, { k: 'ちょう', kind: 'b', v: 1000000000000 },
  { k: 'いち', kind: 'd', v: 1 }, { k: 'さん', kind: 'd', v: 3 }, { k: 'よん', kind: 'd', v: 4 },
  { k: 'ろく', kind: 'd', v: 6 }, { k: 'なな', kind: 'd', v: 7 }, { k: 'しち', kind: 'd', v: 7 },
  { k: 'はち', kind: 'd', v: 8 }, { k: 'れい', kind: 'd', v: 0 }, { k: 'ぜろ', kind: 'd', v: 0 },
  { k: 'いっ', kind: 'd', v: 1 }, { k: 'ろっ', kind: 'd', v: 6 }, { k: 'はっ', kind: 'd', v: 8 },
  { k: 'せん', kind: 's', v: 1000 }, { k: 'ぜん', kind: 's', v: 1000 }, { k: 'まん', kind: 'b', v: 10000 },
  { k: 'おく', kind: 'b', v: 100000000 },
  { k: 'に', kind: 'd', v: 2 }, { k: 'し', kind: 'd', v: 4 }, { k: 'ご', kind: 'd', v: 5 },
  { k: 'く', kind: 'd', v: 9 },
];
const SOKUON: Sokuon[] = [
  { k: 'じゅっ', v: 10, unit: 10 }, { k: 'じっ', v: 10, unit: 10 }, { k: 'ひゃっ', v: 100, unit: 100 },
  { k: 'せっ', v: 1000, unit: 1000 }, { k: 'いっ', v: 1, unit: 1 }, { k: 'ろっ', v: 6, unit: 1 },
  { k: 'はっ', v: 8, unit: 1 },
];

// Day-offset triggers: reading -> days from today.
const DAY_OFFSETS: Record<string, number> = {
  'きょう': 0, 'ほんじつ': 0,
  'あした': 1, 'あす': 1, 'みょうにち': 1,
  'きのう': -1, 'さくじつ': -1,
  'あさって': 2, 'みょうごにち': 2, 'しあさって': 3,
  'おととい': -2, 'いっさくじつ': -2, 'さきおととい': -3,
};

// Year-offset triggers: reading -> years from this year.
const YEAR_OFFSETS: Record<string, number> = {
  'ことし': 0, 'こんねん': 0,
  'らいねん': 1, 'みょうねん': 1,
  'きょねん': -1, 'さくねん': -1,
  'さらいねん': 2,
};

const MONTH_OFFSETS: Record<string, number> = {
  'こんげつ': 0, 'らいげつ': 1, 'せんげつ': -1,
};

// 曜日をつける読みの週 (さらいしゅう・らいしゅう・こんしゅう・せんしゅう)。週は月曜はじまり
const WEEKDAY_WEEKS: OffsetEntry[] = [
  { k: 'さらいしゅう', off: 2 }, { k: 'らいしゅう', off: 1 }, { k: 'こんしゅう', off: 0 }, { k: 'せんしゅう', off: -1 },
];

// 週の範囲 (月曜〜日曜)
const WEEK_RANGES: Record<string, number> = {
  'らいしゅう': 1, 'こんしゅう': 0, 'せんしゅう': -1,
};

// Current-time triggers.
const TIME_READINGS: Record<string, boolean> = {
  'いま': true, 'げんざい': true, 'ただいま': true,
};

const ERAS: EraEntry[] = [
  { k: 'れいわ', base: 2018 }, { k: 'へいせい', base: 1988 }, { k: 'しょうわ', base: 1925 },
];

const SPECIAL_DAYS: SpecialDay[] = [
  { k: 'ついたち', d: 1 }, { k: 'ふつか', d: 2 }, { k: 'みっか', d: 3 }, { k: 'よっか', d: 4 },
  { k: 'いつか', d: 5 }, { k: 'むいか', d: 6 }, { k: 'なのか', d: 7 }, { k: 'ようか', d: 8 },
  { k: 'ここのか', d: 9 }, { k: 'とおか', d: 10 }, { k: 'じゅうよっか', d: 14 }, { k: 'はつか', d: 20 },
  { k: 'にじゅうよっか', d: 24 }, { k: 'みそか', d: 30 },
];

export class DateTimePredictor {
  private static wareki(year: number, month: number, day: number): string | null {
    // Reiwa began 2019-05-01. Earlier eras are out of scope for a live-date
    // helper used in 2020s+; return null so no 和暦 variant is offered then.
    if (year > 2019 || (year === 2019 && (month > 5 || (month === 5 && day >= 1)))) {
      const r = year - 2018;
      return r === 1 ? '令和元年' : `令和${r}年`;
    }
    return null;
  }

  // 年の和暦 (令和・平成・昭和)。当たらなければ空
  private static eraYear(year: number): string {
    if (year >= 2019) { return year === 2019 ? '令和元年' : `令和${year - 2018}年`; }
    if (year >= 1989) { return year === 1989 ? '平成元年' : `平成${year - 1988}年`; }
    if (year >= 1926) { return year === 1926 ? '昭和元年' : `昭和${year - 1925}年`; }
    return '';
  }

  private static daysInMonth(year: number, month: number): number {
    return new Date(year, month, 0).getDate();
  }

  private static singleKana(c: string): number {
    if (c === 'に') { return 2; }
    if (c === 'し') { return 4; }
    if (c === 'ご') { return 5; }
    if (c === 'く') { return 9; }
    return -1;
  }

  // かなの数 (2 字以上)。読めなければ -1
  private static parseKana(seg: string): number {
    if (seg.length < 2) { return -1; }
    let body = seg;
    let sokuonValue = -1;
    let sokuonUnit = -1;
    for (const s of SOKUON) {
      if (seg.endsWith(s.k)) {
        sokuonValue = s.v;
        sokuonUnit = s.unit;
        body = seg.slice(0, seg.length - s.k.length);
        break;
      }
    }
    let i = 0;
    let prev = '';
    const kinds: string[] = [];
    const vals: number[] = [];
    while (i < body.length) {
      let hit = false;
      for (const p of NUMBER_PIECES) {
        if (!body.startsWith(p.k, i)) { continue; }
        if ((p.k === 'ぜん' || p.k === 'びゃく') && prev !== 'さん' && prev !== 'なん') { return -1; }
        if (p.k === 'ぴゃく' && prev !== 'ろっ' && prev !== 'はっ') { return -1; }
        kinds.push(p.kind);
        vals.push(p.v);
        prev = p.k;
        i += p.k.length;
        hit = true;
        break;
      }
      if (!hit) { return -1; }
    }
    if (sokuonValue >= 0) {
      if (sokuonUnit > 1 && sokuonValue === sokuonUnit) {
        kinds.push('s');
        vals.push(sokuonUnit);
      } else {
        kinds.push('d');
        vals.push(sokuonValue);
      }
    }
    let total = 0;
    let group = 0;
    let lastSmall = 100000;
    let hasDigit = false;
    let digit = 0;
    for (let j = 0; j < kinds.length; j++) {
      const kind = kinds[j];
      const v = vals[j];
      if (kind === 'd') {
        if (hasDigit) { return -1; }
        hasDigit = true;
        digit = v;
      } else if (kind === 's') {
        if (v >= lastSmall) { return -1; }
        group += (hasDigit ? digit : 1) * v;
        hasDigit = false;
        lastSmall = v;
      } else {
        group += hasDigit ? digit : 0;
        if (group === 0) { return -1; }
        total += group * v;
        group = 0;
        hasDigit = false;
        lastSmall = 100000;
      }
    }
    group += hasDigit ? digit : 0;
    const value = total + group;
    if (value === 0 && !(kinds.length === 1 && kinds[0] === 'd' && vals[0] === 0)) { return -1; }
    return value;
  }

  // 数の読み 1 語 (算用数字 or かな)。読めなければ -1
  private static parseNumberSeg(seg: string): number {
    if (seg.length === 0) { return -1; }
    let arabic = true;
    for (let i = 0; i < seg.length; i++) {
      const c = seg.charCodeAt(i);
      if (!((c >= 0x30 && c <= 0x39) || (c >= 0xff10 && c <= 0xff19))) { arabic = false; break; }
    }
    if (arabic) {
      let x = 0;
      for (let i = 0; i < seg.length; i++) {
        const c = seg.charCodeAt(i);
        x = x * 10 + (c >= 0xff10 ? c - 0xff10 : c - 0x30);
      }
      return x;
    }
    if (seg.length === 1) {
      return DateTimePredictor.singleKana(seg[0]);
    }
    return DateTimePredictor.parseKana(seg);
  }

  // 曜日の読み (にちようび…どようび / にちよう…どよう) -> 0=日..6=土。当たらなければ -1
  private static weekdayOf(s: string): number {
    const WDR: string[] = ['にち', 'げつ', 'か', 'すい', 'もく', 'きん', 'ど'];
    for (let w = 0; w < 7; w++) {
      if (s === `${WDR[w]}ようび` || s === `${WDR[w]}よう`) { return w; }
    }
    return -1;
  }

  private static dateForms(d: Date): string[] {
    const y = d.getFullYear();
    const m = d.getMonth() + 1;
    const day = d.getDate();
    const w = d.getDay();
    const dow = DOW[w];
    const dowf = DOW_FULL[w];
    const out: string[] = [
      `${y}年${m}月${day}日`,
      `${y}年${m}月${day}日(${dow})`,
      `${m}月${day}日`,
      `${m}月${day}日(${dow})`,
      `${m}月${day}日(${dowf})`,
      `${m}/${day}`,
      `${m}/${day}(${dow})`,
      `${y}/${m}/${day}`,
      `${y}-${m}-${day}`,
    ];
    const w_ = DateTimePredictor.wareki(y, m, day);
    if (w_ !== null) {
      out.push(`${w_}${m}月${day}日`);
      out.push(`${w_}${m}月${day}日(${dow})`);
    }
    out.push(`${y}${m < 10 ? '0' : ''}${m}${day < 10 ? '0' : ''}${day}`);   // 数字 8 桁 (20261021。PC 版と同じ)
    return out;
  }

  // <数>がつ<数>にち の日付に曜日をつけた形 (4 通り)
  private static dateDowForms(year: number, month: number, day: number): string[] {
    const d = new Date(year, month - 1, day);
    const w = d.getDay();
    const dow = DOW[w];
    const dowf = DOW_FULL[w];
    return [
      `${month}月${day}日(${dow})`,
      `${month}月${day}日(${dowf})`,
      `${month}/${day}(${dow})`,
      `${year}年${month}月${day}日(${dow})`,
    ];
  }

  private static timeForms(d: Date): string[] {
    const m = d.getMonth() + 1;
    const day = d.getDate();
    const h = d.getHours();
    const mm = d.getMinutes();
    const mm2 = mm < 10 ? `0${mm}` : `${mm}`;
    const ampm = h < 12 ? '午前' : '午後';
    let h12 = h % 12;
    if (h12 === 0) { h12 = 12; }
    return [
      `${h}:${mm2}`,
      `${h}時${mm}分`,
      `${ampm}${h12}時${mm}分`,
      `${m}月${day}日 ${h}:${mm2}`,
    ];
  }

  private static yearOffsetForms(y: number): string[] {
    const out: string[] = [`${y}年`];
    const w = DateTimePredictor.wareki(y, 6, 15); // mid-year: era is unambiguous
    if (w !== null) { out.push(w); }
    return out;
  }

  private static monthForms(y: number, m: number): string[] {
    return [`${y}年${m}月`, `${m}月`];
  }

  private static weekRangeForms(monday: Date): string[] {
    const sunday = new Date(monday.getFullYear(), monday.getMonth(), monday.getDate() + 6);
    const m1 = monday.getMonth() + 1;
    const d1 = monday.getDate();
    const m2 = sunday.getMonth() + 1;
    const d2 = sunday.getDate();
    return [
      `${m1}月${d1}日〜${m2}月${d2}日`,
      `${m1}/${d1}〜${m2}/${d2}`,
    ];
  }

  // <数>がつ<数>にち [<数>ねん 付きも]。曜日のついた日付の読み
  private static parseMonthDay(reading: string): MonthDayResult | null {
    const g = reading.indexOf('がつ');
    if (g < 0) { return null; }
    const before = reading.slice(0, g);
    const after = reading.slice(g + 2);
    let year = 0;
    let hasYear = false;
    let month = -1;
    const n = before.indexOf('ねん');
    if (n >= 0) {
      const yr = before.slice(0, n);
      const mo = before.slice(n + 2);
      let yv = -1;
      for (const e of ERAS) {
        if (!yr.startsWith(e.k)) { continue; }
        const nseg = yr.slice(e.k.length);
        yv = nseg === 'がん' ? 1 : DateTimePredictor.parseNumberSeg(nseg);
        if (yv < 0) { return null; }
        yv = e.base + yv;
        hasYear = true;
        break;
      }
      if (!hasYear) {
        yv = DateTimePredictor.parseNumberSeg(yr);
        if (yv < 1) { return null; }
        hasYear = true;
      }
      year = yv;
      const mv = DateTimePredictor.parseNumberSeg(mo);
      if (mv < 1 || mv > 12) { return null; }
      month = mv;
    } else {
      const mv = DateTimePredictor.parseNumberSeg(before);
      if (mv < 1 || mv > 12) { return null; }
      month = mv;
    }
    let day = -1;
    for (const s of SPECIAL_DAYS) {
      if (after === s.k) { day = s.d; break; }
    }
    if (day < 0 && after.endsWith('にち')) {
      const body = after.slice(0, after.length - 2);
      const dv = DateTimePredictor.parseNumberSeg(body);
      if (dv >= 1 && dv <= 31) { day = dv; }
    }
    if (day < 0) { return null; }
    return { year, month, day, hasYear };
  }

  // Live date/time candidates for `reading` as of `now`, or [] if the reading
  // isn't a date/time trigger.
  static predict(reading: string, now: Date): string[] {
    // 数字 8 桁の日付 (20261018 → 2026年10月18日(日) など)。ありえない日付は出さない (PC 版と同じ)
    const digits = reading.replace(/[０-９]/g, (c: string) => String.fromCharCode(c.charCodeAt(0) - 0xFEE0));
    if (/^\d{8}$/.test(digits)) {
      const y = parseInt(digits.substring(0, 4), 10);
      const m = parseInt(digits.substring(4, 6), 10);
      const dd = parseInt(digits.substring(6, 8), 10);
      const d8 = new Date(y, m - 1, dd);
      if (y >= 1900 && y <= 2100 && d8.getFullYear() === y && d8.getMonth() === m - 1 && d8.getDate() === dd) {
        return DateTimePredictor.dateForms(d8);
      }
    }
    const dayOff = DAY_OFFSETS[reading];
    if (dayOff !== undefined) {
      const d = new Date(now.getFullYear(), now.getMonth(), now.getDate() + dayOff);
      return DateTimePredictor.dateForms(d);
    }
    const yearOff = YEAR_OFFSETS[reading];
    if (yearOff !== undefined) {
      return DateTimePredictor.yearOffsetForms(now.getFullYear() + yearOff);
    }
    if (TIME_READINGS[reading]) {
      return DateTimePredictor.timeForms(now);
    }
    if (reading === 'ようび' || reading === 'なんようび') {
      const w = now.getDay();
      return [DOW_FULL[w], DOW[w], `(${DOW[w]})`];
    }
    const weekOff = WEEK_RANGES[reading];
    if (weekOff !== undefined) {
      const mon = new Date(now.getFullYear(), now.getMonth(), now.getDate() - (now.getDay() + 6) % 7 + weekOff * 7);
      return DateTimePredictor.weekRangeForms(mon);
    }
    const monthOff = MONTH_OFFSETS[reading];
    if (monthOff !== undefined) {
      const t = new Date(now.getFullYear(), now.getMonth() + monthOff, 1);
      return DateTimePredictor.monthForms(t.getFullYear(), t.getMonth() + 1);
    }
    // こんしゅうのげつようび・らいしゅうのきんようび … (の の有無どちらも、さらいしゅう も)
    for (const wk of WEEKDAY_WEEKS) {
      if (!reading.startsWith(wk.k)) { continue; }
      let rest = reading.slice(wk.k.length);
      if (rest.startsWith('の')) { rest = rest.slice(1); }
      const w = DateTimePredictor.weekdayOf(rest);
      if (w < 0) { continue; }
      const monOff = (now.getDay() + 6) % 7;
      const target = (w + 6) % 7;
      return DateTimePredictor.dateForms(new Date(now.getFullYear(), now.getMonth(), now.getDate() - monOff + target + wk.off * 7));
    }
    // 曜日だけ (げつようび …): 今日か次のその曜日
    const wOnly = DateTimePredictor.weekdayOf(reading);
    if (wOnly >= 0) {
      return DateTimePredictor.dateForms(new Date(now.getFullYear(), now.getMonth(), now.getDate() + (wOnly - now.getDay() + 7) % 7));
    }
    // <数>がつ<数>にち [<数>ねん 付きも]: 日付に曜日をつける
    const md = DateTimePredictor.parseMonthDay(reading);
    if (md !== null) {
      let year = md.hasYear ? md.year : now.getFullYear();
      if (!md.hasYear) {
        const target = new Date(year, md.month - 1, md.day);
        const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
        const diff = Math.round((today.getTime() - target.getTime()) / 86400000);
        if (diff >= 60) { year++; }
      }
      if (md.day > DateTimePredictor.daysInMonth(year, md.month)) { return []; }
      return DateTimePredictor.dateDowForms(year, md.month, md.day);
    }
    // <数>ねん (西暦 -> 和暦) / れいわ<数>ねん … (和暦 -> 西暦)
    if (reading.endsWith('ねん')) {
      const body = reading.slice(0, reading.length - 2);
      for (const e of ERAS) {
        if (!body.startsWith(e.k)) { continue; }
        const nseg = body.slice(e.k.length);
        const nv = nseg === 'がん' ? 1 : DateTimePredictor.parseNumberSeg(nseg);
        if (nv < 1) { return []; }
        const Y = e.base + nv;
        if (Y < 1) { return []; }
        return [`${Y}年`];
      }
      const Y = DateTimePredictor.parseNumberSeg(body);
      if (Y >= 1926 && Y <= 2100) {
        const w = DateTimePredictor.eraYear(Y);
        if (w.length > 0) { return [w]; }
      }
    }
    return [];
  }
}

import dataPreferences from '@ohos.data.preferences';
import fs from '@ohos.file.fs';
import image from '@ohos.multimedia.image';

// クリップボードの履歴 (2026-11-05 ユーザー: 画像・動画も見本つきで・保存する。Android 版 ClipStore と同じ考え)。
// 文と、画像の見本 (縮小した jpg) を保存する。画面には AppStorage 'clipItems' (ClipItem[]、新しい順) で配る。
// HarmonyOS の入力方式の拡張からはクリップボードに書けない (InputLog の注) ので、画像・ファイルを貼れるのは
// 今クリップボードにあるものだけ (入力欄のアプリに貼り付けを頼む)。貼れない古い画像・ファイルは残さない
export interface ClipItem {
  id: number;
  kind: string;    // 'text' | 'image' | 'video' | 'file'
  text: string;    // 文 (kind = text)
  name: string;    // ファイルの名前 (image・video・file。分からなければ空)
  thumb: string;   // 見本の jpg の場所 (無ければ空)
  sig: string;     // 同じものか (文はその文、画像・ファイルはコピーした時刻か URI)
  time: number;
}

const STORE: string = 'shunti_clip_history';
const KEY: string = 'v1';
const MAX_ITEMS: number = 50;
const MAX_DAYS: number = 30;
const THUMB_PX: number = 320;

export class ClipHistory {
  private static dir: string = '';
  private static ctx: ESObject = undefined;
  private static lastId: number = 0;

  static async load(context: ESObject, filesDir: string): Promise<void> {
    ClipHistory.ctx = context;
    ClipHistory.dir = filesDir + '/clips';
    try {
      if (!fs.accessSync(ClipHistory.dir)) { fs.mkdirSync(ClipHistory.dir); }
    } catch (_e) {
      // 作れなくても文の履歴は使える
    }
    try {
      const prefs = await dataPreferences.getPreferences(context, STORE);
      const raw = await prefs.get(KEY, '[]');
      const parsed = JSON.parse(String(raw)) as ClipItem[];
      const items = Array.isArray(parsed) ? parsed.filter((c: ClipItem) => c && typeof c.kind === 'string') : [];
      AppStorage.setOrCreate('clipItems', ClipHistory.prune(items));
    } catch (_e) {
      AppStorage.setOrCreate('clipItems', [] as ClipItem[]);
    }
  }

  static items(): ClipItem[] {
    return AppStorage.get<ClipItem[]>('clipItems') ?? [];
  }

  static addText(text: string): void {
    if (text.length === 0) { return; }
    ClipHistory.put({ id: 0, kind: 'text', text: text, name: '', thumb: '', sig: 'T:' + text, time: Date.now() } as ClipItem);
  }

  // 画像・動画・ファイル。sig が同じなら一番上へ動かすだけ (見本は前のものを使う)
  static has(sig: string): boolean {
    return ClipHistory.items().some((c: ClipItem) => c.sig === sig);
  }

  static addMedia(kind: string, name: string, sig: string, thumb: string): void {
    ClipHistory.put({ id: 0, kind: kind, text: '', name: name, thumb: thumb, sig: sig, time: Date.now() } as ClipItem);
  }

  // 画像の見本を縮小して保存し、その場所を返す (失敗したら '')
  static async saveThumb(pm: image.PixelMap): Promise<string> {
    if (ClipHistory.dir.length === 0) { return ''; }
    try {
      const info = await pm.getImageInfo();
      const m = Math.max(info.size.width, info.size.height);
      if (m > THUMB_PX) {
        const r = THUMB_PX / m;
        await pm.scale(r, r);
      }
      const packer = image.createImagePacker();
      const buf: ArrayBuffer = await packer.packing(pm, { format: 'image/jpeg', quality: 85 } as image.PackingOption);
      packer.release();
      const path = `${ClipHistory.dir}/${ClipHistory.newId()}.jpg`;
      const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, buf);
      fs.closeSync(f);
      return path;
    } catch (_e) {
      return '';
    }
  }

  static remove(id: number): void {
    const items = ClipHistory.items();
    const c = items.find((x: ClipItem) => x.id === id);
    if (!c) { return; }
    ClipHistory.dropThumb(c, items);
    ClipHistory.publish(items.filter((x: ClipItem) => x.id !== id));
  }

  static clear(): void {
    for (const c of ClipHistory.items()) { ClipHistory.dropThumb(c, []); }
    ClipHistory.publish([] as ClipItem[]);
  }

  // ---------------------------------------------------------------- 中
  private static newId(): number {
    ClipHistory.lastId = Math.max(Date.now(), ClipHistory.lastId + 1);
    return ClipHistory.lastId;
  }

  private static put(c: ClipItem): void {
    const items = ClipHistory.items();
    const old = items.find((x: ClipItem) => x.sig === c.sig);
    if (old && items[0] === old) { return; }   // 同じものが一番上にある
    c.id = ClipHistory.newId();
    if (old && c.thumb.length === 0) { c.thumb = old.thumb; }
    if (old && c.name.length === 0) { c.name = old.name; }
    // 画像・ファイルは今クリップボードにあるものしか貼れない (拡張から書けない)。貼れないものは出さない
    // (ユーザー 2026-11-05) ので、新しいものが来たら前の画像・ファイルは見本ごと消す
    const rest = items.filter((x: ClipItem) => x.sig !== c.sig && x.kind === 'text');
    for (const x of items) {
      if (x.kind !== 'text' && x.sig !== c.sig && x.thumb !== c.thumb) { ClipHistory.dropThumb(x, rest); }
    }
    ClipHistory.publish(ClipHistory.prune([c, ...rest]));
  }

  // 上限: 50 件・30 日 (古いものから。見本も消す)
  private static prune(items: ClipItem[]): ClipItem[] {
    const old = Date.now() - MAX_DAYS * 86400000;
    const keep = items.filter((c: ClipItem) => c.time >= old).slice(0, MAX_ITEMS);
    for (const c of items) {
      if (keep.indexOf(c) < 0) { ClipHistory.dropThumb(c, keep); }
    }
    return keep;
  }

  private static dropThumb(c: ClipItem, rest: ClipItem[]): void {
    if (c.thumb.length === 0 || rest.some((x: ClipItem) => x !== c && x.thumb === c.thumb)) { return; }
    try { fs.unlinkSync(c.thumb); } catch (_e) { /* もう無い */ }
  }

  private static publish(items: ClipItem[]): void {
    AppStorage.setOrCreate('clipItems', items);
    if (ClipHistory.ctx === undefined) { return; }
    dataPreferences.getPreferences(ClipHistory.ctx, STORE).then(async (prefs: dataPreferences.Preferences) => {
      await prefs.put(KEY, JSON.stringify(items));
      await prefs.flush();
    }).catch((_e: Error) => {});
  }
}

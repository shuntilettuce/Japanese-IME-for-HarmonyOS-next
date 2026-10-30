#!/usr/bin/env python3
"""同梱する日本語フォント (rawfile/font/NotoSansJP-Regular.ttf) を作り直す。

※ 置き換え済み (2026-10): 同梱フォントは tools/build_ud_font.py で作る (BIZ UDP ゴシック + 補いの Noto Sans JP)。ここは app_charset などの部品として残す。

Noto Sans JP の可変フォントを wght=400 で切り出し、このアプリが実際に描く文字
だけに絞って同梱する。

  元データ: https://github.com/google/fonts
            ofl/notosansjp/NotoSansJP[wght].ttf   (SIL OFL 1.1)

■ なぜ作り直すか

同梱していたファイルは Noto Sans JP を絞ったものだったが、絞りすぎていて
アプリが自分で描く文字まで落ちていた:

  ⏎ ⇧ ← → ↑ ↓        キーボードのキーラベルそのもの
  ① 〜 ⑳ ○ △ ★ ♪ ≦   記号パネルの ① / ◇ タブの中身そのもの
  ｱ ｲ ｶ ｸ …           半角カタカナ ([全] タブ)
  ㍻ ㍼ ㍽ ㍾ ㋿        元号合字
  α β Γ Δ …          ギリシャ文字
  𠮷 𠮟 𩸽 𥝱 兔 卄     JIS X 0213 第3・第4水準 (𠮷野, 𩸽 など実用のもの含む)
  神 福 祥 海 梅 …      互換漢字 (人名の旧字形。通常の U+795E 等とは別コード)

これらはシステムフォントに落ちていた。記号や矢印は字形が変わるだけだが、
漢字はシステムフォント = 中国語字形になるので、このフォントを同梱している
目的そのものが崩れる。

■ 「日本語の文字が2,700字も足りない」は誤りだった

最初この不足を数えたとき 2,777 字出てきたが、その大半は辞書側のゴミだった。
Unihan の kJapaneseOn (全CJK文字に付いている音読み) を読みとして取り込んだ跡で、
中国語専用字が日本語の音読みで引けてしまっていた ──「ほう」に 31件、
「げち」に 30件、「ない」に 12件。tools/clean_nonjapanese.py で除去済み。
除去後に本当に足りなかったのは上記の 454 字で、Noto Sans JP 本体は
日本語をきちんと網羅している。

■ サイズ

対象文字は「現在の同梱フォントの収録字」∪「アプリが描く字」。前者を必ず含める
ので、今出ている字が1つでも消えることはない。絵文字はシステムの絵文字フォントが
描くので入れない (入れると数MB増えて、しかも白黒になる)。

  4,003,068 -> 4,115,736 バイト (+112,668, +2.8%) で +454 字

縦書き用の vmtx/vhea は横書きしか描かないので落とす。

Usage:
  # 元の可変フォントを取ってくる (一度だけ)
  curl -sSLo /tmp/NotoSansJP.ttf \
    'https://raw.githubusercontent.com/google/fonts/main/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf'
  python3 tools/build_jp_font.py /tmp/NotoSansJP.ttf
"""
import glob
import json
import os
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUNDLED = os.path.join(ROOT, 'entry/src/main/resources/rawfile/font/NotoSansJP-Regular.ttf')
RAWFILE = os.path.join(ROOT, 'entry/src/main/resources/rawfile')

FAMILY = 'Noto Sans JP'
SUBFAMILY = 'Regular'
POSTSCRIPT = 'NotoSansJP-Regular'

# 横書きしか描かないので不要。
DROP_TABLES = ['vmtx', 'vhea']

# 辞書ソース (表記だけ拾う。読みはかななので文字集合に足しても増えない)
DICT_FILES = ['dict.json', 'global_dict.json', 'mozc_dict_surfaces.json']


def is_emoji(cp: int) -> bool:
    """絵文字だけを外す。範囲を広く取りすぎないこと。

    ここを `cp >= 0x1F000` と `0x2600..0x27BF` にしていたら、CJK拡張B
    (𠮟 U+20B9F, 𩸽 U+29E3D …) と ★ ♪ ♭ ♯ ☆ ♂ ♀ まで巻き込んで落ちていた。
    前者は JIS X 0213 の日本語、後者は記号パネルが出す文字で、どちらも要る。
    """
    return 0x1F000 <= cp <= 0x1FAFF


def app_charset() -> set:
    """アプリが画面に出しうる文字。UI文字列 (記号パネルの中身を含む) と辞書の表記。"""
    out = set()
    for p in glob.glob(os.path.join(ROOT, 'entry/src/main/ets/**/*.ets'), recursive=True):
        with open(p, encoding='utf-8') as f:
            out |= set(ord(c) for c in f.read())
    for name in DICT_FILES:
        path = os.path.join(RAWFILE, name)
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        for v in (d.values() if isinstance(d, dict) else d):
            for s in (v if isinstance(v, list) else [v]):
                if isinstance(s, str):
                    out |= set(ord(c) for c in s)
    return {c for c in out if not is_emoji(c) and c > 0x20}


def fix_names(f: TTFont) -> None:
    """可変フォントの既定インスタンス (Thin) の名残を消す。

    字形と OS/2 は Regular なのに name だけ "Noto Sans JP Thin" というファイルを
    同梱していた。registerFont は呼び出し側のエイリアスで引くので実害は出て
    いなかったが、内部名で引く経路に当たった瞬間に「登録したのに効かない」に化ける。
    """
    name = f['name']
    for rec in list(name.names):
        if rec.nameID == 1:
            rec.string = FAMILY
        elif rec.nameID == 2:
            rec.string = SUBFAMILY
        elif rec.nameID == 3:
            rec.string = f'{FAMILY}; {POSTSCRIPT}'
        elif rec.nameID == 4:
            rec.string = FAMILY
        elif rec.nameID == 6:
            rec.string = POSTSCRIPT
    name.names = [r for r in name.names if r.nameID not in (16, 17, 25)]


def verify(old_path: str, new_path: str) -> int:
    """今出ている字が1字も消えず、字形も1点も動いていないことを確認する。"""
    a, b = TTFont(old_path), TTFont(new_path)
    ca, cb = a.getBestCmap(), b.getBestCmap()
    ga, gb = a['glyf'], b['glyf']
    bad = 0

    lost = sorted(set(ca) - set(cb))
    print(f'  {"OK" if not lost else "NG"} 収録字 {len(ca):,} -> {len(cb):,} '
          f'(+{len(set(cb) - set(ca)):,}, 消えた字 {len(lost)})')
    if lost:
        print('     消えた:', ''.join(chr(c) for c in lost[:60]))
        bad += 1

    diff = 0
    for cp in set(ca) & set(cb):
        pa, pb = ga[ca[cp]], gb[cb[cp]]
        if pa.numberOfContours <= 0 or pb.numberOfContours <= 0:
            continue
        if list(pa.getCoordinates(ga)[0]) != list(pb.getCoordinates(gb)[0]):
            diff += 1
    print(f'  {"OK" if not diff else "NG"} 共通グリフの輪郭 '
          f'{len(set(ca) & set(cb)):,} 字中 相違 {diff}')
    if diff:
        bad += 1

    w = b['OS/2'].usWeightClass
    print(f'  {"OK" if w == 400 else "NG"} usWeightClass {w}')
    bad += (w != 400)

    names = {r.nameID: str(r) for r in b['name'].names}
    for nid, want in ((1, FAMILY), (2, SUBFAMILY), (6, POSTSCRIPT)):
        ok = names.get(nid) == want
        print(f'  {"OK" if ok else "NG"} name {nid} = {names.get(nid)}')
        bad += (not ok)

    for tag in DROP_TABLES:
        print(f'  {"OK" if tag not in b else "NG"} {tag} 削除')
        bad += (tag in b)

    for tag in ('cmap', 'glyf', 'loca', 'head', 'hhea', 'hmtx', 'maxp', 'name', 'OS/2', 'post'):
        if tag not in b:
            print(f'  NG 必須テーブル {tag} が無い')
            bad += 1
    return bad


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    src = sys.argv[1]
    tmp_static = BUNDLED + '.static.tmp'
    tmp_out = BUNDLED + '.tmp'
    charfile = BUNDLED + '.chars.tmp'

    want = app_charset()
    have = set(TTFont(BUNDLED).getBestCmap()) if os.path.exists(BUNDLED) else set()
    # 今の同梱フォントの収録字を必ず含める -> 出ている字が消えることはない
    target = want | have
    print(f'現在の同梱フォント {len(have):,} 字')
    print(f'アプリが描く字 (絵文字除く) {len(want):,} 字')
    print(f'subset 目標 {len(target):,} 字')

    with open(charfile, 'w', encoding='utf-8') as f:
        f.write(''.join(chr(c) for c in sorted(target)))

    static = instancer.instantiateVariableFont(TTFont(src), {'wght': 400}, inplace=False)
    static.save(tmp_static)

    subset.main([
        tmp_static, f'--output-file={tmp_out}', f'--text-file={charfile}',
        '--layout-features=*', '--drop-tables+=' + ','.join(DROP_TABLES),
        '--no-hinting', '--name-IDs=*', '--legacy-kern', '--notdef-outline',
    ])
    f = TTFont(tmp_out)
    fix_names(f)
    f.save(tmp_out)

    before = os.path.getsize(BUNDLED)
    after = os.path.getsize(tmp_out)
    print(f'\n{before:,} -> {after:,} バイト ({after - before:+,})')
    print('検証:')
    bad = verify(BUNDLED, tmp_out)
    for p in (tmp_static, charfile):
        os.remove(p)
    if bad:
        os.remove(tmp_out)
        print(f'\n{bad} 件の不一致。出力を破棄した。')
        sys.exit(1)
    os.replace(tmp_out, BUNDLED)
    print('\nOK')


if __name__ == '__main__':
    main()

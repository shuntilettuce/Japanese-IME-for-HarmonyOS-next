#!/usr/bin/env python3
"""同梱する日本語フォントを作り直す (HarmonyOS 版・Android 版の両方で使う)。

  rawfile/font/BIZUDPGothic-Regular.ttf  本体: BIZ UDP ゴシック (ユニバーサルデザインのフォント)
  rawfile/font/NotoSansJP-Fallback.ttf   補い: BIZ UDP ゴシックに無い字だけの Noto Sans JP

  元データ: https://github.com/google/fonts (どちらも SIL OFL 1.1)
            ofl/bizudpgothic/BIZUDPGothic-Regular.ttf
            ofl/notosansjp/NotoSansJP[wght].ttf

■ なぜ 2 つに分けるか

候補は字形の区別がつきやすい UD フォントで見せたい (2026-10 の指摘)。ただ BIZ UDP
ゴシックは JIS 第 1・第 2 水準が中心で、辞書に出るまれな漢字 (𩸽 など、約 1,800 字)
が無い。無い字がシステムフォントに落ちると、中国向けの端末では中国語の字形になる
(同梱する理由そのもの)。そこで無い字だけ Noto Sans JP で補い、画面では
"BIZUDPGothic,NotoSansJP" の順で引く。

■ 字の集合

「今の同梱フォント 2 つの収録字」∪「アプリが描く字」(build_jp_font.py と同じ考え)。
今出ている字が 1 字も消えないことを確かめてから置き換える。絵文字は入れない。

Usage:
  curl -sSLo /tmp/BIZUDPGothic-Regular.ttf \
    'https://raw.githubusercontent.com/google/fonts/main/ofl/bizudpgothic/BIZUDPGothic-Regular.ttf'
  curl -sSLo /tmp/NotoSansJP.ttf \
    'https://raw.githubusercontent.com/google/fonts/main/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf'
  python3 tools/build_ud_font.py /tmp/BIZUDPGothic-Regular.ttf /tmp/NotoSansJP.ttf
"""
import os
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_jp_font import app_charset, fix_names, is_emoji  # noqa: E402
import build_jp_font  # noqa: E402

FONT_DIR = os.path.join(build_jp_font.RAWFILE, 'font')
UD = os.path.join(FONT_DIR, 'BIZUDPGothic-Regular.ttf')
FALLBACK = os.path.join(FONT_DIR, 'NotoSansJP-Fallback.ttf')
OLD_NOTO = os.path.join(FONT_DIR, 'NotoSansJP-Regular.ttf')   # 置き換える前の 1 本だけの同梱
DROP_TABLES = ['vmtx', 'vhea']


def cmap(path: str) -> set:
    return set(TTFont(path).getBestCmap()) if os.path.exists(path) else set()


def cut(src: TTFont, chars: set, out: str) -> None:
    tmp = out + '.src.tmp'
    src.save(tmp)
    charfile = out + '.chars.tmp'
    with open(charfile, 'w', encoding='utf-8') as f:
        f.write(''.join(chr(c) for c in sorted(chars)))
    subset.main([
        tmp, f'--output-file={out}', f'--text-file={charfile}',
        '--layout-features=*', '--drop-tables+=' + ','.join(DROP_TABLES),
        '--no-hinting', '--name-IDs=*', '--legacy-kern', '--notdef-outline',
    ])
    os.remove(tmp)
    os.remove(charfile)


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    ud_src, noto_src = TTFont(sys.argv[1]), TTFont(sys.argv[2])
    if 'fvar' in noto_src:
        noto_src = instancer.instantiateVariableFont(noto_src, {'wght': 400}, inplace=False)

    have = cmap(OLD_NOTO) | cmap(UD) | cmap(FALLBACK)
    target = {c for c in app_charset() | have if not is_emoji(c)}
    in_ud = set(ud_src.getBestCmap())
    in_noto = set(noto_src.getBestCmap())
    ud_chars = target & in_ud
    fb_chars = (target - in_ud) & in_noto
    print(f'今の同梱 {len(have):,} 字 / 目標 {len(target):,} 字')
    print(f'  BIZ UDP ゴシック {len(ud_chars):,} 字・Noto で補う {len(fb_chars):,} 字・どちらにも無い {len(target - ud_chars - fb_chars):,} 字')

    ud_tmp, fb_tmp = UD + '.tmp', FALLBACK + '.tmp'
    cut(ud_src, ud_chars, ud_tmp)
    cut(noto_src, fb_chars, fb_tmp)
    f = TTFont(fb_tmp)
    fix_names(f)
    f.save(fb_tmp)

    got = cmap(ud_tmp) | cmap(fb_tmp)
    lost = sorted(c for c in have if c not in got and not is_emoji(c))
    print(f'  {"OK" if not lost else "NG"} 消えた字 {len(lost)}' + (': ' + ''.join(chr(c) for c in lost[:60]) if lost else ''))
    if lost:
        os.remove(ud_tmp)
        os.remove(fb_tmp)
        sys.exit(1)
    for tmp, dst in ((ud_tmp, UD), (fb_tmp, FALLBACK)):
        os.replace(tmp, dst)
        print(f'  {os.path.basename(dst)} {os.path.getsize(dst):,} バイト')
    if os.path.exists(OLD_NOTO):
        os.remove(OLD_NOTO)
        print('  NotoSansJP-Regular.ttf (1 本だけの同梱) を外した')
    print('OK')


if __name__ == '__main__':
    main()

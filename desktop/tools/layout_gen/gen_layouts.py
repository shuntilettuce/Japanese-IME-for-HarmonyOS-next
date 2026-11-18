# ACT・gACT10・DvorakJP・SKY の配列の表 (desktop/layouts/*.tsv) を作者の表から作る (2026-11-15)
#   python -I gen_layouts.py <lay_src のフォルダ> <dvorak.tsv> <出力フォルダ>
import os, re, sys, io
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flat_google import load as gload, flatten as gflat

SRC, DVORAK, OUT = sys.argv[1], sys.argv[2], sys.argv[3]


def hira(s):
    return ''.join(chr(ord(c) - 0x60) if 'ァ' <= c <= 'ヶ' and c not in 'ヵヶ' else c for c in s).replace('ヴ', 'ゔ')


def dvorak_rows():
    rows = []
    for l in open(DVORAK, encoding='utf-8'):
        l = l.rstrip('\n')
        if l and not l.startswith('#') and '\t' in l:
            rows.append(l)
    return rows


def write(name, head, pairs, keys=None, note=()):
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        for h in head:
            f.write('#! ' + h + '\n')
        for n in note:
            f.write('# ' + n + '\n')
        seen = {}
        for k, v in pairs:
            seen[k] = v   # 後の表が上書き
        for k, v in seen.items():
            f.write(f'{k}\t{v}\n')
        if keys:
            f.write('#! keys\n')
            for r in keys:
                f.write(r + '\n')
    print(name, len(seen), 'keys' if keys else '')


# ---------------------------------------------------------------- ACT (ACTリファレンス 2003/12/28 + 総合解説書 2009/04/19)
def act_pairs():
    pairs = []
    cols = None
    skip = lambda v: not v or v == '*' or v[0] in '（(' or v[-1] in '）)'
    for l in open(os.path.join(SRC, 'act_ref_tables2.txt'), encoding='utf-8'):
        l = l.rstrip('\n')
        if l.startswith('##') or l == '--' or l.startswith('上段'):
            if l == '--' or l.startswith('##'):
                cols = None
            continue
        cells = [c.strip() for c in l.split('|')]
        if cells[0] == '' and cols is None:
            cols = [re.sub(r'\(\*\)$', '', c) for c in cells[1:]]
            continue
        if cols is None:
            continue
        head = cells[0]
        for c, v in zip(cols, cells[1:]):
            if c and not skip(v):
                pairs.append((head + c, hira(v)))
    # 解説書の本文にある打ち方 (外来語・小書き・っ)
    pairs += [("'", 'っ'), ('twi', 'てぃ'), ('twu', 'てゅ'), ('two', 'とぅ'), ('dbi', 'でぃ'), ('dbu', 'でゅ'), ('dbo', 'どぅ'),
              ('wmo', 'うぉ'), ('la', 'ぁ'), ('li', 'ぃ'), ('lu', 'ぅ'), ('le', 'ぇ'), ('lo', 'ぉ'),
              ('lya', 'ゃ'), ('lyu', 'ゅ'), ('lyo', 'ょ'), ('ltu', 'っ'), ('lka', 'ヵ'), ('lke', 'ヶ'), ('lwa', 'ゎ')]
    return pairs


# ---------------------------------------------------------------- DvorakJP (作者の配列図 key_layout_Dvorakjp.gif と説明)
ROW = {'k': 'かきくけこ', 'c': 'かきくけこ', 's': 'さしすせそ', 't': 'たちつてと', 'n': 'なにぬねの', 'h': 'はひふへほ', 'm': 'まみむめも',
       'r': 'らりるれろ', 'g': 'がぎぐげご', 'z': 'ざじずぜぞ', 'd': 'だぢづでど', 'b': 'ばびぶべぼ', 'p': 'ぱぴぷぺぽ'}
MULTI = {'y': ['や', None, 'ゆ', None, 'よ'], 'f': ['ふぁ', 'ふぃ', 'ふ', 'ふぇ', 'ふぉ'], 'v': ['ゔぁ', 'ゔぃ', 'ゔ', 'ゔぇ', 'ゔぉ'],
         'j': ['じゃ', 'じ', 'じゅ', 'じぇ', 'じょ'], 'sh': ['しゃ', 'し', 'しゅ', 'しぇ', 'しょ'], 'ch': ['ちゃ', 'ち', 'ちゅ', 'ちぇ', 'ちょ'],
         'ts': ['つぁ', 'つぃ', 'つ', 'つぇ', 'つぉ'], 'w': ['わ', None, None, None, None]}
SMALL = ['ゃ', 'ぃ', 'ゅ', 'ぇ', 'ょ']
IKANA = {'k': 'き', 'g': 'ぎ', 's': 'し', 'z': 'じ', 't': 'ち', 'd': 'ぢ', 'n': 'に', 'h': 'ひ', 'b': 'び', 'p': 'ぴ', 'm': 'み', 'r': 'り',
         'f': 'ふ', 'v': 'ゔ', 'c': 'き', 'l': ''}


def djp_pairs():
    sy = {}   # 綴りの頭 → [あ段 い段 う段 え段 お段]
    for k, v in ROW.items():
        sy[k] = list(v)
    sy.update(MULTI)
    for k in 'kgsztdnhbpmr':   # ふつうの拗音 (ky…)
        sy[k + 'y'] = [IKANA[k] + s for s in SMALL]
    combo = {}
    for k in 'cgdtbmf':   # 青のキー: N (Y2)
        combo[k + 'n'] = [IKANA[k] + s for s in SMALL]
    for k in 'przvl':     # 黄のキー: H (Y1)。l は小書き (lha = ゃ)
        combo[k + 'h'] = [IKANA[k] + s for s in SMALL]
    pairs = []
    for k, ks in list(ROW.items())[:2]:
        pass
    for i, v in enumerate('aiueo'):
        pairs.append(('c' + v, ROW['c'][i]))   # か行は C (ci = き・ce = け)
    for p, ks in combo.items():
        for i, v in enumerate('aiueo'):
            pairs.append((p + v, ks[i]))
    sy.update(combo)
    EXT = {"'": (0, 'い'), ',': (4, 'う'), '.': (3, 'い'), ';': (0, 'ん'), 'q': (4, 'ん'), 'j': (3, 'ん'), 'k': (2, 'ん'), 'x': (1, 'ん')}
    for p, ks in sy.items():
        for e, (vi, tail) in EXT.items():
            if len(p) == 1 and p == e:   # kk・jj は子音を重ねた「っ」のまま
                continue
            if ks[vi] is None:
                continue
            if p == 'w' and e not in "';":
                continue
            pairs.append((p + e, ks[vi] + tail))
    return pairs


# ---------------------------------------------------------------- SKY (白鳥・小橋 1987 の基本配列 + 木村 1991 の X・V・F・ー・・)
SKY = {'q': ',', 'w': 'w', 'e': 'r', 'r': 'm', 't': 'h', 'y': 'uu', 'u': 'ai', 'i': 'ou', 'o': '.', 'p': 'ei',
       'a': 'n', 's': 't', 'd': 's', 'f': 'k', 'g': 'y', 'h': 'u', 'j': 'a', 'k': 'o', 'l': 'i', ';': 'e',
       'z': 'p', 'x': 'd', 'c': 'z', 'v': 'g', 'b': 'b', 'n': 'unn', 'm': 'ann', ',': 'onn', '.': 'inn', '/': 'enn',
       '=': 'x', 'yen': 'v', '[': 'f', "'": '-', 'ro': '・'}


def main():
    dv = dvorak_rows()
    write('act.tsv', ['name ACT (Dvorak の拡張ローマ字)', 'kind romaji', 'author 木村清',
                      'url http://www1.vecceed.ne.jp/~bemu/act/act_index.html'],
          act_pairs(), keys=dv,
          note=['ACT リファレンス (2003/12/28) の変換テーブル一覧と ACT 総合解説書 (2009/04/19) のとおり。キーは Dvorak に読み替える (#! keys の後は dvorak.tsv と同じ)',
                'か行は C、「っ」は \' (Dvorak の左上)、撥音拡張は母音の下・二重母音拡張は母音の上。( ) の欄と * の欄は別の表で決まるので書かない',
                '作者は gACT10 (QWERTY のまま使える改良版) を勧めている'])
    rows = gload(os.path.join(SRC, 'gACT10.dat'))
    flat = gflat(rows, maxlen=4)
    gp = [(k, hira(v)) for k, v in sorted(flat.items(), key=lambda x: (len(x[0]), x[0])) if v]
    write('gact10.tsv', ['name gACT10 (QWERTY の ACT)', 'kind romaji', 'base empty', 'author 木村清',
                         'url http://www1.vecceed.ne.jp/~bemu/gact/gact10_doc.html'],
          gp, note=['作者の Google 日本語入力用の表 gACT10.dat (2010.12) の「入力・出力・次の入力」を、打つキーの列 → かな に開いたもの',
                    'ふつうのローマ字の表は使わない (#! base empty)。キーは QWERTY のまま'])
    write('dvorakjp.tsv', ['name DvorakJP (Dvorak の拡張ローマ字)', 'kind romaji', 'author アキーズ',
                           'url http://www7.plala.or.jp/dvorakjp/dvorakjp.htm'],
          djp_pairs(), keys=dv,
          note=['作者の配列図と説明のとおり: か行は C・拗音は青のキー (C G D T B M F) の後に N、黄のキー (P R Z V L) の後に H',
                '二重母音 \' = ai・, = ou・. = ei、撥音 ; = ann・q = onn・j = enn・k = unn・x = inn (子音の後の 2・3 打目だけ)',
                'ふつうのローマ字の綴りもそのまま使える。kk・jj は子音を重ねた「っ」のまま'])
    write('sky.tsv', ['name SKY 配列', 'kind romaji', 'author 白鳥嘉勇・小橋史彦', 'url https://ja.wikipedia.org/wiki/SKY%E9%85%8D%E5%88%97'],
          [], keys=[f'{k}\t{v}' for k, v in SKY.items()],
          note=['白鳥・小橋 (1987) の基本配列。左手が子音、右手が母音・二重母音・撥音 (UN など 1 キー)',
                'X (小書き)・V・F・ー・中点 は「ワープロ徹底操縦法」(木村泉 1991) の拡張の位置 (= ¥ [ \' ろ)',
                'キーは読み替えた字をローマ字に流す (ai・unn など 1 キーで 2〜3 字)'])


main()

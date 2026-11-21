# AZIK 総合解説書 (木村清、変換一覧表) どおりに layouts/azik.tsv を作る。表にない所は足さない
import sys
T = {}
def put(k, v):
    if k in T and T[k] != v: raise SystemExit(f'食い違い {k}: {T[k]} / {v}')
    T[k] = v
# 清音の行: 子音 → (a, i, u, e, o) のかな。空 = 表に無い
ROW = {
 'k': 'か き く け こ', 's': 'さ し す せ そ', 't': 'た ち つ て と', 'n': 'な に ぬ ね の', 'h': 'は ひ ふ へ ほ',
 'f': 'ふぁ ふぃ ふ ふぇ ふぉ', 'm': 'ま み む め も', 'y': 'や - ゆ - よ', 'r': 'ら り る れ ろ', 'w': 'わ うぃ う うぇ を',
 'g': 'が ぎ ぐ げ ご', 'z': 'ざ じ ず ぜ ぞ', 'd': 'だ ぢ づ で ど', 'b': 'ば び ぶ べ ぼ', 'p': 'ぱ ぴ ぷ ぺ ぽ'}
ROW = {c: [None if x == '-' else x for x in v.split()] for c, v in ROW.items()}
# 撥音拡張 (z=ann k=inn j=unn d=enn l=onn) と撥音互換 n (=ann)、二重母音拡張 (q=ai h=uu w=ei p=ou)。表の空欄
BLANK = {('y', 'k'), ('y', 'd'), ('w', 'j'), ('w', 'h'), ('y', 'w'), ('w', 'w')}
for c, (a, i, u, e, o) in ROW.items():
    for ext, base, add in [('z', a, 'ん'), ('n', a, 'ん'), ('k', i, 'ん'), ('j', u, 'ん'), ('d', e, 'ん'), ('l', o, 'ん'),
                           ('q', a, 'い'), ('h', u, 'う'), ('w', e, 'い'), ('p', o, 'う')]:
        if (c, ext) in BLANK or base is None: continue
        if c == 'n' and ext == 'n': continue          # nn = ん (下で)
        if c == 'm' and ext == 'n': continue          # mn = もの (特殊拡張)
        put(c + ext, base + add)
# ou の外来語 (表のとおり長音記号)
T['fp'] = 'ふぉー'; T['wp'] = 'うぉー'; T['wl'] = 'うぉん'   # w の o は を だが、表では うぉん
# 拗音と拗音互換 (y の代わりに g、シャ行 x、チャ行 c、ジャ行 j)
YO = {'ky': 'き', 'kg': 'き', 'sy': 'し', 'x': 'し', 'ty': 'ち', 'c': 'ち', 'ny': 'に', 'ng': 'に', 'hy': 'ひ', 'hg': 'ひ',
      'my': 'み', 'mg': 'み', 'ry': 'り', 'gy': 'ぎ', 'zy': 'じ', 'j': 'じ', 'by': 'び', 'py': 'ぴ', 'pg': 'ぴ'}
for p, b in YO.items():
    for k, v in [('a', 'ゃ'), ('u', 'ゅ'), ('e', 'ぇ'), ('o', 'ょ'), ('z', 'ゃん'), ('n', 'ゃん'), ('j', 'ゅん'), ('d', 'ぇん'),
                 ('l', 'ょん'), ('q', 'ゃい'), ('h', 'ゅう'), ('w', 'ぇい'), ('p', 'ょう')]:
        put(p + k, b + v)
# 外来語の拗音・小さい字
for k, v in {'tgi': 'てぃ', 'tgu': 'とぅ', 'dci': 'でぃ', 'dcu': 'どぅ', 'wsi': 'うぃ', 'wse': 'うぇ', 'wso': 'うぉ',
             'la': 'ぁ', 'li': 'ぃ', 'lu': 'ぅ', 'le': 'ぇ', 'lo': 'ぉ', 'lya': 'ゃ', 'lyu': 'ゅ', 'lyo': 'ょ', 'lwa': 'ゎ'}.items():
    put(k, v)
# 同指打鍵互換キー・打ちにくいパターン
for k, v in {'kf': 'き', 'jf': 'じゅ', 'hf': 'ふ', 'yf': 'ゆ', 'mf': 'む', 'nf': 'ぬ', 'df': 'で', 'cf': 'ちぇ', 'pf': 'ぽん',
             'wf': 'わい', 'sf': 'さい', 'ss': 'せい', 'zc': 'ざ', 'zv': 'ざい', 'zf': 'ぜ', 'zx': 'ぜい'}.items():
    put(k, v)
# 特殊拡張
for k, v in {'kt': 'こと', 'st': 'した', 'tt': 'たち', 'ht': 'ひと', 'wt': 'わた', 'mn': 'もの', 'ms': 'ます', 'ds': 'です',
             'km': 'かも', 'tm': 'ため', 'dm': 'でも', 'kr': 'から', 'sr': 'する', 'tr': 'たら', 'nr': 'なる', 'yr': 'よる',
             'rr': 'られ', 'zr': 'ざる', 'mt': 'また', 'tb': 'たび', 'nb': 'ねば', 'bt': 'びと', 'gr': 'がら', 'gt': 'ごと',
             'nt': 'にち', 'dt': 'だち', 'wr': 'われ'}.items():
    put(k, v)
# 促音・撥音・長音
put(';', 'っ'); put('q', 'ん'); put('nn', 'ん'); put(':', 'ー')
head = ['#! name AZIK (拡張ローマ字)', '#! kind romaji', '#! author 木村清', '#! url http://hp.vector.co.jp/authors/VA002116/azik/azikinfo.htm',
        '# ふつうのローマ字に足す・上書きする綴り (AZIK 総合解説書の変換一覧表のとおり)。表に無い綴りはふつうのローマ字のまま',
        '# 「っ」は ; 、「ん」は q か nn、「ー」は : か - 。kk・ss・tt などは AZIK の綴りになるので、子音を 2 つ重ねた「っ」は表に無い子音だけ']
with open(sys.argv[1], 'w', encoding='utf-8', newline='\n') as w:
    w.write('\n'.join(head) + '\n')
    for k in sorted(T, key=lambda s: (len(s), s)): w.write(f'{k}\t{T[k]}\n')
print(len(T), 'entries')

# Google 日本語入力のローマ字の表 (入力・出力・次の入力) を、打つキーの列 → かな の平らな表にする
import sys, io, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
def load(path):
    rows = []
    for l in open(path, encoding='utf-8-sig'):
        l = l.rstrip('\r\n')
        if not l or l.startswith('#') or '\t' not in l: continue
        p = l.split('\t'); p += [''] * (3 - len(p)); rows.append(tuple(p[:3]))
    return rows
def isascii(s): return all(0x20 < ord(c) < 0x7f for c in s)
def flatten(rows, maxlen=5, skip_pend=()):
    last = {}
    for i, o, p in rows: last[i] = (o, p)   # 同じ入力の行は後の行が勝つ (Google 日本語入力と同じ)
    rows = [(i, o, p) for i, (o, p) in last.items()]
    res = {}
    def is_kana(s): return s and all('ぁ' <= c <= 'ー' for c in s) and not s.endswith('行')
    fr = [(i, o, p) for i, o, p in rows if i and isascii(i)]
    seen = set()
    while fr:
        nf = []
        for k, o, p in fr:
            if (k, o, p) in seen: continue
            seen.add((k, o, p))
            if p in skip_pend: continue
            if not p:
                if k not in res: res[k] = o
                continue
            for i2, o2, p2 in rows:
                if i2.startswith(p) and len(i2) > len(p) and isascii(i2[len(p):]) and len(k) + len(i2) - len(p) <= maxlen:
                    nf.append((k + i2[len(p):], o + o2, p2))
        fr = nf
    # 続きが無いときは、途中の表示 (っ など、かなだけのもの) がそのまま出る
    for i, o, p in rows:
        if i and isascii(i) and is_kana(p) and i not in res:
            res[i] = o + p
    return res
if __name__ == '__main__':
    rows = load(sys.argv[1])
    res = flatten(rows)
    print(len(res))
    for k in sorted(res, key=lambda x: (len(x), x))[:400]: print(k, res[k])

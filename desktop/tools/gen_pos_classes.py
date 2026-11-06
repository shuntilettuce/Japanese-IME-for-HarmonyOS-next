"""
文節の区切りに使う品詞の分類 (desktop/core/pos_classes.h) を、Mozc の品詞 ID の表 (id.def) から作る。

  python desktop/tools/gen_pos_classes.py

辞書 (shuntorge) の品詞 ID は Mozc と同じ。ID ごとに 1 字で:
  '1' = 付属語 (前の語につく: 助詞・助動詞・非自立の動詞と形容詞・接尾・句読点・閉じ括弧)
  '2' = 接頭 (次の語がつく: 接頭詞・開き括弧)
  '3' = サ変接続の名詞 (電話・勉強。後ろの「する」がつく)
  '4' = サ変・スルの動詞 (する・し・さ・せ。サ変接続の名詞の後ろならその文節につく)
  '0' = それ以外 (自立語。ここで文節が始まる)
あわせて学習を品詞ごとに分ける粗い品詞 (POS_GROUPS、2026-11-06) も書く:
  N 名詞 (一般・固有・代名詞・数・サ変・形容動詞語幹・副詞可能など)  n 名詞の非自立 (こと・もの)  S 名詞の接尾
  V 動詞の自立  v 動詞の非自立・接尾  A 形容詞の自立  a 形容詞の非自立・接尾  P 助詞  X 助動詞  F 接頭詞
  D 副詞  R 連体詞  C 接続詞  I 感動詞・フィラー  K 記号  O それ以外
id.def は tools/mozc_data/fetch_mozc.py で取ってくる (tools/mozc_data/cache/。リポジトリには入れない)。
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'tools', 'mozc_data', 'cache', 'id.def')
OUT = os.path.join(ROOT, 'desktop', 'core', 'pos_classes.h')

ATTACH = ('助詞,', '助動詞,', '動詞,非自立', '動詞,接尾', '形容詞,非自立', '形容詞,接尾', '名詞,接尾',
          '記号,句点', '記号,読点', '記号,括弧閉')
PREFIX = ('接頭詞,', '記号,括弧開')
SAHEN_NOUN = ('名詞,サ変接続',)
SURU = ('動詞,自立,*,*,サ変・スル',)


GROUPS = (('名詞,非自立', 'n'), ('名詞,接尾', 'S'), ('名詞,', 'N'), ('動詞,自立', 'V'), ('動詞,', 'v'),
          ('形容詞,自立', 'A'), ('形容詞,', 'a'), ('助詞,', 'P'), ('助動詞,', 'X'), ('接頭詞,', 'F'), ('副詞,', 'D'),
          ('連体詞,', 'R'), ('接続詞,', 'C'), ('感動詞,', 'I'), ('フィラー,', 'I'), ('記号,', 'K'))


def group(pos):
    for p, g in GROUPS:
        if pos.startswith(p):
            return g
    return 'O'


def main():
    cls = {}
    grp = {}
    for line in open(SRC, encoding='utf-8'):
        i, pos = line.rstrip('\n').split(' ', 1)
        c = ('1' if pos.startswith(ATTACH) else '2' if pos.startswith(PREFIX) else '3' if pos.startswith(SAHEN_NOUN)
             else '4' if pos.startswith(SURU) else '0')
        cls[int(i)] = c
        grp[int(i)] = group(pos)
    n = max(cls) + 1
    s = ''.join(cls.get(i, '0') for i in range(n))
    lines = [s[i:i + 100] for i in range(0, n, 100)]
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// 生成物: desktop/tools/gen_pos_classes.py (Mozc の id.def から)。手で直さない\n')
        f.write('// 品詞 ID ごとの文節の分類: 1 = 付属語 (前につく)、2 = 接頭 (次がつく)、3 = サ変接続の名詞、\n')
        f.write('// 4 = サ変・スルの動詞 (3 の後ろならつく)、0 = 自立語\n')
        f.write('#pragma once\n\nnamespace shunti {\n')
        f.write('constexpr int POS_CLASS_COUNT = %d;\n' % n)
        f.write('constexpr const char POS_CLASSES[] =\n')
        for l in lines:
            f.write('    "%s"\n' % l)
        f.write('    ;\n')
        g = ''.join(grp.get(i, 'O') for i in range(n))
        f.write('// 学習を分ける粗い品詞 (N 名詞・n 名詞非自立・S 名詞接尾・V 動詞・v 動詞非自立/接尾・A 形容詞・a 形容詞非自立/接尾・\n')
        f.write('// P 助詞・X 助動詞・F 接頭詞・D 副詞・R 連体詞・C 接続詞・I 感動詞・K 記号・O それ以外)\n')
        f.write('constexpr const char POS_GROUPS[] =\n')
        for i in range(0, n, 100):
            f.write('    "%s"\n' % g[i:i + 100])
        f.write('    ;\n}  // namespace shunti\n')
    print(OUT, n, 'attach', s.count('1'), 'prefix', s.count('2'), 'sahen', s.count('3'), 'suru', s.count('4'))


if __name__ == '__main__':
    main()

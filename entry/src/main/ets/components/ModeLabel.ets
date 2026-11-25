// 「あA」キーの表示: 押すと切り替わる順に、今のモードから並べる (あA中1 / A中1あ / 中1あA / 1あA中)。
// 中 (中国語のなぞり) は設定でオンにしたとき、1 (数字パッド) は数字パッドを使うときだけ入る
export function modeKeyLabel(cur: string, numeric: boolean, zh: boolean): string {
  const order: string[] = ['あ', 'A'];
  if (zh) { order.push('中'); }
  if (numeric) { order.push('1'); }
  const i = Math.max(0, order.indexOf(cur));
  return order.slice(i).join('') + order.slice(0, i).join('');
}

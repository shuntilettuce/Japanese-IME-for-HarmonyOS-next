// 9 キー (FlickKeyboardView) と数字パッド (NumericPadView) で共有する、
// フリック中の見た目の部品。三角の形と、浮き文字の丸の大きさを 1 か所にまとめる。

export type FlickDirection = 'center' | 'up' | 'right' | 'down' | 'left';

// 打った方向の辺に出す三角。キーの実寸 (wVp x hVp, vp) に合わせて作る。
// 三角は辺の全長を覆い、角の丸み (rVp) の部分も角丸に沿って塗る。
// 頂点 (apex) は辺から内側へ 38% の位置。
// Path の commands の数値は px 単位なので、vp2px で変換してから座標を作る。
// A コマンドの sweep 値 (1 = 時計回り, 0 = 反時計回り; 画面座標は y が下向き) は
// 各角の回る向きに合わせてある。
export function flickTriangleCommands(dir: FlickDirection, wVp: number, hVp: number, rVp: number): string {
  const w: number = vp2px(wVp);
  const h: number = vp2px(hVp);
  const r: number = vp2px(Math.min(rVp, wVp / 2, hVp / 2));
  const dx: number = w * 0.38;
  const dy: number = h * 0.38;
  if (dir === 'right') {
    // 右辺: 上の角 → 右辺 → 下の角 → 頂点 (内側) → 戻る
    return `M${w - r} 0 A${r} ${r} 0 0 1 ${w} ${r} L${w} ${h - r} A${r} ${r} 0 0 1 ${w - r} ${h} L${w - dx} ${h / 2} Z`;
  }
  if (dir === 'left') {
    return `M${r} 0 A${r} ${r} 0 0 0 0 ${r} L0 ${h - r} A${r} ${r} 0 0 0 ${r} ${h} L${dx} ${h / 2} Z`;
  }
  if (dir === 'up') {
    return `M0 ${r} A${r} ${r} 0 0 1 ${r} 0 L${w - r} 0 A${r} ${r} 0 0 1 ${w} ${r} L${w / 2} ${dy} Z`;
  }
  if (dir === 'down') {
    return `M0 ${h - r} A${r} ${r} 0 0 0 ${r} ${h} L${w - r} ${h} A${r} ${r} 0 0 0 ${w} ${h - r} L${w / 2} ${h - dy} Z`;
  }
  return '';
}

// 打とうとしている文字を浮かせる丸の直径 (vp)
export const BUBBLE_VP: number = 48;

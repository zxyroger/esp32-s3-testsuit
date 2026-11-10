# -*- coding: utf-8 -*-
"""守卫改造验证：把「全局 mad 门控的局部保护」换成「始终启用的局部差异调制」。
静止区噪声小 -> 仍强平均压横纹；局部运动区差异大 -> 自动绕过 EMA 不留重影。
同时扫 LOC_G0 / LOC_K，找横纹与拖影的双优区间。
"""
import glob
import itertools

import numpy as np
from PIL import Image

from sim14_lowlight import desat_dark, directional_vlp, sharpen_h, Meter

FR = sorted(glob.glob('frames_1405/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
rng = np.random.default_rng(7)

# ---------- 两套序列：静止序列（测横纹）+ 局部运动序列（测拖影） ----------
base = D[0].copy()
other = D[-1].copy()
seq_static = [np.clip(D[i] + rng.normal(0, 0.6, D[i].shape), 0, 255).astype(np.float32)
              for i in range(8)]
BX, BY, BS = W // 2 - 60, H // 2 - 60, 120
seq_move = []
for i in range(12):
    f = base + rng.normal(0, 0.8, base.shape)
    if 4 <= i <= 7:
        patch = np.roll(other[BY:BY + BS, BX:BX + BS].copy(), (i - 4) * 3, axis=1)
        f[BY:BY + BS, BX:BX + BS] = patch
    seq_move.append(np.clip(f, 0, 255).astype(np.float32))

M = Meter(base)


def smooth(diff):
    p = np.pad(diff, ((0, 0), (1, 1)), mode='edge')
    t = (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3
    q = np.pad(t, ((1, 1), (0, 0)), mode='edge')
    return (q[:-2, :] + q[1:-1, :] + q[2:, :]) / 3


def ema(frames, alpha, m0, mode, g0=2.0, kg=12.0, local_k=10.0):
    acc = None
    outs = []
    for d in frames:
        if acc is None:
            acc = d.copy()
            outs.append(d.copy()); continue
        diff = np.abs(d - acc).mean(axis=2)
        mad = float(diff.mean())
        a = 0.30 if mad > 16 else alpha
        loc = smooth(diff)
        if mode == 'old':
            guard = float(np.clip((mad - m0) / 3, 0, 1))
            wgt = np.clip(1 - guard * loc / local_k, 0, 1)
        else:
            wgt = np.clip(1 - (loc - g0) / kg, 0, 1)
        ae = a * wgt
        cur = acc * ae[..., None] + d * (1 - ae[..., None])
        acc = cur.copy()
        outs.append(cur.copy())
    return outs


def post(frames, vlpK=4, lam=0.9, sharp=1.0):
    o = []
    for f in frames:
        x = desat_dark(f.copy(), 80)
        x = directional_vlp(x, vlpK, lam)
        x = sharpen_h(x, sharp, horizontal=True)
        o.append(x)
    return o


def ghost(out, truth, k):
    a = out[k][BY:BY + BS, BX:BX + BS]
    b = truth[k][BY:BY + BS, BX:BX + BS]
    return float(np.sqrt(((a - b) ** 2).mean()))


CFG = dict(alpha=0.85, m0=7, vlpK=4, lam=0.9, sharp=1.0)
print('%-34s %9s %9s %9s' % ('守卫方案', '静止flat', '运动首帧', '运动末帧'))
E = ema(seq_static, CFG['alpha'], CFG['m0'], 'old')
print('%-34s %9.3f' % ('现状（全局 guard 门控）', M.band(post(E)[5])), end='')
Em = ema(seq_move, CFG['alpha'], CFG['m0'], 'old')
Om = post(Em)
print(' %9.2f %9.2f' % (ghost(Om, seq_move, 4), ghost(Om, seq_move, 7)))

print()
print('新方案扫描（始终局部调制）')
print('%-24s %9s %9s %9s %9s' % ('G0/K', '静止flat', '运动首帧', '运动末帧', '恢复帧'))
best = None
for g0, kg in itertools.product((1.0, 2.0, 3.0, 4.0), (8.0, 12.0, 16.0, 24.0)):
    Es = ema(seq_static, CFG['alpha'], CFG['m0'], 'new', g0, kg)
    f = M.band(post(Es)[5])
    Em = ema(seq_move, CFG['alpha'], CFG['m0'], 'new', g0, kg)
    Om = post(Em)
    g4, g7, g10 = ghost(Om, seq_move, 4), ghost(Om, seq_move, 7), ghost(Om, seq_move, 10)
    print('G0=%.0f K=%-4.0f %14.3f %9.2f %9.2f %9.2f' % (g0, kg, f, g4, g7, g10))
    # 目标：拖影小、横纹不明显变差
    score = (g7 / 10.0) + f
    if best is None or score < best[0]:
        best = (score, g0, kg, f, g4, g7, g10)
print()
print('推荐 G0=%.1f K=%.0f : 静止flat=%.3f 运动末帧=%.2f 恢复帧=%.2f' %
      (best[1], best[2], best[3], best[5], best[6]))
print('对照：无 EMA 运动末帧 5.92，现状 25.80')

# -*- coding: utf-8 -*-
"""守卫判据升级：用「局部差异 / 全局差异」的比值区分
  ① 局部运动（比值高 -> 保护，不留拖影）
  ② 全局光照变化（比值≈1 -> 仍强平均，只是整体延迟，无重影）
对比旧守卫（绝对 loc 阈值）在黄昏云影场景下的表现。
"""
import glob

import numpy as np
from PIL import Image

from sim14_lowlight import desat_dark, directional_vlp, sharpen_h, Meter

FR = sorted(glob.glob('frames_1405/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
rng = np.random.default_rng(11)
base = D[0].copy()
other = D[-1].copy()
BX, BY, BS = W // 2 - 60, H // 2 - 60, 120

# ① 静止    ② 局部运动    ③ 全局光照变化（黄昏云影：整体乘性变化 + 轻微渐变）
seq_static = [np.clip(D[i] + rng.normal(0, 0.6, D[i].shape), 0, 255).astype(np.float32)
              for i in range(8)]
seq_move = []
for i in range(12):
    f = base + rng.normal(0, 0.8, base.shape)
    if 4 <= i <= 7:
        patch = np.roll(other[BY:BY + BS, BX:BX + BS].copy(), (i - 4) * 3, axis=1)
        f[BY:BY + BS, BX:BX + BS] = patch
    seq_move.append(np.clip(f, 0, 255).astype(np.float32))
seq_light = []
for i in range(12):
    g = 1.0 + 0.05 * np.sin(i * 1.1)          # 全画面 ±5% 亮度摆动
    ramp = 1.0 + 0.03 * np.linspace(0, 1, H)[:, None, None]   # 轻微上下渐变（云影）
    f = D[i % len(D)] * g * ramp
    seq_light.append(np.clip(f + rng.normal(0, 0.6, f.shape), 0, 255).astype(np.float32))

M = Meter(base)


def smooth(diff):
    p = np.pad(diff, ((0, 0), (1, 1)), mode='edge')
    t = (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3
    q = np.pad(t, ((1, 1), (0, 0)), mode='edge')
    return (q[:-2, :] + q[1:-1, :] + q[2:, :]) / 3


def ema(frames, alpha, mode, g0=2.0, kg=24.0, r0=1.6, rk=1.4):
    acc = None
    outs = []
    for d in frames:
        if acc is None:
            acc = d.copy(); outs.append(d.copy()); continue
        diff = np.abs(d - acc).mean(axis=2)
        mad = float(diff.mean())
        a = 0.30 if mad > 16 else alpha
        loc = smooth(diff)
        if mode == 'abs':
            wgt = np.clip(1 - (loc - g0) / kg, 0, 1)
        elif mode == 'ratio':
            r = loc / (mad + 1.0)
            wgt = np.clip(1 - (r - r0) / rk, 0, 1)
        else:                                    # 无保护
            wgt = np.ones_like(loc)
        ae = a * wgt
        acc = acc * ae[..., None] + d * (1 - ae[..., None])
        outs.append(acc.copy())
    return outs


def post(frames, vlpK=4, lam=0.9, sharp=1.0):
    o = []
    for f in frames:
        x = desat_dark(f.copy(), 80)
        x = directional_vlp(x, vlpK, lam)
        o.append(sharpen_h(x, sharp, horizontal=True))
    return o


def ghost(out, truth, k):
    a = out[k][BY:BY + BS, BX:BX + BS]
    b = truth[k][BY:BY + BS, BX:BX + BS]
    return float(np.sqrt(((a - b) ** 2).mean()))


print('%-30s %10s %10s %10s %10s' % ('守卫', '静止flat', '运动末帧', '云影flat', '云影亮度误差'))
for name, mode, kw in [('无保护', 'none', {}),
                       ('旧 绝对loc G0=2 K=24', 'abs', {}),
                       ('新 相对比值 R0=1.6 RK=1.4', 'ratio', {}),
                       ('相对 R0=2.0 RK=1.5', 'ratio', dict(r0=2.0, rk=1.5)),
                       ('相对 R0=1.3 RK=1.2', 'ratio', dict(r0=1.3, rk=1.2))]:
    Os = post(ema(seq_static, 0.85, mode, **kw))
    Om = post(ema(seq_move, 0.85, mode, **kw))
    Ol = post(ema(seq_light, 0.85, mode, **kw))
    flat_s = M.band(Os[5])
    g_move = ghost(Om, seq_move, 7)
    flat_l = M.band(Ol[8])
    # 云影亮度误差：输出均值与真实帧均值之差（强平均会把亮度变化拖慢）
    lum_err = abs(float(0.299 * Ol[8][..., 0].mean() + 0.587 * Ol[8][..., 1].mean()
                        + 0.114 * Ol[8][..., 2].mean())
                  - float(0.299 * seq_light[8][..., 0].mean() + 0.587 * seq_light[8][..., 1].mean()
                          + 0.114 * seq_light[8][..., 2].mean()))
    print('%-30s %10.3f %10.2f %10.3f %10.2f' % (name, flat_s, g_move, flat_l, lum_err))

print()
print('期望：静止 flat 低、运动末帧接近 5.9、云影下 flat 低且亮度误差小。')

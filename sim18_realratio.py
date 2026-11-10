# -*- coding: utf-8 -*-
"""用真实黄昏连续帧比较三种守卫：无保护 / 旧绝对阈值 / 新相对比值。
数据源默认 frames_dusk_1700（17:00 连续 8 帧，mad 偏大的黄昏场景）。
"""
import glob
import sys

import numpy as np
from PIL import Image

from sim14_lowlight import desat_dark, directional_vlp, sharpen_h, Meter

PAT = sys.argv[1] if len(sys.argv) > 1 else 'frames_dusk_1700/*.jpg'
FR = sorted(glob.glob(PAT))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
print('帧 %d 张 %dx%d' % (len(D), D[0].shape[1], D[0].shape[0]))
M = Meter(D[len(D) // 2])


def smooth(diff):
    p = np.pad(diff, ((0, 0), (1, 1)), mode='edge')
    t = (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3
    q = np.pad(t, ((1, 1), (0, 0)), mode='edge')
    return (q[:-2, :] + q[1:-1, :] + q[2:, :]) / 3


def ema(frames, alpha, mode, g0=2.0, kg=24.0, r0=2.0, rk=1.5):
    acc = None
    outs = []
    mads = []
    for d in frames:
        if acc is None:
            acc = d.copy(); outs.append(d.copy()); mads.append(0.0); continue
        diff = np.abs(d - acc).mean(axis=2)
        mad = float(diff.mean()); mads.append(mad)
        a = 0.30 if mad > 16 else alpha
        loc = smooth(diff)
        if mode == 'none':
            wgt = np.ones_like(loc)
        elif mode == 'abs':
            wgt = np.clip(1 - (loc - g0) / kg, 0, 1)
        else:
            wgt = np.clip(1 - (loc / (mad + 1.0) - r0) / rk, 0, 1)
        ae = a * wgt
        acc = acc * ae[..., None] + d * (1 - ae[..., None])
        outs.append(acc.copy())
    return outs, mads


def post(frames, vlpK=4, lam=0.9, sharp=1.0):
    o = []
    for f in frames:
        x = desat_dark(f.copy(), 80)
        x = directional_vlp(x, vlpK, lam)
        o.append(sharpen_h(x, sharp, horizontal=True))
    return o


k = len(D) // 2
print('原始: L=%.1f flat=%.3f sharp=%.2f' % (M.mean, M.band(D[k]), M.sharp(D[k])))
print('%-24s %8s %8s %7s %8s' % ('守卫', 'flat', 'dark', 'sharp', '平均mad'))
for name, mode in [('无保护', 'none'), ('旧 绝对阈值', 'abs'), ('新 相对比值', 'ratio')]:
    E, mads = ema(D, 0.85, mode)
    O = post(E)
    print('%-24s %8.3f %8.3f %7.2f %8.2f' %
          (name, M.band(O[k]), M.band(O[k], M.flat_dark) if M.flat_dark else 0,
           M.sharp(O[k]), float(np.mean(mads[1:]))))

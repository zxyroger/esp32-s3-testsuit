# -*- coding: utf-8 -*-
import glob
import numpy as np
from PIL import Image


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def smooth_rows(rm, k):
    h = len(rm)
    out = np.empty(h)
    for y in range(h):
        out[y] = rm[max(0, y - k):min(h, y + k + 1)].mean()
    return out


def metric(d, k=10):
    L = lum(d)
    rows = L.mean(axis=1)
    lp = smooth_rows(rows, k)
    hp = rows - lp
    dark = hp[lp < 90]
    bright = hp[lp > 150]
    return (float(hp.std()),
            float(dark.std()) if dark.size else 0.0,
            float(bright.std()) if bright.size else 0.0)


frames = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32)
          for f in sorted(glob.glob('frames_day/cam_*.jpg'))]

acc = None
emas = []
for d in frames:
    d = d.astype(np.float32)
    if acc is not None:
        d = acc * 0.55 + d * 0.45
    acc = d.copy()
    emas.append(d)

offs = []
gains = []
for d in emas:
    rm = lum(d).mean(axis=1)
    lp = smooth_rows(rm, 10)
    offs.append(rm - lp)
    gains.append(lp / np.maximum(rm, 1))
addp = np.mean(offs, axis=0)
mulp = np.mean(gains, axis=0)
print('加性profile std=%.2f  乘性profile std=%.4f' % (addp.std(), mulp.std()))

res_add = [metric(d - addp[:, None, None]) for d in emas]
res_mul = [metric(d * mulp[:, None, None]) for d in emas]
base = [metric(d) for d in emas]


def avg(m, i):
    return sum(x[i] for x in m) / len(m)


print('%-10s %8s %8s %8s' % ('方案', '总体', '暗', '亮'))
print('%-10s %8.2f %8.2f %8.2f' % ('EMA基准', avg(base, 0), avg(base, 1), avg(base, 2)))
print('%-10s %8.2f %8.2f %8.2f' % ('EMA+加性', avg(res_add, 0), avg(res_add, 1), avg(res_add, 2)))
print('%-10s %8.2f %8.2f %8.2f' % ('EMA+乘性', avg(res_mul, 0), avg(res_mul, 1), avg(res_mul, 2)))

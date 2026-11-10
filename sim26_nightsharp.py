# -*- coding: utf-8 -*-
"""sim26: 夜间"糊"的定量诊断与参数重标定。

难点：夜间亮度低，普通梯度均值会被噪点抬高，看起来"很锐"其实是噪。
所以这里用两个抗噪指标：
  acut  —— 只统计强边缘（|grad| 前 2%）的平均梯度，模糊会把它拉低，
           而随机噪点几乎不进入这个集合；
  noise —— 暗区高频残差 std（Y 减去 3x3 均值），衡量颗粒。
目标：band4 保持 <1.0 的前提下，把 acut 拉回来、noise 别涨。
"""
import glob
import itertools
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = [
    ("18:36", "frames_dusk_1836/*.jpg"),
    ("18:20", "frames_dusk_1820/*.jpg"),
]


def to_L(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def band4(a):
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    return float(np.std(r - sm))


def acut(a):
    """强边缘平均梯度（抗噪锐度）。"""
    L = to_L(a)
    gx = np.abs(np.diff(L, axis=1))[:-1, :]
    gy = np.abs(np.diff(L, axis=0))[:, :-1]
    g = gx + gy
    thr = np.percentile(g, 98)
    return float(g[g >= thr].mean())


def noise(a):
    L = to_L(a)
    p = np.pad(L, 1, mode="edge")
    m = sum(p[dy:dy + L.shape[0], dx:dx + L.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
    hf = L - m
    dark = L < 45
    return float(hf[dark].std()) if dark.sum() > 500 else float(hf.std())


def flicker_norm(a, s):
    if s <= 0:
        return a
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    g = np.clip(1.0 - s * (r - sm) / np.maximum(r, 1.0), 0.5, 2.0)
    return a * g[:, None, None]


def ema(acc, cur, a_base):
    if acc is None:
        return cur.copy()
    diff = np.abs(to_L(cur) - to_L(acc))
    mad = float(diff.mean()) + 1.0
    loc = (np.pad(diff, ((0, 0), (1, 1)), mode="edge")[:, :-2] + diff
           + np.pad(diff, ((0, 0), (1, 1)), mode="edge")[:, 2:]) / 3.0
    loc = (np.pad(loc, ((1, 1), (0, 0)), mode="edge")[:-2] + loc
           + np.pad(loc, ((1, 1), (0, 0)), mode="edge")[2:]) / 3.0
    wgt = np.clip(1.0 - (loc / mad - 2.0) / 1.5, 0.0, 1.0)
    ae = (a_base * wgt)[..., None]
    return acc * ae + cur * (1.0 - ae)


def vlp(a, K, lam):
    if K < 1 or lam <= 0:
        return a
    L = to_L(a)
    gx = np.zeros_like(L)
    gx[:, 1:-1] = np.abs(L[:, 2:] - L[:, :-2]) / 2.0
    gx = gx * 0.5 + 0.25 * (np.roll(gx, 1, 1) + np.roll(gx, -1, 1))
    mask = np.clip((gx - 2.0) / 8.0, 0.0, 1.0)
    h = a.shape[0]
    ext = np.pad(a, ((K, K), (0, 0), (0, 0)), mode="edge")
    v = np.zeros_like(a)
    for j in range(2 * K + 1):
        v += ext[j:j + h]
    v /= (2 * K + 1)
    return a + (lam * (1.0 - mask))[..., None] * (v - a)


def sharp_h(a, amt):
    if amt <= 0:
        return a
    L = to_L(a)
    gx = np.zeros_like(L)
    gx[:, 1:-1] = np.abs(L[:, 2:] - L[:, :-2]) / 2.0
    gx = gx * 0.5 + 0.25 * (np.roll(gx, 1, 1) + np.roll(gx, -1, 1))
    mask = np.clip((gx - 2.0) / 8.0, 0.0, 1.0)
    blur = a.copy()
    blur[:, 1:-1] = (a[:, :-2] + a[:, 1:-1] + a[:, 2:]) / 3.0
    return a + (amt * mask)[..., None] * (a - blur)


def run(fs, alpha, K, lam, sh):
    acc, B, A, N = None, [], [], []
    for f in fs:
        a = np.asarray(Image.open(f).convert("RGB"), np.float32)
        a = flicker_norm(a, 0.85)
        acc = ema(acc, a, alpha)
        b = sharp_h(vlp(acc, K, lam), sh)
        B.append(band4(b)); A.append(acut(b)); N.append(noise(b))
    return float(np.mean(B)), float(np.mean(A)), float(np.mean(N))


def main():
    for name, pat in SETS:
        fs = sorted(glob.glob(os.path.join(HERE, pat)))
        if not fs:
            continue
        raws = [np.asarray(Image.open(f).convert("RGB"), np.float32) for f in fs]
        rb = float(np.mean([band4(a) for a in raws]))
        ra = float(np.mean([acut(a) for a in raws]))
        rn = float(np.mean([noise(a) for a in raws]))
        print("\n=== %s  原始 band4=%.2f acut=%.2f noise=%.2f ===" % (name, rb, ra, rn))
        print(" alpha  K  lam  sharp |  band4   acut(锐度)  noise  软硬比")
        for alpha, K, lam, sh in itertools.product([0.75, 0.85, 0.90],
                                                   [1, 2, 4],
                                                   [0.5, 0.8, 1.0],
                                                   [0.8, 1.2]):
            b, a_, n = run(fs, alpha, K, lam, sh)
            print("  %.2f   %d  %.1f   %.1f  |  %5.2f   %6.2f    %5.2f   %5.2f"
                  % (alpha, K, lam, sh, b, a_, n, a_ / max(n, 1e-6)))


if __name__ == "__main__":
    main()

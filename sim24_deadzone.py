# -*- coding: utf-8 -*-
"""sim24: 定位 L≈65 死区，并验证「加强行增益归一化」能否安全补上。

关键先验：flickerRowNorm 只按行乘一个增益，横向细节（x 方向梯度）完全不动，
所以加强它不会糊画面；风险只在于暗行放大噪点（钳位 0.5~2 已限制）。
因此这里同时监控 band4 与 sharp，找「压得下去且锐度不掉」的强度。
"""
import glob
import itertools
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = [
    ("17:00", "frames_dusk_1700/*.jpg"),
    ("17:20", "frames_dusk_1720/*.jpg"),
    ("17:40", "frames_dusk_1740/*.jpg"),
    ("18:20", "frames_dusk_1820/*.jpg"),
]


def to_L(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def band4_whole(a):
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    return float((r - sm).std())


def band4_flat(a, BW=80, n=10):
    L = to_L(a)
    h, w = L.shape
    blocks = []
    for by in range(0, h - BW + 1, BW):
        for bx in range(0, w - BW + 1, BW):
            blocks.append((L[by:by + BW, bx:bx + BW].std(), by, bx))
    blocks.sort(key=lambda t: t[0])
    hp = []
    for _, by, bx in blocks[:n]:
        rr = L[by:by + BW, bx:bx + BW].mean(axis=1)
        ext = np.pad(rr, (4, 4), mode="edge")
        sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
        hp.extend(rr - sm)
    return float(np.std(hp))


def sharp_h(a):
    L = to_L(a)
    gx = np.zeros_like(L)
    gx[:, 1:-1] = np.abs(L[:, 2:] - L[:, :-2]) / 2.0
    return float(gx[:, 1:-1].mean())


def flick(a, s, K=4):
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (K, K), mode="edge")
    sm = np.convolve(ext, np.ones(2 * K + 1) / (2 * K + 1), mode="same")[K:-K]
    g = np.clip(1.0 - s * (r - sm) / np.maximum(r, 1.0), 0.5, 2.0)
    return a * g[:, None, None]


def dominant_period(a):
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    hp = r - sm
    F = np.abs(np.fft.rfft(hp * np.hanning(len(hp))))
    fr = np.fft.rfftfreq(len(hp))
    F[fr < 1 / 40.0] = 0
    i = int(np.argmax(F))
    return 1.0 / max(fr[i], 1e-6), F


def main():
    for name, pat in SETS:
        fs = sorted(glob.glob(os.path.join(HERE, pat)))
        if not fs:
            continue
        imgs = [np.asarray(Image.open(f).convert("RGB"), np.float32) for f in fs]
        L = float(np.mean([to_L(a).mean() for a in imgs]))
        per = [dominant_period(a)[0] for a in imgs]
        base_w = float(np.mean([band4_whole(a) for a in imgs]))
        base_f = float(np.mean([band4_flat(a) for a in imgs]))
        base_s = float(np.mean([sharp_h(a) for a in imgs]))
        print("\n=== %s  L=%.1f  主周期 %.1f~%.1f 行 ===" % (name, L, min(per), max(per)))
        print("  s    迭代  整体band4  平坦band4   sharp")
        for s, it in itertools.product([0.85, 1.0, 1.2, 1.5], [1, 2]):
            bw, bf, ss = [], [], []
            for a in imgs:
                b = a
                for _ in range(it):
                    b = flick(b, s)
                bw.append(band4_whole(b)); bf.append(band4_flat(b)); ss.append(sharp_h(b))
            print(" %4.2f  %3d  %8.2f  %9.2f  %6.2f  (压制 %3.0f%%)"
                  % (s, it, np.mean(bw), np.mean(bf), np.mean(ss),
                     100 * (1 - np.mean(bf) / max(base_f, 1e-6))))
        print("  原始        %8.2f  %9.2f  %6.2f" % (base_w, base_f, base_s))


if __name__ == "__main__":
    main()

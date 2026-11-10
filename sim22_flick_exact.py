# -*- coding: utf-8 -*-
"""sim22: 精确复刻页面 flickerRowNorm（flickerS=0.85，钳位 0.5~2），
在 17:00→18:36 全链 + 白天帧上验证压制率，并检验 L=95 硬开关的跳变风险。

同时对比三种门控策略：
  A 现状：lightEMA < 95 全开（硬开关）
  B 亮度平滑 ramp：t=(105-L)/70
  C 实测自门控：用帧内 band4 相对强度决定力度
"""
import glob
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = [
    ("白天SVGA", "frames_1405/cam_*.jpg"),
    ("17:00", "frames_dusk_1700/*.jpg"),
    ("17:20", "frames_dusk_1720/*.jpg"),
    ("17:40", "frames_dusk_1740/*.jpg"),
    ("18:00", "frames_dusk_1800/*.jpg"),
    ("18:20", "frames_dusk_1820/*.jpg"),
    ("18:36", "frames_dusk_1836/*.jpg"),
]


def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def band4(L):
    """行均值 9 抽头高通残差 std（与 JS flickTmp 同口径）。"""
    r = L.mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    return float((r - sm).std())


def flicker_norm(L, s):
    """精确复刻 JS：逐行乘性校正，钳位 0.5~2。"""
    r = L.mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    hp = r - sm
    den = np.maximum(r, 1.0)
    g = 1.0 - s * hp / den
    g = np.clip(g, 0.5, 2.0)
    return L * g[:, None]


def flat8(L, block=40, n=10):
    h, w = L.shape
    vals = []
    for y in range(0, h - block + 1, block):
        for x in range(0, w - block + 1, block):
            p = L[y:y + block, x:x + block]
            if p.std() > 12:
                continue
            rr = p.mean(axis=1)
            ext = np.pad(rr, (8, 8), mode="edge")
            sm = np.convolve(ext, np.ones(17) / 17.0, mode="same")[8:-8]
            vals.append(float((rr - sm).std()))
    return float(np.mean(sorted(vals)[:n])) if vals else float("nan")


def main():
    print("组        L      band4    A:s=0.85全开   B:ramp      C:实测门控   flat8(raw->out)")
    for name, pat in SETS:
        fs = sorted(glob.glob(os.path.join(HERE, pat)))
        if not fs:
            continue
        Ls = [lum(np.asarray(Image.open(f).convert("RGB"), np.float32)) for f in fs]
        Lm = float(np.mean([L.mean() for L in Ls]))
        b4 = float(np.mean([band4(L) for L in Ls]))
        # A 硬开关
        sA = 0.85 if Lm < 95 else 0.0
        # B 亮度 ramp
        tB = min(1.0, max(0.0, (105 - Lm) / 70))
        sB = 0.85 * tB
        # C 实测自门控：band4/行均值 相对强度
        rel = b4 / max(Lm, 1.0)
        tC = min(1.0, max(0.0, (rel - 0.03) / 0.12))
        sC = 0.85 * tC
        rA = float(np.mean([band4(flicker_norm(L, sA)) for L in Ls]))
        rB = float(np.mean([band4(flicker_norm(L, sB)) for L in Ls]))
        rC = float(np.mean([band4(flicker_norm(L, sC)) for L in Ls]))
        f0 = float(np.nanmean([flat8(L) for L in Ls]))
        f1 = float(np.nanmean([flat8(flicker_norm(L, sA)) for L in Ls]))
        print("%-8s %6.1f %7.2f  %5.2f>%5.2f(%3.0f%%) %5.2f(%3.0f%%) %5.2f(%3.0f%%)  %5.3f>%5.3f"
              % (name, Lm, b4, b4, rA, 100 * rA / max(b4, 1e-6),
                 rB, 100 * rB / max(b4, 1e-6), rC, 100 * rC / max(b4, 1e-6), f0, f1))


if __name__ == "__main__":
    main()

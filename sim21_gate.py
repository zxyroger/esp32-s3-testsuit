# -*- coding: utf-8 -*-
"""sim21: 用 17:00~18:36 六组真实帧，检验「亮度代理」是否是工频条带的可靠开关。

要回答三件事：
1) 各组帧的真实条带强度 band4 是多少？是否随 L 单调变化？
2) 现有自适应（按 L 映射 t）在每组会给多少力度？够不够？
3) 若改成「按帧内实测 band4 自门控」，压制率能否在所有档位都稳定？
"""
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DIRS = ["frames_dusk_1700", "frames_dusk_1720", "frames_dusk_1740",
        "frames_dusk_1800", "frames_dusk_1820", "frames_dusk_1836"]


def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def row_hp_std(L, k):
    """行均值的高通残差 std（k=4 抓工频条带，k=8 抓固定行 FPN）。"""
    r = L.mean(axis=1)
    ext = np.pad(r, (k, k), mode="reflect")
    sm = np.convolve(ext, np.ones(2 * k + 1) / (2 * k + 1), mode="same")[k:-k]
    return float((r - sm).std())


def flat_band(L, block=40, n=10):
    """最平坦 10 块（block x block）的行高通 std —— 与页面 measureFrame 同口径。"""
    h, w = L.shape
    vals = []
    for y in range(0, h - block + 1, block):
        for x in range(0, w - block + 1, block):
            p = L[y:y + block, x:x + block]
            if p.std() > 12:
                continue
            vals.append(row_hp_std(p, 8))
    if not vals:
        return float("nan")
    vals.sort()
    return float(np.mean(vals[:n]))


def flicker_row_norm(L, K=4, lam=1.0):
    """逐帧行增益归一化：行均值 4 行高通 → 减掉 → 还原。lam=强度。"""
    r = L.mean(axis=1)
    ext = np.pad(r, (K, K), mode="reflect")
    sm = np.convolve(ext, np.ones(2 * K + 1) / (2 * K + 1), mode="same")[K:-K]
    hp = r - sm
    gain = 1.0 - lam * hp / np.maximum(r, 8.0)
    gain = np.clip(gain, 0.80, 1.25)
    return L * gain[:, None]


def main():
    print("组                L     band4   band8  flat8  rawSharp  t(L代理)  t(实测)  压制后band4  残留%")
    for d in DIRS:
        fs = sorted(glob.glob(os.path.join(HERE, d, "*.jpg")))
        if not fs:
            continue
        rows = []
        for f in fs:
            a = np.asarray(Image.open(f).convert("RGB"), np.float32)
            rows.append(a)
        Ls = np.stack([lum(a) for a in rows])
        Lm = float(Ls.mean())
        b4 = float(np.mean([row_hp_std(L, 4) for L in Ls]))
        b8 = float(np.mean([row_hp_std(L, 8) for L in Ls]))
        f8 = float(np.nanmean([flat_band(L) for L in Ls]))
        gy = np.mean([np.abs(np.diff(L, axis=1)).mean() for L in Ls])
        # 亮度代理：与页面 updateAdaptive 同一映射
        tL = 0.0 if Lm >= 75 else (1.0 if Lm <= 35 else (75 - Lm) / 40)
        # 实测门控：band4 相对行均值的比例
        rel = b4 / max(Lm, 1)
        tM = 0.0 if rel < 0.05 else (1.0 if rel > 0.20 else (rel - 0.05) / 0.15)
        # 用实测门控强度压制，看残留
        out = [flicker_row_norm(L, 4, tM) for L in Ls]
        b4o = float(np.mean([row_hp_std(L, 4) for L in out]))
        print("%-16s %6.1f %7.2f %6.2f %6.3f %8.2f %8.2f %8.2f %10.2f %6.1f%%"
              % (d, Lm, b4, b8, f8, gy, tL, tM, b4o, 100 * b4o / max(b4, 1e-6)))
        del rows


if __name__ == "__main__":
    main()

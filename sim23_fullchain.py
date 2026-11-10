# -*- coding: utf-8 -*-
"""sim23: 整条页面管线（flicker -> EMA(比值守卫) -> 方向性纵低通 -> 水平锐化）
的 Python 精确复刻，用来跨光亮度量端到端 band4 / flat / sharp，
判断 17:00~18:36 之间是否存在「条带压不下去」的死区。

指标口径与页面 measureFrame 完全一致：最平坦 10 个 80x80 块，
flat 用 17 抽头高通、band4 用 9 抽头高通。
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


def to_L(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def measure(a):
    """复刻 measureFrame：返回 flat / band4 / sharp。"""
    L = to_L(a)
    h, w = L.shape
    BW = BH = 80
    blocks = []
    for by in range(0, h - BH + 1, BH):
        for bx in range(0, w - BW + 1, BW):
            p = L[by:by + BH, bx:bx + BW]
            blocks.append((p.std(), by, bx))
    blocks.sort(key=lambda t: t[0])
    hp8, hp4 = [], []
    for _, by, bx in blocks[:10]:
        rr = L[by:by + BH, bx:bx + BW].mean(axis=1)
        for k, dst in ((8, hp8), (4, hp4)):
            ext = np.pad(rr, (k, k), mode="edge")
            sm = np.convolve(ext, np.ones(2 * k + 1) / (2 * k + 1), mode="same")[k:-k]
            dst.extend(rr - sm)
    flat = float(np.std(hp8)) if hp8 else float("nan")
    # band4 与页面一致：整幅行均值的 9 抽头高通残差
    rr = L.mean(axis=1)
    ext = np.pad(rr, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    b4 = float(np.std(rr - sm))
    gx = np.abs(np.diff(L, axis=1)).mean()
    return flat, b4, float(gx)


def flicker_norm(a, s):
    if s <= 0:
        return a
    r = to_L(a).mean(axis=1)
    ext = np.pad(r, (4, 4), mode="edge")
    sm = np.convolve(ext, np.ones(9) / 9.0, mode="same")[4:-4]
    g = np.clip(1.0 - s * (r - sm) / np.maximum(r, 1.0), 0.5, 2.0)
    return a * g[:, None, None]


def ema_step(acc, cur, a_base):
    """比值守卫 EMA：局部差值 / 全局差值 越大越像局部运动，权重越小。"""
    if acc is None:
        return cur.copy()
    dcur = to_L(cur)
    dacc = to_L(acc)
    diff = np.abs(dcur - dacc)
    mad = float(diff.mean())
    loc = diff
    loc = (np.pad(loc, ((0, 0), (1, 1),), mode="edge")[:, :-2]
           + loc + np.pad(loc, ((0, 0), (1, 1)), mode="edge")[:, 2:]) / 3.0
    loc = (np.pad(loc, ((1, 1), (0, 0)), mode="edge")[:-2]
           + loc + np.pad(loc, ((1, 1), (0, 0)), mode="edge")[2:]) / 3.0
    ratio = loc / (mad + 1.0)
    wgt = np.clip(1.0 - (ratio - 2.0) / 1.5, 0.0, 1.0)
    ae = a_base * wgt
    return acc * ae[..., None] + cur * (1.0 - ae[..., None])


def vlp(a, K, lam):
    if K < 1 or lam <= 0:
        return a
    L = to_L(a)
    h, w = L.shape
    gx = np.zeros_like(L)
    gx[:, 1:-1] = np.abs(L[:, 2:] - L[:, :-2]) / 2.0
    gx = gx * 0.5 + 0.25 * (np.roll(gx, 1, 1) + np.roll(gx, -1, 1))
    mask = np.clip((gx - 2.0) / 8.0, 0.0, 1.0)
    ext = np.pad(a, ((K, K), (0, 0), (0, 0)), mode="edge")
    vavg = np.zeros_like(a)
    for j in range(2 * K + 1):
        vavg += ext[j:j + h]
    vavg /= (2 * K + 1)
    wgt = (lam * (1.0 - mask))[..., None]
    return a + wgt * (vavg - a)


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
    am = (amt * mask)[..., None]
    return a + am * (a - blur)


def run(fs, s_flick, alpha, K, lam, sharp, npass=1):
    acc = None
    fo, bo, so = [], [], []
    for f in fs:
        a = np.asarray(Image.open(f).convert("RGB"), np.float32)
        for _ in range(npass):
            a = flicker_norm(a, s_flick)
        acc = ema_step(acc, a, alpha)
        b = vlp(acc, K, lam)
        b = sharp_h(b, sharp)
        flat, b4, sh = measure(b)
        fo.append(flat); bo.append(b4); so.append(sh)
    return float(np.nanmean(fo)), float(np.nanmean(bo)), float(np.nanmean(so))


def main():
    print("组        L      raw(flat/band4/sharp)      处理后(flat/band4/sharp)   band4压制")
    for name, pat in SETS:
        fs = sorted(glob.glob(os.path.join(HERE, pat)))
        if not fs:
            continue
        L = float(np.mean([to_L(np.asarray(Image.open(f).convert("RGB"), np.float32)).mean()
                           for f in fs]))
        rflat, rb4, rsh = [], [], []
        for f in fs:
            a = np.asarray(Image.open(f).convert("RGB"), np.float32)
            x = measure(a)
            rflat.append(x[0]); rb4.append(x[1]); rsh.append(x[2])
        rf, rb, rs = float(np.nanmean(rflat)), float(np.nanmean(rb4)), float(np.nanmean(rsh))
        s = 0.85 if L < 95 else 0.0
        t = min(1.0, max(0.0, (105 - L) / 70))
        alpha = 0.80 + 0.05 * t
        lam = 0.8 + 0.10 * t if L > 60 else 1.0
        sh = 0.80 - 0.20 * t
        of, ob, os_ = run(fs, s, alpha, 4, lam, sh, 1)
        of2, ob2, os2 = run(fs, s, alpha, 4, lam, sh, 2)
        print("%-8s %6.1f  %5.2f/%5.2f/%5.2f   一遍 %5.2f/%5.2f/%5.2f(%3.0f%%)  两遍 %5.2f/%5.2f/%5.2f(%3.0f%%)"
              % (name, L, rf, rb, rs, of, ob, os_, 100 * (1 - ob / max(rb, 1e-6)),
                 of2, ob2, os2, 100 * (1 - ob2 / max(rb, 1e-6))))


if __name__ == "__main__":
    main()

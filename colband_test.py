# -*- coding: utf-8 -*-
"""colband_test: 逐帧列条带校正（column FPN，单帧估计）离线评估。

与之前的 fixedColProfile 不同：不跨帧学习，只在当前帧内
把「行带内列方向高通残差」的均值当作条带，减掉。
评估 col4（列均值 9 抽头高通）、acut（x 向强边缘 98 分位）、
flat、以及竖直边缘保留度。
"""
import glob

import numpy as np
from PIL import Image

SETS = [
    ("18:20", "frames_dusk_1820/*.jpg"),
    ("18:36", "frames_dusk_1836/*.jpg"),
    ("18:40", "frames_day2night/f_1839*.jpg"),
]


def to_L(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def hp_col(L, k=4):
    cm = L.mean(axis=0)
    ext = np.pad(cm, (k, k), mode="edge")
    sm = np.convolve(ext, np.ones(2 * k + 1) / (2 * k + 1), mode="same")[k:-k]
    return float(np.std(cm - sm))


def acut(L):
    g = np.abs(np.diff(L, axis=1))
    return float(np.percentile(g, 98))


def flat(L):
    h, w = L.shape
    bw = bh = 80
    blocks = []
    for by in range(0, h - bh + 1, bh):
        for bx in range(0, w - bw + 1, bw):
            blocks.append((L[by:by + bh, bx:bx + bw].std(), by, bx))
    blocks.sort()
    vals = []
    for _, by, bx in blocks[:10]:
        rr = L[by:by + bh, bx:bx + bw].mean(axis=1)
        ext = np.pad(rr, (8, 8), mode="edge")
        sm = np.convolve(ext, np.ones(17) / 17.0, mode="same")[8:-8]
        vals.extend(rr - sm)
    return float(np.std(vals))


def blur_h(L, k=2):
    """列方向 5 抽头均值。"""
    ext = np.pad(L, ((0, 0), (k, k)), mode="edge")
    c = np.cumsum(ext, axis=1)
    c = np.concatenate([np.zeros((L.shape[0], 1)), c], axis=1)
    n = 2 * k + 1
    return (c[:, n:] - c[:, :-n]) / n


def est_coloff(L, band=16, clamp=8.0, guard=0.0):
    """逐行带估计列条带：残差在 band 行内取均值。"""
    h, w = L.shape
    res = L - blur_h(L)
    off = np.zeros(w)
    cnt = np.zeros(w)
    for y0 in range(0, h, band):
        y1 = min(y0 + band, h)
        blk = res[y0:y1]
        m = blk.mean(axis=0)
        if guard > 0:
            # 该列的残差在行方向不稳定 => 多半是真实竖直纹理，压低权重
            sd = blk.std(axis=0) + 1e-6
            wt = 1.0 / (1.0 + (sd / guard) ** 2)
            m = m * wt
            off += m
            cnt += wt
        else:
            off += m * (y1 - y0)
            cnt += (y1 - y0)
    off = off / np.maximum(cnt, 1e-6)
    lo, hi = np.percentile(off, [5, 95])
    return np.clip(off, -clamp, clamp), (lo, hi)


def apply_off(L, off, band=16, strength=1.0):
    h, w = L.shape
    out = L.copy()
    for y0 in range(0, h, band):
        y1 = min(y0 + band, h)
        out[y0:y1] -= strength * off
    return out


VARIANTS = [
    ("C0 原始", dict(mode=0)),
    ("C1 整幅 band=h", dict(band=10 ** 6, clamp=8, guard=0)),
    ("C2 band=32 紧夹3", dict(band=32, clamp=3, guard=0)),
    ("C3 band=16 夹3 守卫0.6", dict(band=16, clamp=3, guard=0.6)),
    ("C4 band=16 夹2 守卫0.4", dict(band=16, clamp=2, guard=0.4)),
]


def main():
    rows = {}
    for name, pat in SETS:
        fs = sorted(glob.glob(pat))[:4]
        if not fs:
            continue
        acc = {v[0]: [] for v in VARIANTS}
        for f in fs:
            a = np.asarray(Image.open(f).convert("RGB"), dtype=np.float32)
            L = to_L(a)
            for vn, kw in VARIANTS:
                if kw.get("mode") == 0:
                    Lv = L
                else:
                    off, _ = est_coloff(L, kw["band"], kw["clamp"], kw["guard"])
                    Lv = apply_off(L, off, kw["band"], 1.0)
                acc[vn].append((hp_col(Lv), acut(Lv), flat(Lv)))
        print(f"\n[{name}]  n={len(fs)}")
        for vn, _ in VARIANTS:
            m = np.array(acc[vn]).mean(axis=0)
            print(f"  {vn:22s} col4={m[0]:.2f}  acut={m[1]:6.2f}  flat={m[2]:.3f}")
        rows[name] = acc
    # 保存一组对比图
    f = sorted(glob.glob(SETS[1][1]))[0]
    a = np.asarray(Image.open(f).convert("RGB"), dtype=np.float32)
    L = to_L(a)
    off, (lo, hi) = est_coloff(L, 16, 3, 0.6)
    L2 = apply_off(L, off, 16, 1.0)
    print(f"\n条带估计 off: min={off.min():.2f} max={off.max():.2f} 5/95分位={lo:.2f}/{hi:.2f}")
    crop = (slice(0, 200), slice(120, 420))
    base = np.clip(L[crop], 0, 255).astype(np.uint8)
    fix = np.clip(L2[crop], 0, 255).astype(np.uint8)
    img = np.concatenate([base, np.full((200, 8), 128, np.uint8), fix], axis=1)
    Image.fromarray(img).resize((img.shape[1] * 2, img.shape[0] * 2), Image.NEAREST).save("colband_ab.png")
    print("已保存 colband_ab.png")


if __name__ == "__main__":
    main()

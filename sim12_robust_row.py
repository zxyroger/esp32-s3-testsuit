# -*- coding: utf-8 -*-
"""SVGA(800x600) 真实帧上的行偏置估计算法对比。

现状 fixedRowProfile 用「整段行均值 - 纵向低通」跨帧累积，静态楼房的窗户
横排会被当成传感器固定偏置烤进 profile，减到平坦天空上反而造出横带。
本脚本验证「逐帧 + 只取垂直平坦像素 + 分段稳健中位数」是否能真正压掉
平坦区横纹，同时不伤锐度。所有方案共用同一套 FLAT/TEX 块，指标可比。
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

PAT = sys.argv[1] if len(sys.argv) > 1 else 'frames_1405/cam_*.jpg'
FR = sorted(glob.glob(PAT))
if not FR:
    FR = sorted(glob.glob('frames_day2night/f_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
print('帧 %d 张, %dx%d, 来自 %s' % (len(D), W, H, FR[0]))


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def sm(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


# ---------- 与 cam_view_check.py 完全一致的指标 ----------
L0 = lum(D[len(D) // 2])
BW = BH = 80
blocks = sorted((float(L0[by:by + BH, bx:bx + BW].std()), by, bx)
                for by in range(0, H - BH + 1, BH)
                for bx in range(0, W - BW + 1, BW))
FLAT = [(by, bx) for _, by, bx in blocks[:10]]
TEX = [(by, bx) for _, by, bx in blocks[-10:]]
FLAT_DARK = [(by, bx) for by, bx in FLAT
             if L0[by:by + BH, bx:bx + BW].mean() < 100]
print('平坦块 %d 个 (其中暗块 %d 个)' % (len(FLAT), len(FLAT_DARK)))


def band_of(d, blks):
    L = lum(d)
    hp = []
    for by, bx in blks:
        rr = L[by:by + BH, bx:bx + BW].mean(axis=1)
        hp.extend((rr - sm(rr, 8)).tolist())
    return float(np.std(hp)) if hp else 0.0


def flat_band(d):
    return band_of(d, FLAT)


def dark_band(d):
    return band_of(d, FLAT_DARK)


def sharp(d):
    L = lum(d)
    vals = []
    for by, bx in TEX:
        p = L[by:by + BH, bx:bx + BW]
        vals.append(float(np.abs(np.diff(p, axis=1)).mean() +
                          np.abs(np.diff(p, axis=0)).mean()))
    return float(np.mean(vals))


def full_band(d):
    r = lum(d).mean(axis=1)
    return float((r - sm(r, 10)).std())


# ---------- 现有管线的 numpy 复刻 ----------
def block_profile(frames, K=25, nseg=8):
    sw = W // nseg
    P = np.zeros((nseg, 3, H))
    for d in frames:
        for s in range(nseg):
            sub = d[:, s * sw:(s + 1) * sw, :]
            for c in range(3):
                rm = sub[..., c].mean(axis=1)
                P[s, c] += rm - sm(rm, K)
    return P / len(frames), sw


def apply_block(d, P, sw, strength=1.0):
    nseg = P.shape[0]
    xs = np.arange(W, dtype=np.float32)
    t = np.clip(xs / sw - 0.5, 0, nseg - 1)
    s0 = np.floor(t).astype(int)
    s1 = np.minimum(s0 + 1, nseg - 1)
    f = (t - s0)[None, :]
    o = d.copy()
    for c in range(3):
        a = P[:, c, :].T
        o[..., c] = np.clip(o[..., c] -
                            strength * (a[:, s0] * (1 - f) + a[:, s1] * f), 0, 255)
    return o


def row_normalize(d, s=0.3):
    rm = lum(d).mean(axis=1)
    g = np.clip(sm(rm, 6) / np.maximum(rm, 1), 0.3, 3.0)
    return np.clip(d * (1 + (g - 1) * s)[:, None, None], 0, 255)


def row_offset(d):
    L = lum(d)
    lo = np.percentile(L, 8, axis=1)
    off = lo - sm(lo, 12)
    wgt = np.clip(1 - L / 90, 0, 1)
    return np.clip(d - (off[:, None] * wgt)[..., None], 0, 255)


def desat_dark(d, thresh=80):
    L = lum(d)[..., None]
    t = np.clip(L / thresh, 0, 1)
    g = d.mean(axis=2, keepdims=True)
    return g + (d - g) * t


def box3(d):
    p = np.pad(d, ((0, 0), (1, 1), (0, 0)), mode='edge')
    t = (p[:, :-2, :] + p[:, 1:-1, :] + p[:, 2:, :]) / 3
    q = np.pad(t, ((1, 1), (0, 0), (0, 0)), mode='edge')
    return (q[:-2, :, :] + q[1:-1, :, :] + q[2:, :, :]) / 3


def local_grad(d):
    L = lum(d)
    gx = np.abs(np.diff(L, axis=1, append=L[:, -1:]))
    gy = np.abs(np.diff(L, axis=0, append=L[-1:, :]))
    g = (gx + gy) / 2
    p = np.pad(g, 1, mode='edge')
    s = np.zeros_like(g)
    for dy in range(3):
        for dx in range(3):
            s += np.roll(np.roll(p, dy, 0), dx, 1)[1:-1, 1:-1]
    return s / 9


def unsharp_edge(d, amount=0.30, Kg=8):
    hi = d - box3(d)
    mask = np.clip((local_grad(d) - 2) / Kg, 0, 1)[..., None]
    return np.clip(d + amount * hi * mask, 0, 255)


def ema_fixed(frames, a=0.55):
    acc = None
    out = []
    for d in frames:
        d = acc * a + d * (1 - a) if acc is not None else d.copy()
        acc = d.copy()
        out.append(d)
    return out


# ---------- 新算法：逐帧稳健行偏置 ----------
def robust_row(d, nseg=8, K=8, T=8, strength=1.0, clip=12.0):
    """只取「垂直方向平坦」的像素估计每行偏置，避开楼房水平边缘。

    1) 纵向 K 半径 box 低通当场景低频，R = L - low；
    2) 垂直梯度 |L(y+1)-L(y-1)| < T 的像素才参与统计（排除水平边缘）；
    3) 每段每行取 R 的中位数 —— 场景的局部结构被中位数抑制，
       而整行一致的传感器偏置保留下来；
    4) 段中心之间线性插值后按行减掉。
    """
    L = lum(d)
    h, w = L.shape
    p = np.pad(L, ((K, K), (0, 0)), mode='edge')
    cs = np.vstack([np.zeros((1, w), np.float64), np.cumsum(p, axis=0)])
    low = (cs[2 * K + 1:2 * K + 1 + h] - cs[0:h]) / (2 * K + 1)
    R = L - low

    gy = np.zeros_like(L)
    gy[1:-1] = np.abs(L[2:] - L[:-2])
    mask = gy < T

    sw = w // nseg
    prof = np.zeros((nseg, h), np.float32)
    for s in range(nseg):
        x0, x1 = s * sw, (s + 1) * sw
        Rs, ms = R[:, x0:x1], mask[:, x0:x1]
        for y in range(h):
            v = Rs[y][ms[y]]
            prof[s, y] = np.median(v) if v.size > sw * 0.25 else 0.0
    np.clip(prof, -clip, clip, out=prof)

    xs = np.arange(w, dtype=np.float32)
    t = np.clip(xs / sw - 0.5, 0, nseg - 1)
    s0 = np.floor(t).astype(int)
    s1 = np.minimum(s0 + 1, nseg - 1)
    f = (t - s0)[None, :]
    a = prof.T                                    # (h, nseg)
    rowp = a[:, s0] * (1 - f) + a[:, s1] * f      # (h, w)
    return np.clip(d - (strength * rowp)[..., None], 0, 255), rowp


def post(d):
    return unsharp_edge(desat_dark(row_offset(row_normalize(d))))


def report(name, frames):
    k = len(frames) // 2
    d = frames[k]
    print('%-34s flat=%6.3f dark=%6.3f full=%6.3f sharp=%6.2f' %
          (name, flat_band(d), dark_band(d), full_band(d), sharp(d)))
    return d


E = ema_fixed(D, 0.55)
P, SW = block_profile(E, 25, 8)

report('原始(无处理)', D)
report('EMA only', E)
CUR = [post(apply_block(d, P, SW)) for d in E]
report('现状完整管线', CUR)

for nseg in (1, 4, 8, 16):
    for K in (4, 8, 12):
        RR = []
        for d in E:
            o, _ = robust_row(d, nseg=nseg, K=K)
            RR.append(post(o))
        report('稳健行 nseg=%d K=%d +post' % (nseg, K), RR)

# 与现有分块 profile 叠加（现有降权）
for st in (0.0, 0.3, 0.5):
    OUT = []
    for d in E:
        o, _ = robust_row(d, nseg=8, K=8)
        o = apply_block(o, P, SW, strength=st)
        OUT.append(post(o))
    report('稳健行 + 旧profile×%.1f' % st, OUT)

# 保存视觉对比（取最平坦暗块附近裁剪 + 全图缩略）
def save_cmp(frames_dict):
    tiles = []
    for name, d in frames_dict.items():
        im = Image.fromarray(np.clip(d, 0, 255).astype(np.uint8))
        im.thumbnail((400, 300))
        tiles.append((name, im))
    out = Image.new('RGB', (400 * 2, 330 * ((len(tiles) + 1) // 2)), (250, 250, 250))
    from PIL import ImageDraw
    dr = ImageDraw.Draw(out)
    for i, (name, im) in enumerate(tiles):
        x, y = (i % 2) * 400, (i // 2) * 330
        out.paste(im, (x, y + 26))
        dr.text((x + 6, y + 6), name, fill=(0, 0, 0))
    out.save('sim12_compare.png')
    print('已保存 sim12_compare.png')


k = len(E) // 2
new, _ = robust_row(E[k], nseg=8, K=8)
save_cmp({
    'raw': D[k],
    'current pipeline': CUR[k],
    'robust row only': post(new),
    'robust + old x0.3': post(apply_block(new, P, SW, 0.3)),
})

# -*- coding: utf-8 -*-
"""方向性平坦区纵向低通：横纹沿 x 方向几乎不变，楼房细节在 x 方向有梯度。
用「水平梯度」做掩码，只在 gx 小的像素上做跨行低通，理应压掉行纹而不糊细节。
在 SVGA 真实帧上与现状管线、稳健行 profile 组合对比。
"""
import glob
import sys

import numpy as np
from PIL import Image, ImageDraw

PAT = sys.argv[1] if len(sys.argv) > 1 else 'frames_1405/cam_*.jpg'
FR = sorted(glob.glob(PAT))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
print('帧 %d 张 %dx%d' % (len(D), W, H))


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def sm(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


L0 = lum(D[len(D) // 2])
BW = BH = 80
blocks = sorted((float(L0[by:by + BH, bx:bx + BW].std()), by, bx)
                for by in range(0, H - BH + 1, BH)
                for bx in range(0, W - BW + 1, BW))
FLAT = [(by, bx) for _, by, bx in blocks[:10]]
TEX = [(by, bx) for _, by, bx in blocks[-10:]]
FLAT_DARK = [(by, bx) for by, bx in FLAT
             if L0[by:by + BH, bx:bx + BW].mean() < 100]


def band_of(d, blks):
    L = lum(d)
    hp = []
    for by, bx in blks:
        rr = L[by:by + BH, bx:bx + BW].mean(axis=1)
        hp.extend((rr - sm(rr, 8)).tolist())
    return float(np.std(hp)) if hp else 0.0


def sharp(d):
    L = lum(d)
    return float(np.mean([
        np.abs(np.diff(L[by:by + BH, bx:bx + BW], axis=1)).mean() +
        np.abs(np.diff(L[by:by + BH, bx:bx + BW], axis=0)).mean()
        for by, bx in TEX]))


def report(name, d):
    print('%-40s flat=%6.3f dark=%6.3f sharp=%6.2f' %
          (name, band_of(d, FLAT), band_of(d, FLAT_DARK), sharp(d)))


# ---------- 现有管线 ----------
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


# ---------- 方向性平坦区纵向低通 ----------
def vblur(d, K=1):
    """沿 y 的 (2K+1) 行 box 均值"""
    p = np.pad(d, ((K, K), (0, 0), (0, 0)), mode='edge')
    cs = np.vstack([np.zeros((1, W, 3), np.float64), np.cumsum(p, axis=0)])
    return (cs[2 * K + 1:2 * K + 1 + H] - cs[0:H]) / (2 * K + 1)


def flat_vlp(d, lam=1.0, K=1, mode='gx', G0=2.0, Kg=8.0):
    L = lum(d)
    gx = np.abs(np.diff(L, axis=1, append=L[:, -1:]))
    gy = np.abs(np.diff(L, axis=0, append=L[-1:, :]))
    if mode == 'gx':
        g = gx
    elif mode == 'gx2gy':
        g = gx * 0.75 + gy * 0.25
    else:
        g = (gx + gy) / 2
    # 3x3 平滑掩码，避免单点噪声
    p = np.pad(g, 1, mode='edge')
    s = np.zeros_like(g)
    for dy in range(3):
        for dx in range(3):
            s += np.roll(np.roll(p, dy, 0), dx, 1)[1:-1, 1:-1]
    s /= 9
    mask = np.clip((s - G0) / Kg, 0, 1)      # 纹理 ->1 保留原图
    w = lam * (1 - mask)                       # 平坦 ->lam 用纵向均值
    return np.clip(d + w[..., None] * (vblur(d, K) - d), 0, 255)


E = ema_fixed(D, 0.55)
P, SW = block_profile(E, 25, 8)
k = len(E) // 2
CUR = unsharp_edge(desat_dark(row_offset(row_normalize(apply_block(E[k], P, SW)))))

report('原始', D[k])
report('现状管线', CUR)

for mode in ('gx', 'gx2gy', 'both'):
    for K in (1, 2, 3):
        for lam in (0.6, 1.0):
            o = flat_vlp(CUR, lam=lam, K=K, mode=mode)
            report('现状+纵低通 %s K=%d lam=%.1f' % (mode, K, lam), o)

# 放在锐化之前（避免锐化把被抹平的行纹又补回来）
BASE = desat_dark(row_offset(row_normalize(apply_block(E[k], P, SW))))
for K in (1, 2, 3):
    o = unsharp_edge(flat_vlp(BASE, lam=1.0, K=K, mode='gx'))
    report('纵低通(锐化前) K=%d' % K, o)

# 纯纵低通，不叠加旧 profile（看它自身能力）
for K in (1, 2, 3):
    o = unsharp_edge(desat_dark(row_offset(row_normalize(
        flat_vlp(E[k], lam=1.0, K=K, mode='gx')))))
    report('无profile+纵低通 K=%d' % K, o)


def save_cmp(items):
    tiles = []
    for name, d in items:
        im = Image.fromarray(np.clip(d, 0, 255).astype(np.uint8))
        im.thumbnail((400, 300))
        tiles.append((name, im))
    out = Image.new('RGB', (800, 330 * ((len(tiles) + 1) // 2)), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for i, (name, im) in enumerate(tiles):
        x, y = (i % 2) * 400, (i // 2) * 330
        out.paste(im, (x, y + 26))
        dr.text((x + 6, y + 6), name, fill=(0, 0, 0))
    out.save('sim13_compare.png')
    print('已保存 sim13_compare.png')


save_cmp([
    ('raw', D[k]),
    ('current pipeline', CUR),
    ('current + vlp K1 gx', flat_vlp(CUR, 1.0, 1, 'gx')),
    ('pre-sharp vlp K2', unsharp_edge(flat_vlp(BASE, 1.0, 2, 'gx'))),
])

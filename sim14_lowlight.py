# -*- coding: utf-8 -*-
"""暗光自适应研究：精确复刻 viewer/app.js 的降噪管线到 numpy，
在白天 SVGA 帧、真实夜间 QVGA 帧、以及数字增益模拟的中间亮度帧上做参数扫描，
找出「画面亮度 -> 最优降噪参数」的规律。

用法: python sim14_lowlight.py [quick|full]
"""
import glob
import itertools
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

MODE = sys.argv[1] if len(sys.argv) > 1 else 'quick'
rng = np.random.default_rng(20261110)


# ---------------- 基础工具 ----------------
def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def hbox3_2d(a):
    """横向 3 抽头，edge 边界（2D 单通道）"""
    p = np.pad(a, ((0, 0), (1, 1)), mode='edge')
    return (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3


def vbox1_2d(a):
    """纵向 3 抽头，edge 边界（2D 单通道）"""
    p = np.pad(a, ((1, 1), (0, 0)), mode='edge')
    return (p[:-2, :] + p[1:-1, :] + p[2:, :]) / 3


def hbox3_3d(a):
    p = np.pad(a, ((0, 0), (1, 1), (0, 0)), mode='edge')
    return (p[:, :-2, :] + p[:, 1:-1, :] + p[:, 2:, :]) / 3


def vboxk_3d(a, K):
    H = a.shape[0]
    p = np.pad(a, ((K, K), (0, 0), (0, 0)), mode='edge')
    cs = np.vstack([np.zeros((1, a.shape[1], 3), np.float64), np.cumsum(p, axis=0)])
    return (cs[2 * K + 1:2 * K + 1 + H] - cs[:H]) / (2 * K + 1)


def sm(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


# ---------------- 指标（与 app.js measureFrame 一致） ----------------
class Meter:
    def __init__(self, ref, BW=80, BH=80):
        self.H, self.W = ref.shape[:2]
        BW = min(BW, self.W)
        BH = min(BH, self.H)
        self.BW, self.BH = BW, BH
        L0 = lum(ref)
        blks = sorted((float(L0[by:by + BH, bx:bx + BW].std()), by, bx)
                      for by in range(0, self.H - BH + 1, BH)
                      for bx in range(0, self.W - BW + 1, BW))
        self.flat = [(by, bx) for _, by, bx in blks[:min(10, len(blks))]]
        self.tex = [(by, bx) for _, by, bx in blks[-min(10, len(blks)):]]
        self.flat_dark = [(by, bx) for by, bx in self.flat
                          if L0[by:by + BH, bx:bx + BW].mean() < 80]
        self.mean = float(L0.mean())

    def band(self, a, blks=None):
        L = lum(a)
        blks = self.flat if blks is None else blks
        hp = []
        for by, bx in blks:
            rr = L[by:by + self.BH, bx:bx + self.BW].mean(axis=1)
            hp.extend((rr - sm(rr, 8)).tolist())
        return float(np.std(hp)) if hp else 0.0

    def sharp(self, a):
        L = lum(a)
        vals = []
        for by, bx in self.tex:
            b = L[by:by + self.BH, bx:bx + self.BW]
            vals.append(np.abs(np.diff(b, axis=1)).mean() + np.abs(np.diff(b, axis=0)).mean())
        return float(np.mean(vals))

    def report(self, a):
        return '%6.3f %6.3f %6.2f' % (self.band(a), self.band(a, self.flat_dark) if self.flat_dark else float('nan'), self.sharp(a))


# ---------------- 管线组件 ----------------
def ema_seq(frames, alpha=0.80, m0=7, span=3):
    acc = None
    out = []
    for d in frames:
        if acc is None:
            acc = d.copy()
            out.append(d.copy())
            continue
        diff = np.abs(d - acc).mean(axis=2)
        mad = float(diff.mean())
        a = 0.30 if mad > 16 else alpha
        guard = float(np.clip((mad - m0) / span, 0, 1))
        if guard > 0:
            tmp = hbox3_2d(diff)
            loc = vbox1_2d(tmp)
            wgt = np.clip(1 - guard * loc / 10.0, 0, 1)
            ae = a * wgt
        else:
            ae = np.full(diff.shape, a, np.float32)
        cur = acc * ae[..., None] + d * (1 - ae[..., None])
        acc = cur.copy()
        out.append(cur.copy())
    return out


def desat_dark(a, thresh=80):
    L = lum(a)[..., None]
    t = np.clip(L / thresh, 0, 1)
    g = a.mean(axis=2, keepdims=True)
    return g + (a - g) * t


def directional_vlp(a, K=2, lam=0.8, g0=2.0, kg=8.0):
    L = lum(a)
    lx = np.concatenate([L[:, 1:], L[:, -1:]], axis=1)
    ly = np.concatenate([L[1:, :], L[-1:, :]], axis=0)
    g = (np.abs(L - lx) + np.abs(L - ly)) / 2
    gl = np.concatenate([g[:, :1], g[:, :-1]], axis=1)
    gr = np.concatenate([g[:, 1:], g[:, -1:]], axis=1)
    gu = np.concatenate([g[:1, :], g[:-1, :]], axis=0)
    gd = np.concatenate([g[1:, :], g[-1:, :]], axis=0)
    loc = (gl + g + gr + gu + gd) / 5
    mask = np.clip((loc - g0) / kg, 0, 1)
    wgt = lam * (1 - mask)
    avg = vboxk_3d(a, K)
    return np.clip(a + wgt[..., None] * (avg - a), 0, 255)


def sharpen_h(a, amount=0.80, g0=2.0, kg=8.0, horizontal=True):
    blur_h = hbox3_3d(a)
    L = lum(a)
    lr = np.concatenate([L[:, 1:], L[:, -1:]], axis=1)
    ld = np.concatenate([L[1:, :], L[-1:, :]], axis=0)
    gh = (np.abs(L - lr) + np.abs(L - ld)) / 2
    gv = hbox3_2d(gh)
    gu = np.concatenate([gv[:1, :], gv[:-1, :]], axis=0)
    gd = np.concatenate([gv[1:, :], gv[-1:, :]], axis=0)
    loc = (gu + gv + gd) / 3
    mask = np.clip((loc - g0) / kg, 0, 1)
    am = amount * mask
    blur = blur_h if horizontal else vboxk_3d(blur_h, 1)
    return np.clip(a + am[..., None] * (a - blur), 0, 255)


def pipeline(frames, P, k=None):
    """P: dict(alpha,m0,span,vlpK,vlpLam,vlpG0,vlpKg,sharp,sharpH,desat,vlp,sharpOn)"""
    E = ema_seq(frames, P['alpha'], P['m0'], P['span'])
    k = len(E) // 2 if k is None else k
    d = E[k].copy()
    d = desat_dark(d, P['desat'])
    if P['vlp']:
        d = directional_vlp(d, P['vlpK'], P['vlpLam'], P['vlpG0'], P['vlpKg'])
    if P['sharpOn']:
        d = sharpen_h(d, P['sharp'], horizontal=bool(P['sharpH']))
    return d


BASE = dict(alpha=0.80, m0=7, span=3, vlpK=2, vlpLam=0.8, vlpG0=2, vlpKg=8,
            sharp=0.80, sharpH=1, desat=80, vlp=1, sharpOn=1)


# ---------------- 数据加载 ----------------
def load(pat, limit=12):
    fs = sorted(glob.glob(pat))[:limit]
    return [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in fs]


def dim_frames(frames, target_L, rowpat, gain_pow=0.5):
    """物理近似的低照度模拟：曝光降低 → gamma 域变暗；AGC 部分补偿 →
    读出噪声与固定行 FPN 按 gain 放大。rowpat 从真实帧估计的固定行偏置。"""
    out = []
    for a in frames:
        lin = (np.clip(a, 0, 255) / 255.0) ** 2.2
        lo, hi = 0.01, 1.0
        for _ in range(18):
            r = (lo + hi) / 2
            v = np.clip(lin * r, 0, 1) ** (1 / 2.2) * 255
            if lum(v).mean() > target_L:
                hi = r
            else:
                lo = r
        v = np.clip(lin * (lo + hi) / 2, 0, 1) ** (1 / 2.2) * 255
        gain = max(1.0, (lum(a).mean() / max(target_L, 1)) ** gain_pow)
        extra = (gain - 1)
        v = v + rowpat * extra * 1.0
        v = v + rng.normal(0, 1.1 * extra, v.shape)
        out.append(np.clip(v, 0, 255).astype(np.float32))
    return out


def estimate_rowpat(frames):
    """从多帧平均里提取固定行偏置（每行、每通道、沿列的变化用高通去掉场景）。"""
    m = np.mean(frames, axis=0)
    L = lum(m)
    r = L.mean(axis=1)
    hp = r - sm(r, 25)
    return np.clip(hp, -6, 6)[:, None, None] * np.ones((1, 1, 3), np.float32)


# ---------------- 主流程 ----------------
def scan(name, frames, tags=''):
    if len(frames) < 3:
        print('%s: 帧不足' % name)
        return None
    k = len(frames) // 2
    M = Meter(frames[k])
    raw = frames[k]
    print('\n=== %s  %dx%d  L=%.1f  帧=%d  %s ===' %
          (name, M.W, M.H, M.mean, len(frames), tags))
    print('%-42s %6s %6s %6s' % ('配置', 'flat', 'dark', 'sharp'))
    print('%-42s %s' % ('原始', M.report(raw)))
    cur = pipeline(frames, BASE, k)
    print('%-42s %s' % ('当前发布参数', M.report(cur)))

    # 阶段 1：固定 EMA，扫 vlp / sharp
    best = None
    grid = itertools.product((1, 2, 3, 4), (0.6, 0.8, 1.0), (0.4, 0.8, 1.2))
    E = ema_seq(frames, BASE['alpha'], BASE['m0'], BASE['span'])
    for K, lam, sh in grid:
        d = desat_dark(E[k].copy(), BASE['desat'])
        d = directional_vlp(d, K, lam, BASE['vlpG0'], BASE['vlpKg'])
        d = sharpen_h(d, sh, horizontal=True)
        f, dk, s = M.band(d), M.band(d, M.flat_dark) if M.flat_dark else 0, M.sharp(d)
        # 目标：横纹最小，锐度不低于原始 95%
        s0 = M.sharp(raw)
        score = f + max(0, s0 * 0.95 - s) * 0.6
        if best is None or score < best[0]:
            best = (score, K, lam, sh, f, s)
    print('%-42s %6.3f %6s %6.2f  (K=%d lam=%.1f sharp=%.1f)' %
          ('阶段1最优 vlp/sharp', best[4], '-', best[5], best[1], best[2], best[3]))

    # 阶段 2：用最优 vlp/sharp，扫 EMA alpha / m0
    P = dict(BASE, vlpK=best[1], vlpLam=best[2], sharp=best[3])
    best2 = None
    for alpha, m0 in itertools.product((0.60, 0.75, 0.85, 0.92), (4, 7, 10)):
        Pp = dict(P, alpha=alpha, m0=m0)
        d = pipeline(frames, Pp, k)
        f, s = M.band(d), M.sharp(d)
        s0 = M.sharp(raw)
        score = f + max(0, s0 * 0.95 - s) * 0.6
        if best2 is None or score < best2[0]:
            best2 = (score, alpha, m0, f, s)
    print('%-42s %6.3f %6s %6.2f  (alpha=%.2f m0=%d)' %
          ('阶段2最优 EMA', best2[3], '-', best2[4], best2[1], best2[2]))

    Pf = dict(P, alpha=best2[1], m0=best2[2])
    final = pipeline(frames, Pf, k)
    print('%-42s %s' % ('组合最优', M.report(final)))
    return dict(name=name, L=M.mean, raw=M.band(raw), cur=M.band(cur),
                best=M.band(final), rawSharp=M.sharp(raw),
                curSharp=M.sharp(cur), bestSharp=M.sharp(final), P=Pf, meter=M,
                rawim=raw, curim=cur, bestim=final)


def main():
    datasets = []
    day = load('frames_1405/cam_*.jpg', 12)
    if day:
        rp = estimate_rowpat(day)
        datasets.append(('白天 SVGA L118', day, '真实'))
        for tgt in (70, 45, 28):
            datasets.append(('模拟 L%d' % tgt, dim_frames(day, tgt, rp), '数字增益'))
    for nm, pat in [('夜间 QVGA L30', 'frames_clean/cam_*.jpg'),
                    ('夜间 QVGA L30b', 'frames_dn2/cam_*.jpg'),
                    ('极暗 QVGA L9', 'frames_50hz2/cam_*.jpg'),
                    ('极暗 QVGA L9b', 'frames_bl2/cam_*.jpg')]:
        f = load(pat, 8)
        if f:
            datasets.append((nm, f, '真实'))

    results = []
    for nm, fr, tag in datasets:
        r = scan(nm, fr, tag)
        if r:
            results.append(r)

    print('\n\n========== 亮度 -> 最优参数汇总 ==========')
    print('%-18s %6s %7s %7s %7s | %5s %5s %5s %6s %4s' %
          ('数据集', 'L', 'raw', 'cur', 'best', 'a', 'm0', 'K', 'lam', 'sh'))
    for r in results:
        P = r['P']
        print('%-18s %6.1f %7.3f %7.3f %7.3f | %5.2f %5d %5d %6.1f %4.1f' %
              (r['name'], r['L'], r['raw'], r['cur'], r['best'],
               P['alpha'], P['m0'], P['vlpK'], P['vlpLam'], P['sharp']))

    # 拼图：每个亮度级的 原始 / 当前 / 最优
    tiles = []
    for r in results[:4] + results[-2:]:
        for nm, im in [('raw', r['rawim']), ('cur', r['curim']), ('best', r['bestim'])]:
            p = Image.fromarray(np.clip(im, 0, 255).astype(np.uint8))
            p.thumbnail((240, 180))
            tiles.append(('%s %s' % (r['name'], nm), p))
    cols = 3
    out = Image.new('RGB', (240 * cols, 206 * ((len(tiles) + cols - 1) // cols)), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for i, (nm, im) in enumerate(tiles):
        x, y = (i % cols) * 240, (i // cols) * 206
        out.paste(im, (x, y + 24))
        dr.text((x + 4, y + 6), nm, fill=(0, 0, 0))
    out.save('sim14_compare.png')
    print('\n已保存 sim14_compare.png')


if __name__ == '__main__':
    main()

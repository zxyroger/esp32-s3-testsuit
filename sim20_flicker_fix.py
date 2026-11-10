# -*- coding: utf-8 -*-
"""入夜工频条带（周期约 4 行、帧间相位随机）抑制方案验证。
对比：
  A 原始
  B 强 EMA（无局部守卫）—— 靠多帧平均抵消随机相位
  C 当前发布（相对守卫 EMA + K4 纵低通）
  D 多帧中位数参考 + 行增益校正 + 强 EMA
  E 逐帧 4 行高通行归一化 + 强 EMA
指标：band4（行均值 4 行高通残差，专门测这种条纹）、flat8（旧指标）、sharp
"""
import glob
import sys

import numpy as np
from PIL import Image

from sim14_lowlight import desat_dark, directional_vlp, sharpen_h

PAT = sys.argv[1] if len(sys.argv) > 1 else 'frames_dusk_1720/*.jpg'
FR = sorted(glob.glob(PAT))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
print('帧 %d 张 %dx%d' % (len(D), W, H))


def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def mavg(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


def band4(a):
    r = lum(a).mean(axis=1)
    return float(np.std(r - mavg(r, 4)))


def flat8(a):
    L = lum(a)
    BW = BH = 80
    blks = sorted((float(L[y:y + BH, x:x + BW].std()), y, x)
                  for y in range(0, H - BH + 1, BH)
                  for x in range(0, W - BW + 1, BW))
    hp = []
    for _, y, x in blks[:10]:
        rr = L[y:y + BH, x:x + BW].mean(axis=1)
        hp.extend((rr - mavg(rr, 8)).tolist())
    return float(np.std(hp))


def sharp(a):
    L = lum(a)
    BW = BH = 80
    blks = sorted((float(L[y:y + BH, x:x + BW].std()), y, x)
                  for y in range(0, H - BH + 1, BH)
                  for x in range(0, W - BW + 1, BW))
    v = []
    for _, y, x in blks[-10:]:
        b = L[y:y + BH, x:x + BW]
        v.append(np.abs(np.diff(b, axis=1)).mean() + np.abs(np.diff(b, axis=0)).mean())
    return float(np.mean(v))


def smooth2(diff):
    p = np.pad(diff, ((0, 0), (1, 1)), mode='edge')
    t = (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3
    q = np.pad(t, ((1, 1), (0, 0)), mode='edge')
    return (q[:-2, :] + q[1:-1, :] + q[2:, :]) / 3


def ema(frames, alpha=0.85, guard=True, r0=2.0, rk=1.5):
    acc = None
    outs = []
    for d in frames:
        if acc is None:
            acc = d.copy(); outs.append(d.copy()); continue
        diff = np.abs(d - acc).mean(axis=2)
        mad = float(diff.mean())
        a = 0.30 if mad > 16 else alpha
        if guard:
            loc = smooth2(diff)
            wgt = np.clip(1 - (loc / (mad + 1.0) - r0) / rk, 0, 1)
            ae = a * wgt
        else:
            ae = np.full(diff.shape, a, np.float32)
        acc = acc * ae[..., None] + d * (1 - ae[..., None])
        outs.append(acc.copy())
    return outs


def post(frames, K=4, lam=0.92, sh=0.9):
    o = []
    for f in frames:
        x = desat_dark(f.copy(), 80)
        x = directional_vlp(x, K, lam)
        o.append(sharpen_h(x, sh, horizontal=True))
    return o


def flicker_fix(frames, ref_n=6):
    """用多帧行均值中位数做参考（条纹相位随机 -> 中位数近似无条纹），
    估计每帧逐行增益并校正。只改行方向增益，不动场景结构。"""
    rms = np.array([[lum(f).mean(axis=1) for f in frames]])[0]      # (n,H)
    ref = np.median(rms, axis=0)                                     # (H,)
    ref = np.maximum(ref, 1.0)
    out = []
    for f, rm in zip(frames, rms):
        g = rm / ref                                                 # 逐行增益
        g = np.clip(g, 0.6, 1.6)
        out.append(np.clip(f / g[:, None, None], 0, 255))
    return out


def row_norm4(frames, s=0.85):
    """逐帧：行均值 4 行高通残差当作条纹，按行做乘性校正。"""
    out = []
    for f in frames:
        rm = lum(f).mean(axis=1)
        hp = rm - mavg(rm, 4)
        g = np.clip(1 - s * hp / np.maximum(rm, 1.0), 0.5, 2.0)
        out.append(np.clip(f * g[:, None, None], 0, 255))
    return out


k = len(D) // 2
print('%-30s %8s %8s %7s' % ('方案', 'band4', 'flat8', 'sharp'))
print('%-30s %8.2f %8.3f %7.2f' % ('原始', band4(D[k]), flat8(D[k]), sharp(D[k])))

E = ema(D, 0.85, guard=False); O = post(E)
print('%-30s %8.2f %8.3f %7.2f' % ('B 强EMA(无守卫)', band4(O[k]), flat8(O[k]), sharp(O[k])))

E = ema(D, 0.85, guard=True); O = post(E)
print('%-30s %8.2f %8.3f %7.2f' % ('C 当前(相对守卫)', band4(O[k]), flat8(O[k]), sharp(O[k])))

F = flicker_fix(D)
E = ema(F, 0.85, guard=False); O = post(E)
print('%-30s %8.2f %8.3f %7.2f' % ('D 行增益校正+强EMA', band4(O[k]), flat8(O[k]), sharp(O[k])))

F = row_norm4(D)
E = ema(F, 0.85, guard=False); O = post(E)
print('%-30s %8.2f %8.3f %7.2f' % ('E 逐帧4行归一+强EMA', band4(O[k]), flat8(O[k]), sharp(O[k])))

F = flicker_fix(D)
O = post(F)
print('%-30s %8.2f %8.3f %7.2f' % ('F 仅行增益校正(不平均)', band4(O[k]), flat8(O[k]), sharp(O[k])))

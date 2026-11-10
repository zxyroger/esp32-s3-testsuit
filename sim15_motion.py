# -*- coding: utf-8 -*-
"""局部运动拖影测试：全局 mad 基本不变（静止场景里只有一小块在动）时，
提高 EMA alpha 会不会在运动区留下重影。对比旧参数与几档 alpha。
"""
import glob
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, '.')
from sim14_lowlight import ema_seq, desat_dark, directional_vlp, sharpen_h, Meter, lum

FR = sorted(glob.glob('frames_1405/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape
BX, BY, BS = W // 2 - 60, H // 2 - 60, 120
rng = np.random.default_rng(7)

# 构造序列：0..3 静止；4..7 中间方块换成另一帧的内容（局部运动）；8..11 静止
base = D[0].copy()
other = D[-1].copy()
seq = []
for i in range(12):
    f = base + rng.normal(0, 0.8, base.shape)
    if 4 <= i <= 7:
        patch = other[BY:BY + BS, BX:BX + BS].copy()
        # 每帧再把方块平移一点，模拟物体移动
        sh = (i - 4) * 3
        patch = np.roll(patch, sh, axis=1)
        f[BY:BY + BS, BX:BX + BS] = patch
    seq.append(np.clip(f, 0, 255).astype(np.float32))

M = Meter(base)


def pipe(frames, alpha, m0, vlpK=4, lam=0.9, sharp=1.0):
    outs = []
    acc = None
    for d in frames:
        if acc is None:
            acc = d.copy()
        else:
            diff = np.abs(d - acc).mean(axis=2)
            mad = float(diff.mean())
            a = 0.30 if mad > 16 else alpha
            guard = float(np.clip((mad - m0) / 3, 0, 1))
            if guard > 0:
                p = np.pad(diff, ((0, 0), (1, 1)), mode='edge')
                tmp = (p[:, :-2] + p[:, 1:-1] + p[:, 2:]) / 3
                q = np.pad(tmp, ((1, 1), (0, 0)), mode='edge')
                loc = (q[:-2, :] + q[1:-1, :] + q[2:, :]) / 3
                wgt = np.clip(1 - guard * loc / 10.0, 0, 1)
                ae = a * wgt
            else:
                ae = np.full(diff.shape, a, np.float32)
            cur = acc * ae[..., None] + d * (1 - ae[..., None])
            acc = cur.copy()
        o = desat_dark(acc.copy(), 80)
        o = directional_vlp(o, vlpK, lam)
        o = sharpen_h(o, sharp, horizontal=True)
        outs.append(o)
    return outs


def ghost(out, truth, k):
    """运动方块内输出与真值的 RMSE；越大说明拖影越重。"""
    a = out[k][BY:BY + BS, BX:BX + BS]
    b = truth[k][BY:BY + BS, BX:BX + BS]
    return float(np.sqrt(((a - b) ** 2).mean()))


print('局部运动拖影测试  120x120 方块在帧 4..7 移动')
print('%-28s %9s %9s %9s %8s' % ('参数', '运动首帧', '运动末帧', '恢复帧', '平坦横纹'))
configs = [
    ('旧 alpha0.55 m0=4 K2', 0.55, 4, 2, 0.8, 0.8),
    ('上一版 alpha0.80 m0=7 K2', 0.80, 7, 2, 0.8, 0.8),
    ('定案 alpha0.85 m0=7 K4', 0.85, 7, 4, 0.9, 1.0),
    ('暗光档 alpha0.90 m0=7 K4', 0.90, 7, 4, 1.0, 0.8),
    ('全激进 alpha0.92 m0=4 K4', 0.92, 4, 4, 1.0, 1.2),
    ('无 EMA 参照', 0.0, 7, 4, 0.9, 1.0),
]
for name, a, m0, K, lam, sh in configs:
    out = pipe(seq, a, m0, K, lam, sh)
    print('%-28s %9.2f %9.2f %9.2f %8.3f' %
          (name, ghost(out, seq, 4), ghost(out, seq, 7), ghost(out, seq, 10),
           M.band(out[6])))

print()
print('说明：运动首帧 RMSE 越小=跟得越快；恢复帧 RMSE 越小=残留重影越少。')
print('      平坦横纹为静止区域指标，越低越干净。')

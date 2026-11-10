# -*- coding: utf-8 -*-
"""sim25: 区分入夜画面的「色偏」与「彩色噪点」，并测色度低通的收益。

色偏 = 整幅 Cb/Cr 均值偏离 128；彩色噪点 = 暗区 Cb/Cr 的高频 std。
两者处置方式不同：色偏要整体色度平移（不损细节），噪点要局部低通。
"""
import glob
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SETS = [
    ("白天SVGA", "frames_1405/cam_*.jpg"),
    ("17:20", "frames_dusk_1720/*.jpg"),
    ("17:40", "frames_dusk_1740/*.jpg"),
    ("18:20", "frames_dusk_1820/*.jpg"),
    ("18:36", "frames_dusk_1836/*.jpg"),
]


def ycbcr(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128.0
    return y, cb, cr


def chroma_lp(cb, cr, k=2):
    """3x3 盒式低通（色度专用），只降彩色噪点。"""
    def box(x):
        p = np.pad(x, k, mode="edge")
        acc = np.zeros_like(x)
        for dy in range(2 * k + 1):
            for dx in range(2 * k + 1):
                acc += p[dy:dy + x.shape[0], dx:dx + x.shape[1]]
        return acc / ((2 * k + 1) ** 2)
    return box(cb), box(cr)


def main():
    print("组        暗像素占比  Y均值   Cb均值 Cr均值   暗区色度std  低通后  降幅")
    for name, pat in SETS:
        fs = sorted(glob.glob(os.path.join(HERE, pat)))
        if not fs:
            continue
        rows = []
        for f in fs:
            a = np.asarray(Image.open(f).convert("RGB"), np.float32)
            y, cb, cr = ycbcr(a)
            rows.append((y, cb, cr))
        dark = np.mean([(y < 45).mean() for y, _, _ in rows])
        ym = float(np.mean([y.mean() for y, _, _ in rows]))
        cbm = float(np.mean([cb.mean() for _, cb, _ in rows]))
        crm = float(np.mean([cr.mean() for _, _, cr in rows]))
        # 暗区色度噪点：暗像素上的高频 std（减去 3x3 低通后的残差）
        s0, s1 = [], []
        for y, cb, cr in rows:
            m = y < 45
            if m.sum() < 500:
                m = y < np.percentile(y, 20)
            lb, lr = chroma_lp(cb, cr)
            res0 = np.sqrt((cb[m] - 128) ** 2 + (cr[m] - 128) ** 2).mean()
            res1 = np.sqrt((cb[m] - lb[m]) ** 2 + (cr[m] - lr[m]) ** 2).mean()
            s0.append(res0); s1.append(res1)
        a0, a1 = float(np.mean(s0)), float(np.mean(s1))
        print("%-9s %8.3f %6.1f %7.1f %6.1f %11.2f %8.2f %5.0f%%"
              % (name, dark, ym, cbm, crm, a0, a1, 100 * (1 - a1 / max(a0, 1e-6))))


if __name__ == "__main__":
    main()

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""分析摄像头帧里的横纹：逐行均值、周期（FFT）、奇偶行差异。

用法：python analyze_bands.py [目录]
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

DIR = sys.argv[1] if len(sys.argv) > 1 else r"D:\s3demo\frames"


def analyze(path):
    im = Image.open(path).convert('L')
    a = np.asarray(im, dtype=np.float32)
    h, w = a.shape
    rows = a.mean(axis=1)                    # 每行平均亮度
    d = np.diff(rows)                        # 相邻行跳变
    even = a[0::2].mean()
    odd = a[1::2].mean()

    # FFT：去掉直流，找主周期
    x = rows - rows.mean()
    spec = np.abs(np.fft.rfft(x))
    freqs = np.fft.rfftfreq(len(x))
    spec[0] = 0
    peak = np.argsort(spec)[::-1][:5]
    periods = [(round(1 / freqs[i], 2) if freqs[i] > 0 else 0, round(spec[i] / len(x), 2))
               for i in peak]

    return {
        'name': os.path.basename(path), 'size': (w, h),
        'mean': round(float(a.mean()), 1),
        'row_std': round(float(rows.std()), 2),
        'row_ptp': round(float(rows.max() - rows.min()), 1),
        'diff_abs': round(float(np.abs(d).mean()), 2),
        'even_odd': (round(float(even), 2), round(float(odd), 2),
                     round(float(even - odd), 2)),
        'periods': periods,
        'rows': rows,
    }


def plot(results, out):
    W, H = 1000, 60 + 90 * len(results)
    img = Image.new('RGB', (W, H), (12, 14, 18))
    d = ImageDraw.Draw(img)
    y0 = 40
    for r in results:
        rows = r['rows']
        lo, hi = float(rows.min()), float(rows.max())
        span = max(1.0, hi - lo)
        n = len(rows)
        pts = []
        for i, v in enumerate(rows):
            x = 60 + i * (W - 90) / n
            y = y0 + 80 - (v - lo) / span * 74
            pts.append((x, y))
        d.line(pts, fill=(90, 170, 255), width=1)
        d.text((8, y0 + 30), r['name'][:10], fill=(190, 200, 210))
        d.text((8, y0 + 46), 'std %.1f' % r['row_std'], fill=(140, 150, 160))
        d.text((8, y0 + 60), 'e-o %.1f' % r['even_odd'][2], fill=(140, 150, 160))
        y0 += 90
    img.save(out)
    print('曲线图:', out)


def main():
    files = sorted(glob.glob(os.path.join(DIR, 'cam_*.jpg')))
    if not files:
        sys.exit('没有帧文件')
    results = []
    for f in files:
        r = analyze(f)
        results.append(r)
        print('%-10s %sx%s mean=%5.1f  行std=%5.2f  行ptp=%6.1f  邻行|diff|=%4.2f  '
              '偶-奇=%6.2f  主周期=%s'
              % (r['name'], r['size'][0], r['size'][1], r['mean'], r['row_std'],
                 r['row_ptp'], r['diff_abs'], r['even_odd'][2],
                 ', '.join('%s行(%.2f)' % p for p in r['periods'][:3])))
    plot(results, os.path.join(DIR, 'row_profile.png'))


if __name__ == '__main__':
    main()

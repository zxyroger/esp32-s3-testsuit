"""扫描 PCLK 分频，找 SVGA 下不撕裂的最小值（分频越小帧率越高）。"""
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def sm(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


def metrics(d):
    L = lum(d)
    rows = L.mean(axis=1)
    cb = 128 - 0.169 * d[..., 0] - 0.331 * d[..., 1] + 0.5 * d[..., 2]
    cr = 128 + 0.5 * d[..., 0] - 0.419 * d[..., 1] - 0.081 * d[..., 2]
    cb_hp = cb.mean(axis=1) - sm(cb.mean(axis=1), 8)
    cr_hp = cr.mean(axis=1) - sm(cr.mean(axis=1), 8)
    jump = np.abs(np.diff(rows))
    return (float((rows - sm(rows, 8)).std()),
            float(cb_hp.std() + cr_hp.std()),
            float(np.percentile(jump, 95)),
            float(L.mean()))


DIVS = [int(x) for x in sys.argv[1:]] or [8, 12, 16, 24]
print('%-6s %-8s %10s %10s %10s %8s' % ('分频', '帧数', '亮度横纹', '色度横纹', '行跳变p95', '体积KB'))
for div in DIVS:
    if div != 8:
        r = subprocess.run([sys.executable, 'serial_flow.py', 'cam pclk %d' % div],
                           cwd=HERE, capture_output=True, text=True, timeout=120)
        ok = '已存 NVS' in r.stdout
        print('  -> cam pclk %d : %s' % (div, 'OK' if ok else r.stdout[-200:]))
    out = subprocess.run([sys.executable, 'grab_frame.py', '5', 'frames_pclk%d' % div],
                         cwd=HERE, capture_output=True, text=True, timeout=120)
    fs = sorted(glob.glob(os.path.join(HERE, 'frames_pclk%d' % div, 'cam_*.jpg')))
    if not fs:
        print('%-6d %-8s %s' % (div, '无帧', out.stdout[-200:]))
        continue
    ds = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in fs[2:]]
    m = np.mean([metrics(d) for d in ds], axis=0)
    kb = np.mean([os.path.getsize(f) for f in fs]) / 1024
    print('%-6d %-8d %10.2f %10.2f %10.2f %8.1f' % (div, len(fs), m[0], m[1], m[2], kb))

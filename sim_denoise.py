# -*- coding: utf-8 -*-
"""离线仿真前端降噪管线，对比「现方案」vs「新方案(固定行profile)」的横纹抑制效果。

用 grab_frame 抓到的连续帧，逐帧跑管线，量输出图的细横纹（行profile高通残差）std。
"""
import glob
import os

import numpy as np
from PIL import Image

DIR = r"D:\s3demo\frames_day"
FILES = sorted(glob.glob(os.path.join(DIR, "cam_*.jpg")))


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def smooth_rows(rm, k):
    h = len(rm)
    out = np.empty(h)
    for y in range(h):
        out[y] = rm[max(0, y - k):min(h, y + k + 1)].mean()
    return out


def row_normalize(d):
    h, w = d.shape[:2]
    rm = lum(d).mean(axis=1)
    sm = smooth_rows(rm, 6)
    sc = sm / np.maximum(rm, 1.0)
    sc = np.clip(sc, 0.3, 3.0)
    return d * sc[:, None, None]


def row_offset_subtract(d):
    h, w = d.shape[:2]
    L = lum(d)
    lo = np.percentile(L, 8, axis=1)
    sm = smooth_rows(lo, 12)
    off = lo - sm
    wgt = np.where(L < 90, 1 - L / 90, 0)
    return d - (off[:, None, None] * wgt[:, :, None])


def desat(d, thresh=80):
    L = lum(d)
    t = np.where(L < thresh, L / thresh, 1.0)[:, :, None]
    g = d.mean(axis=2, keepdims=True)
    return g + (d - g) * t


def banding_metric(d, k=10):
    """行profile高通残差std，分暗/中/亮三段。返回 (总体std, 暗std, 亮std)。"""
    L = lum(d)
    rows = L.mean(axis=1)
    lp = smooth_rows(rows, k)
    hp = rows - lp
    dark = hp[lp < 90]
    bright = hp[lp > 150]
    return float(hp.std()), float(dark.std()) if dark.size else 0.0, \
           float(bright.std()) if bright.size else 0.0


def metric_regions(d, k=10):
    """按像素亮度分段，量「行内扣除该行均值后的残差」std —— 更贴横纹观感。"""
    L = lum(d)
    rows = L.mean(axis=1)
    lp = smooth_rows(rows, k)
    hp = rows - lp
    dark = hp[lp < 90]
    mid = hp[(lp >= 90) & (lp <= 150)]
    bright = hp[lp > 150]
    return (float(hp.std()),
            float(dark.std()) if dark.size else 0.0,
            float(mid.std()) if mid.size else 0.0,
            float(bright.std()) if bright.size else 0.0)


def run_old(frames, alpha=0.55):
    acc = None
    out_metrics = []
    for d in frames:
        d = d.astype(np.float32)
        if acc is not None:
            d = acc * alpha + d * (1 - alpha)
        acc = d.copy()
        d = row_normalize(d)
        d = row_offset_subtract(d)
        d = desat(d, 80)
        out_metrics.append(metric_regions(d))
    return out_metrics


def run_new(frames, alpha=0.55, ffc_alpha=0.08):
    acc = None
    ffc = None
    out_metrics = []
    for d in frames:
        d = d.astype(np.float32)
        if acc is not None:
            d = acc * alpha + d * (1 - alpha)
        acc = d.copy()

        # 1) 估计本帧细横纹profile（行均值高通残差）
        h = d.shape[0]
        rm = lum(d).mean(axis=1)
        lp = smooth_rows(rm, 10)
        resid = rm - lp

        # 2) 更新固定profile（慢EMA，跨帧收敛到固定FPN）
        if ffc is None:
            ffc = resid.copy()
        else:
            ffc = ffc * (1 - ffc_alpha) + resid * ffc_alpha

        # 3) 全亮度范围减固定行偏移
        d = d - ffc[:, None, None]

        # 4) 残余乘性 + 暗部加性 + 去饱和
        d = row_normalize(d)
        d = row_offset_subtract(d)
        d = desat(d, 80)
        out_metrics.append(metric_regions(d))
    return out_metrics


def main():
    frames = [np.asarray(Image.open(f).convert("RGB"), dtype=np.float32) for f in FILES]
    print("帧数:", len(frames), "尺寸:", frames[0].shape)

    # 原始（无降噪）基线
    raw = [metric_regions(f) for f in frames]
    old = run_old(frames)
    new = run_new(frames)

    def avg(m, i):
        return sum(x[i] for x in m) / len(m)

    print("\n%-8s %8s %8s %8s %8s" % ("方案", "总体std", "暗std", "中std", "亮std"))
    for name, m in [("原始", raw), ("现方案", old), ("新方案", new)]:
        print("%-8s %8.2f %8.2f %8.2f %8.2f"
              % (name, avg(m, 0), avg(m, 1), avg(m, 2), avg(m, 3)))

    # 输出新方案最后一帧对比图
    d = frames[-1].astype(np.float32)
    acc = None
    ffc = None
    for i in range(len(frames)):
        cur = frames[i].astype(np.float32)
        if acc is not None:
            cur = acc * 0.55 + cur * 0.45
        acc = cur.copy()
        rm = lum(cur).mean(axis=1)
        lp = smooth_rows(rm, 10)
        resid = rm - lp
        ffc = resid.copy() if ffc is None else ffc * 0.92 + resid * 0.08
    out = acc - ffc[:, None, None]
    out = row_normalize(out)
    out = row_offset_subtract(out)
    out = desat(out, 80)
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(
        os.path.join(DIR, "sim_new.png"))
    print("\n新方案末帧已存:", os.path.join(DIR, "sim_new.png"))


if __name__ == "__main__":
    main()

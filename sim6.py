"""
验证两种此前没试过的横纹抑制思路：
  A) 中位数垂直梯度积分：用「逐像素相邻行差在 x 方向取中位数」估计行偏置，
     对场景水平边缘鲁棒，因此不需要「行均值-低通」那个折中窗口，
     理论上能覆盖任意周期的横纹（包括 30~100 行的宽纹）。
  B) 垂直方向轻量低通：针对「非整行均匀」的横纹成分，
     只在 y 方向混合，几乎不损失水平方向清晰度。
"""
import glob
import numpy as np
from PIL import Image

FRAMES = sorted(glob.glob('frames_1123/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in FRAMES]
H, W, _ = D[0].shape


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def smooth_rows(rm, k):
    h = len(rm)
    out = np.empty(h)
    for y in range(h):
        out[y] = rm[max(0, y - k):min(h, y + k + 1)].mean()
    return out


KS = [5, 10, 20, 40, 80]


def band(d):
    rows = lum(d).mean(axis=1)
    return [float((rows - smooth_rows(rows, k)).std()) for k in KS]


def show(name, res):
    print('%-18s %s' % (name, '  '.join('K%-3d=%5.2f' % (k, v) for k, v in zip(KS, res))))


def ema_frames(frames, a=0.55):
    acc = None
    out = []
    for d in frames:
        d = d.copy()
        if acc is not None:
            d = acc * a + d * (1 - a)
        acc = d.copy()
        out.append(d)
    return out


def mean_profile(frames, K=25):
    """现有做法：行均值 - K 宽低通，跨帧平均"""
    offs = np.zeros((3, H))
    for d in frames:
        for c in range(3):
            rm = d[..., c].mean(axis=1)
            offs[c] += rm - smooth_rows(rm, K)
    return offs / len(frames)


def median_profile(d):
    """新做法：相邻行差在 x 上取中位数 → 积分 → 去线性趋势"""
    profs = np.zeros((3, H))
    for c in range(3):
        A = d[..., c]
        g = np.median(A[1:, :] - A[:-1, :], axis=1)  # len H-1
        p = np.concatenate([[0.0], np.cumsum(g)])
        y = np.arange(H)
        p -= np.polyval(np.polyfit(y, p, 1), y)       # 去掉场景整体亮度渐变
        profs[c] = p
    return profs


def apply(d, prof, a=1.0):
    out = d.copy()
    for c in range(3):
        out[..., c] = np.clip(out[..., c] - a * prof[c][:, None], 0, 255)
    return out


def apply_vlow(d, w=0.25):
    """y 方向 3 抽头 [w, 1-2w, w]，x 方向完全不动"""
    out = d.copy()
    out[1:-1, :, :] = (1 - 2 * w) * d[1:-1, :, :] + w * d[:-2, :, :] + w * d[2:, :, :]
    return out


# ---- 基线 ----
show('原始', np.mean([band(d) for d in D], axis=0))

E = ema_frames(D)
show('仅时域EMA', np.mean([band(d) for d in E], axis=0))

# ---- 现有 flat-field（行均值 K25，跨帧平均）----
pm = mean_profile(E, 25)
show('现有FF K25', np.mean([band(apply(d, pm)) for d in E], axis=0))

# ---- 新算法 A：中位数梯度，逐帧 ----
for a in [0.6, 0.8, 1.0]:
    r = []
    for d in E:
        r.append(band(apply(d, median_profile(d), a)))
    show('中位梯度 a=%.1f' % a, np.mean(r, axis=0))

# ---- 新算法 A + 跨帧平均（固定成分）----
mp = np.mean([median_profile(d) for d in E], axis=0)
for a in [0.8, 1.0]:
    show('中位梯度跨帧 a=%.1f' % a,
         np.mean([band(apply(d, mp, a)) for d in E], axis=0))

# ---- 两种 profile 叠加 ----
comb = pm + mp * 0.5
show('均值K25+中位0.5', np.mean([band(apply(d, comb)) for d in E], axis=0))

# ---- 新算法 B：垂直轻低通（叠在现有 FF 之后）----
base = [apply(d, pm) for d in E]
for w in [0.15, 0.25, 0.35]:
    show('FF+垂直低通 w=%.2f' % w,
         np.mean([band(apply_vlow(d, w)) for d in base], axis=0))

# ---- 全部叠加 ----
final = [apply_vlow(apply(d, comb), 0.25) for d in E]
show('组合方案', np.mean([band(d) for d in final], axis=0))

# ---- 出图供肉眼检查伪影 ----
idx = len(D) // 2
Image.fromarray(D[idx].astype(np.uint8)).save('sim6_raw.png')
Image.fromarray(base[idx].astype(np.uint8)).save('sim6_cur.png')
Image.fromarray(final[idx].astype(np.uint8)).save('sim6_new.png')
print('\n已生成 sim6_raw.png / sim6_cur.png / sim6_new.png')

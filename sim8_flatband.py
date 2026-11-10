"""
用「平坦区横纹」指标（排除场景水平边缘）评估固定 profile 的低通窗口 K，
并生成 K25 / K50 / K80 的对比图，检查加宽窗口是否引入带状伪影。
"""
import glob
import numpy as np
from PIL import Image

FR = sorted(glob.glob('frames_1123/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def sm(r, k):
    n = len(r)
    o = np.empty(n)
    for y in range(n):
        o[y] = r[max(0, y - k):min(n, y + k + 1)].mean()
    return o


def ema(frames, a=0.55):
    acc = None
    out = []
    for d in frames:
        d = d.copy()
        if acc is not None:
            d = acc * a + d * (1 - a)
        acc = d.copy()
        out.append(d)
    return out


def ff_profile(frames, K):
    p = np.zeros((3, H))
    for d in frames:
        for c in range(3):
            rm = d[..., c].mean(axis=1)
            p[c] += rm - sm(rm, K)
    return p / len(frames)


def apply(d, p):
    o = d.copy()
    for c in range(3):
        o[..., c] = np.clip(o[..., c] - p[c][:, None], 0, 255)
    return o


# 平坦块：8x6 个 80x80 块中，取亮度 std 最小的 10 块（天空/墙面等无结构区）
L0 = lum(D[len(D) // 2])
BW, BH = 80, 80
blocks = []
for by in range(0, H - BH + 1, BH):
    for bx in range(0, W - BW + 1, BW):
        s = L0[by:by + BH, bx:bx + BW].std()
        blocks.append((s, by, bx))
blocks.sort()
FLAT = [(by, bx) for _, by, bx in blocks[:10]]


def flat_band(d):
    """平坦块内的行高通残差 std —— 只看无场景结构的区域，才是真横纹"""
    L = lum(d)
    hp = []
    for by, bx in FLAT:
        rows = L[by:by + BH, bx:bx + BW].mean(axis=1)
        lp = sm(rows, 8)
        hp.extend((rows - lp).tolist())
    return float(np.std(hp))


def full_band(d, ks=(5, 10, 20, 40, 80)):
    rows = lum(d).mean(axis=1)
    return [float((rows - sm(rows, k)).std()) for k in ks]


E = ema(D)
print('平坦块:', [(by, bx) for by, bx in FLAT])
print('%-14s %8s %8s %8s %8s %8s %8s' % ('方案', '平坦横纹', 'K5', 'K10', 'K20', 'K40', 'K80'))
print('%-14s %8.2f %s' % ('原始', np.mean([flat_band(d) for d in E]),
                          '  '.join('%8.2f' % v for v in np.mean([full_band(d) for d in E], axis=0))))

outs = {}
for K in [10, 25, 40, 60, 80]:
    p = ff_profile(E, K)
    res = [apply(d, p) for d in E]
    outs[K] = res
    fb = np.mean([flat_band(d) for d in res])
    print('%-14s %8.2f %s' % ('固定profile K%d' % K, fb,
                              '  '.join('%8.2f' % v for v in np.mean([full_band(d) for d in res], axis=0))))

# 两级：K60 的宽 profile + 逐帧 K6 细纹（模拟现有 rowNormalize 之后的效果）
p60 = ff_profile(E, 60)
for w in [0.7, 1.0]:
    res = []
    for d in E:
        dd = apply(d, p60 * w)
        # 逐帧细纹归一化（乘性近似）——只做很轻的一次
        rows = lum(dd).mean(axis=1)
        g = sm(rows, 6) / np.maximum(rows, 1)
        g = np.clip(g, 0.9, 1.1)
        res.append(np.clip(dd * g[:, None, None], 0, 255))
    print('%-14s %8.2f %s' % ('K60*%.1f+细纹' % w, np.mean([flat_band(d) for d in res]),
                              '  '.join('%8.2f' % v for v in np.mean([full_band(d) for d in res], axis=0))))

# 出图：原始 / K25(现状) / K60 / K80
idx = len(E) // 2
CW = 420
CH = int(H * CW / W)
canvas = Image.new('RGB', (CW * 2 + 16, CH * 2 + 16), (35, 35, 35))
for i, (name, d) in enumerate([('原始', E[idx]), ('K25 现状', outs[25][idx]),
                               ('K60', outs[60][idx]), ('K80', outs[80][idx])]):
    t = Image.fromarray(d.astype(np.uint8)).resize((CW, CH), Image.Resampling.LANCZOS)
    canvas.paste(t, ((i % 2) * (CW + 16), (i // 2) * (CH + 16)))
canvas.save('sim8_flat.png')
print('\n已生成 sim8_flat.png：左上原始 右上K25 左下K60 右下K80')

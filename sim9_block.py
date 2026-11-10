"""
根因：现有 flat-field 用「整幅画面的行均值」估固定偏置，楼房窗户的水平横排
结构被当成固定横纹烤进 profile，再整行减到天空上 → 平坦区凭空出现水平暗带。

新思路：把画面按列分段，每段独立估计自己的行 profile，段间线性插值。
这样天空段的 profile 只来自天空，不会被楼房结构污染；也不再假设
「横纹整行均匀」（实测 4 竖条 profile 相关性只有 0.02~0.63，甚至负相关）。
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


def full_profile(frames, K=25):
    p = np.zeros((3, H))
    for d in frames:
        for c in range(3):
            rm = d[..., c].mean(axis=1)
            p[c] += rm - sm(rm, K)
    return p / len(frames)


def apply_full(d, p, s=1.0):
    o = d.copy()
    for c in range(3):
        o[..., c] = np.clip(o[..., c] - s * p[c][:, None], 0, 255)
    return o


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
    o = d.copy()
    # 每段中心之间的 profile 做线性插值，避免竖直接缝
    xs = np.arange(W, dtype=np.float32)
    t = np.clip(xs / sw - 0.5, 0, nseg - 1)
    s0 = np.floor(t).astype(int)
    s1 = np.minimum(s0 + 1, nseg - 1)
    f = (t - s0)[None, :]                       # (1,W)
    for c in range(3):
        a = P[:, c, :].T                          # (H,nseg)
        px = a[:, s0] * (1 - f) + a[:, s1] * f    # (H,W)
        o[..., c] = np.clip(o[..., c] - strength * px, 0, 255)
    return o


def rownorm(d, K=6, s=1.0):
    """逐帧细纹归一化（现有 rowNormalize 的加性近似）"""
    rows = lum(d).mean(axis=1)
    off = s * (rows - sm(rows, K))
    return np.clip(d - off[:, None, None], 0, 255)


# 平坦块指标
L0 = lum(D[len(D) // 2])
BW = BH = 80
blocks = sorted((L0[by:by + BH, bx:bx + BW].std(), by, bx)
                for by in range(0, H - BH + 1, BH)
                for bx in range(0, W - BW + 1, BW))
FLAT = [(by, bx) for _, by, bx in blocks[:10]]


def flat_band(d):
    L = lum(d)
    hp = []
    for by, bx in FLAT:
        rows = L[by:by + BH, bx:bx + BW].mean(axis=1)
        hp.extend((rows - sm(rows, 8)).tolist())
    return float(np.std(hp))


def full_band(d):
    rows = lum(d).mean(axis=1)
    return float((rows - sm(rows, 10)).std())


E = ema(D)
pf = full_profile(E, 25)
pb, sw = block_profile(E, 25, 8)
pb12, _ = block_profile(E, 12, 8)

cases = {
    '只EMA': E,
    '只EMA+细纹': [rownorm(d, 6, 0.7) for d in E],
    '全幅K25(现状)': [apply_full(d, pf) for d in E],
    '全幅K25 x0.4': [apply_full(d, pf, 0.4) for d in E],
    '分块K25': [apply_block(d, pb, sw) for d in E],
    '分块K25 x0.6': [apply_block(d, pb, sw, 0.6) for d in E],
    '分块K12': [apply_block(d, pb12, sw) for d in E],
    '分块K12 x0.6': [apply_block(d, pb12, sw, 0.6) for d in E],
    '分块K12+细纹': [rownorm(apply_block(d, pb12, sw, 0.7), 6, 0.6) for d in E],
}

print('%-18s %10s %10s' % ('方案', '平坦横纹', '整幅K10'))
print('%-18s %10.2f %10.2f' % ('原始', np.mean([flat_band(d) for d in D]),
                               np.mean([full_band(d) for d in D])))
for name, ds in cases.items():
    print('%-18s %10.2f %10.2f' % (name, np.mean([flat_band(d) for d in ds]),
                                    np.mean([full_band(d) for d in ds])))

idx = len(E) // 2
CW = 430
CH = int(H * CW / W)
canvas = Image.new('RGB', (CW * 2 + 16, CH * 2 + 16), (35, 35, 35))
picks = [('原始', E[idx]), ('全幅K25 现状', cases['全幅K25(现状)'][idx]),
         ('分块K12', cases['分块K12'][idx]), ('分块K12+细纹', cases['分块K12+细纹'][idx])]
for i, (name, d) in enumerate(picks):
    t = Image.fromarray(d.astype(np.uint8)).resize((CW, CH), Image.Resampling.LANCZOS)
    canvas.paste(t, ((i % 2) * (CW + 16), (i // 2) * (CH + 16)))
canvas.save('sim9_block.png')
print('\n已生成 sim9_block.png：左上原始 右上现状 左下分块K12 右下分块K12+细纹')

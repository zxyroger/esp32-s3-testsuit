"""精确复刻前端三个逐帧步骤，决定分块 profile 之后应该保留哪些。"""
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


def apply_block(d, P, sw):
    nseg = P.shape[0]
    xs = np.arange(W, dtype=np.float32)
    t = np.clip(xs / sw - 0.5, 0, nseg - 1)
    s0 = np.floor(t).astype(int)
    s1 = np.minimum(s0 + 1, nseg - 1)
    f = (t - s0)[None, :]
    o = d.copy()
    for c in range(3):
        a = P[:, c, :].T
        o[..., c] = np.clip(o[..., c] - (a[:, s0] * (1 - f) + a[:, s1] * f), 0, 255)
    return o


def row_normalize(d, s=1.0):
    """前端 rowNormalize：整幅行均值 − K6 低通 → 乘性增益"""
    rm = lum(d).mean(axis=1)
    g = np.clip(sm(rm, 6) / np.maximum(rm, 1), 0.3, 3.0)
    g = 1 + (g - 1) * s
    return np.clip(d * g[:, None, None], 0, 255)


def row_offset(d, s=1.0):
    """前端 rowOffsetSubtract：每行第8分位 − K12 低通，只在暗部(L<90)减"""
    L = lum(d)
    lo = np.percentile(L, 8, axis=1)
    off = lo - sm(lo, 12)
    wgt = np.clip(1 - L / 90, 0, 1)
    return np.clip(d - s * (off[:, None] * wgt)[..., None], 0, 255)


def desat_dark(d, thresh=80):
    L = lum(d)[..., None]
    t = np.clip(L / thresh, 0, 1)
    g = d.mean(axis=2, keepdims=True)
    return g + (d - g) * t


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
    r = lum(d).mean(axis=1)
    return float((r - sm(r, 10)).std())


E = ema(D)
P, sw = block_profile(E, 25, 8)
B = [apply_block(d, P, sw) for d in E]

cases = {
    '只EMA': E,
    '分块K25 单独': B,
    '分块+乘性归一': [row_normalize(d) for d in B],
    '分块+加性暗部': [row_offset(d) for d in B],
    '分块+乘性+加性': [row_offset(row_normalize(d)) for d in B],
    '分块+乘性0.3+加性': [row_offset(row_normalize(d, 0.3)) for d in B],
    '完整(含去饱和)': [desat_dark(row_offset(row_normalize(d))) for d in B],
}
print('%-22s %10s %10s' % ('方案', '平坦横纹', '整幅K10'))
for n, ds in cases.items():
    print('%-22s %10.2f %10.2f' % (n, np.mean([flat_band(d) for d in ds]),
                                   np.mean([full_band(d) for d in ds])))

# profile 用多少帧建立才够（分块后每段只有80像素，噪声更大）
print('\n建立 profile 用不同帧数（对第6帧应用）：')
for n in [1, 2, 3, 4, 6]:
    Pn, swn = block_profile(E[:n], 25, 8)
    dn = apply_block(E[-1], Pn, swn)
    print('  %d 帧: 平坦横纹=%.2f 整幅K10=%.2f' % (n, flat_band(dn), full_band(dn)))

idx = len(E) // 2
CW = 430
CH = int(H * CW / W)
canvas = Image.new('RGB', (CW * 2 + 16, CH * 2 + 16), (35, 35, 35))
picks = [('原始', E[idx]), ('现状全幅K25', None),
         ('分块K25单独', cases['分块K25 单独'][idx]),
         ('分块+加性暗部', cases['分块+加性暗部'][idx])]
# 现状：全幅K25 profile
pf = np.zeros((3, H))
for d in E:
    for c in range(3):
        rm = d[..., c].mean(axis=1)
        pf[c] += rm - sm(rm, 25)
pf /= len(E)
cur = E[idx].copy()
for c in range(3):
    cur[..., c] = np.clip(cur[..., c] - pf[c][:, None], 0, 255)
picks[1] = ('现状全幅K25', cur)
for i, (name, d) in enumerate(picks):
    t = Image.fromarray(d.astype(np.uint8)).resize((CW, CH), Image.Resampling.LANCZOS)
    canvas.paste(t, ((i % 2) * (CW + 16), (i // 2) * (CH + 16)))
canvas.save('sim10_pipeline.png')
print('\n已生成 sim10_pipeline.png')

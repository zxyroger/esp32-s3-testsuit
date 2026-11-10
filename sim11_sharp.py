"""诊断：分块 flat-field 之后，时域 EMA 的锐度代价是否还值得。"""
import glob
import numpy as np
from PIL import Image

FR = sorted(glob.glob('frames_1152/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape


def lum(d):
    return 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]


def sm(r, k):
    n = len(r)
    return np.array([r[max(0, y - k):min(n, y + k + 1)].mean() for y in range(n)])


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


def row_normalize(d, s=0.3):
    rm = lum(d).mean(axis=1)
    g = np.clip(sm(rm, 6) / np.maximum(rm, 1), 0.3, 3.0)
    return np.clip(d * (1 + (g - 1) * s)[:, None, None], 0, 255)


def row_offset(d):
    L = lum(d)
    lo = np.percentile(L, 8, axis=1)
    off = lo - sm(lo, 12)
    wgt = np.clip(1 - L / 90, 0, 1)
    return np.clip(d - (off[:, None] * wgt)[..., None], 0, 255)


def desat_dark(d, thresh=80):
    L = lum(d)[..., None]
    t = np.clip(L / thresh, 0, 1)
    g = d.mean(axis=2, keepdims=True)
    return g + (d - g) * t


# ---------- 纹理区 / 平坦区定位（用原始帧，全脚本共用同一套块） ----------
L0 = lum(D[len(D) // 2])
BW = BH = 80
blocks = sorted((L0[by:by + BH, bx:bx + BW].std(), by, bx)
                for by in range(0, H - BH + 1, BH)
                for bx in range(0, W - BW + 1, BW))
FLAT = [(by, bx) for _, by, bx in blocks[:10]]          # 最平坦 10 块
TEX = [(by, bx) for _, by, bx in blocks[-10:]]         # 纹理最强 10 块


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


def sharp(d):
    """纹理区梯度能量：越高越锐利"""
    L = lum(d)
    vals = []
    for by, bx in TEX:
        p = L[by:by + BH, bx:bx + BW]
        gx = np.abs(np.diff(p, axis=1)).mean()
        gy = np.abs(np.diff(p, axis=0)).mean()
        vals.append(gx + gy)
    return float(np.mean(vals))


# ---------- 运动序列：同一帧逐帧平移，模拟微风/手持抖动 ----------
def shift(d, dx, dy=0):
    o = np.empty_like(d)
    o[:, :] = d
    o = np.roll(o, dx, axis=1)
    o = np.roll(o, dy, axis=0)
    if dx > 0:
        o[:, :dx] = d[:, :1]
    elif dx < 0:
        o[:, dx:] = d[:, -1:]
    if dy > 0:
        o[:dy, :] = d[:1, :]
    return o


MOTION = [shift(D[2], i, (i // 2)) for i in range(6)]


# ---------- EMA 变体 ----------
def ema_fixed(frames, a):
    acc = None
    out = []
    for d in frames:
        if acc is not None:
            d = acc * a + d * (1 - a)
        acc = d.copy()
        out.append(d)
    return out


def ema_adaptive(frames, a=0.55, K=14):
    """逐像素运动自适应：与上一帧差异大 => 认为在动，保留当前帧，不拖影"""
    acc = None
    out = []
    for d in frames:
        d = d.copy()
        if acc is not None:
            diff = np.abs(lum(d) - lum(acc))[..., None]
            # 3x3 局部平均，抑制单点噪声把 w 压到 0
            p = np.pad(diff[:, :, 0], 1, mode='edge')
            loc = np.zeros_like(p)
            for dy in range(3):
                for dx in range(3):
                    loc += np.roll(np.roll(p, dy, 0), dx, 1)
            loc = loc[1:-1, 1:-1] / 9
            w = np.clip(1 - loc / K, 0, 1)[..., None]
            ae = a * w
            d = acc * ae + d * (1 - ae)
        acc = d.copy()
        out.append(d)
    return out


def box3(d):
    """3x3 box blur（分离实现，输出与输入同尺寸）"""
    p = np.pad(d, ((0, 0), (1, 1), (0, 0)), mode='edge')
    t = (p[:, :-2, :] + p[:, 1:-1, :] + p[:, 2:, :]) / 3
    q = np.pad(t, ((1, 1), (0, 0), (0, 0)), mode='edge')
    return (q[:-2, :, :] + q[1:-1, :, :] + q[2:, :, :]) / 3


def unsharp_edge(d, amount=0.45, Kg=8):
    """只在纹理/边缘区做轻度锐化；平坦天空 mask≈0，横纹不会被放大。"""
    hi = d - box3(d)
    g = local_grad(d)[..., None]
    mask = np.clip((g - 2) / Kg, 0, 1)
    return np.clip(d + amount * hi * mask, 0, 255)


def pipeline(frames, ema_fn, post=None):
    E = ema_fn(frames)
    P, sw = block_profile(E, 25, 8)
    B = [apply_block(d, P, sw) for d in E]
    O = [desat_dark(row_offset(row_normalize(d))) for d in B]
    return [post(d) for d in O] if post else O


def local_grad(d):
    """当前帧的空间梯度（3x3 平均）：平坦区≈0，纹理/边缘区大。"""
    L = lum(d)
    gx = np.abs(np.diff(L, axis=1, append=L[:, -1:]))
    gy = np.abs(np.diff(L, axis=0, append=L[-1:, :]))
    g = (gx + gy) / 2
    p = np.pad(g, 1, mode='edge')
    s = np.zeros_like(g)
    for dy in range(3):
        for dx in range(3):
            s += np.roll(np.roll(p, dy, 0), dx, 1)[1:-1, 1:-1]
    return s / 9


def local_diff(d, acc):
    """帧间差异（3x3 平均）：静止区≈噪声，运动区大。"""
    d0 = np.abs(lum(d) - lum(acc))
    p = np.pad(d0, 1, mode='edge')
    s = np.zeros_like(d0)
    for dy in range(3):
        for dx in range(3):
            s += np.roll(np.roll(p, dy, 0), dx, 1)[1:-1, 1:-1]
    return s / 9


def ema_combined(frames, a=0.55, Kg=12, Kt=14, use_spatial=True, use_temporal=True):
    """平坦静止区才做时域平均；纹理/运动区保留当前帧。"""
    acc = None
    out = []
    for d in frames:
        d = d.copy()
        if acc is not None:
            w = np.ones((H, W), np.float32)
            if use_spatial:
                w *= np.clip(1 - local_grad(d) / Kg, 0, 1)
            if use_temporal:
                w *= np.clip(1 - local_diff(d, acc) / Kt, 0, 1)
            ae = (a * w)[..., None]
            d = acc * ae + d * (1 - ae)
        acc = d.copy()
        out.append(d)
    return out


def ema_guard(frames, a=0.55, K=10, mad0=5.0, span=3.0):
    """静止帧完全保持原 EMA；检测到运动才逐渐启用局部保护。"""
    acc = None
    out = []
    for d in frames:
        d = d.copy()
        if acc is not None:
            mad = float(np.abs(d - acc).mean())
            guard = np.clip((mad - mad0) / span, 0, 1)
            if guard > 0:
                loc = local_diff(d, acc)
                w = np.clip(1 - guard * loc / K, 0, 1)
                ae = (a * w)[..., None]
                d = acc * ae + d * (1 - ae)
            else:
                d = acc * a + d * (1 - a)
        acc = d.copy()
        out.append(d)
    return out


cases = {
    '原始(无处理)':      lambda f: f,
    'EMA 0.55 (现状)':   lambda f: ema_fixed(f, 0.55),
    'EMA 0.35':          lambda f: ema_fixed(f, 0.35),
    'EMA 0.20':          lambda f: ema_fixed(f, 0.20),
    '无 EMA':            lambda f: [d.copy() for d in f],
    '自适应 EMA K14':    lambda f: ema_adaptive(f, 0.55, 14),
    '自适应 EMA K8':     lambda f: ema_adaptive(f, 0.55, 8),
    '空间梯度 Kg12':     lambda f: ema_combined(f, Kg=12, use_temporal=False),
    '空间+时域 Kg12Kt14': lambda f: ema_combined(f, Kg=12, Kt=14),
    '空间+时域 Kg8Kt20':  lambda f: ema_combined(f, Kg=8, Kt=20),
    '空间+时域 Kg20Kt20': lambda f: ema_combined(f, Kg=20, Kt=20),
    '守卫 K10(5~8)':     lambda f: ema_guard(f, K=10, mad0=5, span=3),
    '守卫 K8(5~8)':       lambda f: ema_guard(f, K=8, mad0=5, span=3),
    '守卫 K10(4~7)':      lambda f: ema_guard(f, K=10, mad0=4, span=3),
    '守卫+锐化0.30':      lambda f: ema_guard(f, K=10, mad0=4, span=3),
    '守卫+锐化0.45':      lambda f: ema_guard(f, K=10, mad0=4, span=3),
    '守卫+锐化0.60':      lambda f: ema_guard(f, K=10, mad0=4, span=3),
}
POST = {
    '守卫+锐化0.30': lambda d: unsharp_edge(d, 0.30),
    '守卫+锐化0.45': lambda d: unsharp_edge(d, 0.45),
    '守卫+锐化0.60': lambda d: unsharp_edge(d, 0.60),
}

for label, seq in [('静止场景', D), ('运动场景(逐帧平移)', MOTION)]:
    print('\n=== %s ===' % label)
    print('%-20s %10s %10s %10s %10s' % ('方案', '锐度↑', '平坦横纹↓', '整幅band↓', '拖影RMSE↓'))
    for name, fn in cases.items():
        if name == '原始(无处理)':
            out = seq
        else:
            out = pipeline(seq, fn, POST.get(name))
        last = out[-1]
        ghost = float(np.sqrt(((last - seq[-1]) ** 2).mean())) if name != '原始(无处理)' else 0.0
        print('%-20s %10.2f %10.2f %10.2f %10.2f' % (
            name, np.mean([sharp(d) for d in out[-3:]]),
            np.mean([flat_band(d) for d in out[-3:]]),
            np.mean([full_band(d) for d in out[-3:]]), ghost))

# ---------- 视觉对比图：运动场景最后一帧，裁剪纹理区放大 ----------
picks = [
    ('原始', MOTION[-1]),
    ('现状 EMA0.55', pipeline(MOTION, cases['EMA 0.55 (现状)'])[-1]),
    ('守卫(无锐化)', pipeline(MOTION, cases['守卫 K10(4~7)'])[-1]),
    ('守卫+锐化0.3', pipeline(MOTION, cases['守卫+锐化0.30'], POST['守卫+锐化0.30'])[-1]),
]
by, bx = TEX[-1]
crop_h, crop_w = 240, 320
cy, cx = min(by, H - crop_h), min(bx, W - crop_w)
CW, CH = 400, 300
canvas = Image.new('RGB', (CW * 2 + 12, CH * 2 + 12), (35, 35, 35))
for i, (name, d) in enumerate(picks):
    t = Image.fromarray(d.astype(np.uint8)).crop((cx, cy, cx + crop_w, cy + crop_h))
    t = t.resize((CW, CH), Image.Resampling.LANCZOS)
    canvas.paste(t, ((i % 2) * (CW + 12), (i // 2) * (CH + 12)))
canvas.save('sim11_sharp.png')
print('\n已生成 sim11_sharp.png (纹理区放大: 左上原始/右上现状/左下无EMA/右下自适应)')

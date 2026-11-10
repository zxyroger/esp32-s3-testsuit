"""
验证「CSS 非整数缩放造成混叠」假设：
canvas 内部 640x480，手机 CSS 显示约 492x369（0.77x）。
浏览器对 canvas 的 CSS 缩放通常只做双线性（2x2 采样），
在 >1.5x 降采样时会丢行 → 细横纹被折返成低频宽横带（摩尔纹）。
本脚本对比不同降采样算法下的横纹观感与指标。
"""
import glob
import numpy as np
from PIL import Image

FR = sorted(glob.glob('frames_1123/cam_*.jpg'))
D = [np.asarray(Image.open(f).convert('RGB'), np.float32) for f in FR]
H, W, _ = D[0].shape


def sm(rm, k):
    h = len(rm)
    o = np.empty(h)
    for y in range(h):
        o[y] = rm[max(0, y - k):min(h, y + k + 1)].mean()
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


def ff_profile(frames, K=25):
    offs = np.zeros((3, H))
    for d in frames:
        for c in range(3):
            rm = d[..., c].mean(axis=1)
            offs[c] += rm - sm(rm, K)
    return offs / len(frames)


def apply(d, p):
    o = d.copy()
    for c in range(3):
        o[..., c] = np.clip(o[..., c] - p[c][:, None], 0, 255)
    return o


def vlow(d, w=0.25):
    o = d.copy()
    o[1:-1] = (1 - 2 * w) * d[1:-1] + w * d[:-2] + w * d[2:]
    return o


def band(d, ks=(3, 5, 10, 20)):
    L = 0.299 * d[..., 0] + 0.587 * d[..., 1] + 0.114 * d[..., 2]
    rows = L.mean(axis=1)
    return [float((rows - sm(rows, k)).std()) for k in ks]


E = ema(D)
p = ff_profile(E)
clean = apply(E[len(E) // 2], p)          # 现有 flat-field 校正后的 640x480
clean_v = vlow(clean, 0.25)                # 再叠一层垂直抗混叠低通
src = Image.fromarray(clean.astype(np.uint8))
src_v = Image.fromarray(clean_v.astype(np.uint8))

DW, DH = 492, 369                         # 手机 CSS 显示尺寸（0.769x）
methods = {
    'BILINEAR(浏览器近似)': (src, Image.Resampling.BILINEAR),
    'BICUBIC': (src, Image.Resampling.BICUBIC),
    'LANCZOS(高质量)': (src, Image.Resampling.LANCZOS),
    'BOX(面积平均)': (src, Image.Resampling.BOX),
    'BILINEAR+垂直低通': (src_v, Image.Resampling.BILINEAR),
    'LANCZOS+垂直低通': (src_v, Image.Resampling.LANCZOS),
}

print('降采样到 %dx%d 后的行高通残差 std（越小越干净）' % (DW, DH))
print('%-24s %s' % ('方法', '  '.join('K%-2d=%5.2f' % (k, v) for k, v in zip((3, 5, 10, 20), [0] * 4))))
for name, (im, res) in methods.items():
    small = np.asarray(im.resize((DW, DH), res).convert('RGB'), np.float32)
    b = band(small)
    print('%-24s %s' % (name, '  '.join('K%-2d=%5.2f' % (k, v) for k, v in zip((3, 5, 10, 20), b))))

# 把降采样结果放大回 640x480（最近邻，不引入额外插值）便于肉眼对比
labels = ['BILINEAR(浏览器近似)', 'LANCZOS(高质量)', 'BILINEAR+垂直低通', 'LANCZOS+垂直低通']
tiles = []
for name in labels:
    im, res = methods[name]
    tiles.append(im.resize((DW, DH), res).resize((W, H), Image.Resampling.NEAREST))

CW = 420
CH = int(H * CW / W)
canvas = Image.new('RGB', (CW * 2 + 16, CH * 2 + 16), (35, 35, 35))
for i, (t, name) in enumerate(zip(tiles, labels)):
    canvas.paste(t.resize((CW, CH)), ((i % 2) * (CW + 16), (i // 2) * (CH + 16)))
canvas.save('sim7_display.png')
print('\n已生成 sim7_display.png')
print('左上=BILINEAR(现状)  右上=LANCZOS  左下=BILINEAR+垂直低通  右下=LANCZOS+垂直低通')

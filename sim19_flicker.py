# -*- coding: utf-8 -*-
"""入夜条纹性质诊断：区分「固定行 FPN」与「工频闪烁 banding」。
判据：
  - 固定 FPN：多帧行残差高度相关（帧间相关 > 0.8），且频谱峰稳定
  - 工频闪烁：帧间相关低、条纹位置/强度随帧漂移，空间周期对应曝光时间
同时给出色彩偏置（暗光 AWB 是否失准）。
用法: python sim19_flicker.py [目录或通配符...]
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

pats = sys.argv[1:] or ['frames_dusk_1720/*.jpg', 'frames_day2night/f_17*.jpg']
files = []
for p in pats:
    if os.path.isdir(p):
        files.extend(sorted(glob.glob(os.path.join(p, '*.jpg'))))
    else:
        files.extend(sorted(glob.glob(p)))
files = sorted(set(files))
if len(files) < 3:
    print('样本不足: %d' % len(files))
    sys.exit(0)


def load(f):
    a = np.asarray(Image.open(f).convert('RGB'), np.float32)
    L = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    return a, L


def hp_row(L, k=8):
    r = L.mean(axis=1)
    lp = np.array([r[max(0, y - k):min(len(r), y + k + 1)].mean() for y in range(len(r))])
    return r - lp


print('样本 %d 张' % len(files))
HP, LS, RGB = [], [], []
for f in files[:14]:
    a, L = load(f)
    HP.append(hp_row(L))
    LS.append(float(L.mean()))
    RGB.append([float(a[..., c].mean()) for c in range(3)])
HP = np.array(HP)
print('亮度范围 %.1f ~ %.1f' % (min(LS), max(LS)))

# 1) 帧间相关（固定 FPN 会很高）
cs = []
for i in range(len(HP) - 1):
    a, b = HP[i] - HP[i].mean(), HP[i + 1] - HP[i + 1].mean()
    d = np.sqrt((a * a).mean() * (b * b).mean())
    cs.append(float((a * b).mean() / d) if d > 0 else 0)
print('\n帧间行残差相关: 均值 %.3f  最小 %.3f  最大 %.3f' %
      (np.mean(cs), min(cs), max(cs)))
print('判读: >0.8 固定 FPN 为主；<0.4 时变（工频闪烁）为主')

# 2) 与多帧平均（“固定分量”）的相关 => 固定分量占比
ref = HP.mean(axis=0)
ref0 = ref - ref.mean()
fix_ratios = []
for i in range(len(HP)):
    a = HP[i] - HP[i].mean()
    d = np.sqrt((a * a).mean() * (ref0 * ref0).mean())
    fix_ratios.append(float((a * ref0).mean() / d) if d > 0 else 0)
print('各帧与固定分量的相关: ' + ' '.join('%.2f' % v for v in fix_ratios))

# 3) 空间频谱：找主周期
F = np.abs(np.fft.rfft(HP * np.hanning(HP.shape[1])[None, :], axis=1)).mean(axis=0)
freq = np.fft.rfftfreq(HP.shape[1])
top = np.argsort(F[2:])[::-1][:6] + 2
print('\n行残差主频（周期=行）:')
for i in sorted(top):
    if freq[i] > 0:
        print('  周期 %6.2f 行  幅度 %8.1f' % (1 / freq[i], F[i]))

# 4) 条纹能量随时间波动（闪烁的直接证据）
e = HP.std(axis=1)
print('\n每帧条纹强度: ' + ' '.join('%.2f' % v for v in e))
print('波动系数 %.2f（>0.35 说明帧间强度剧烈变化，符合工频闪烁）' % (e.std() / max(e.mean(), 1e-6)))

# 5) 色彩偏置
rgb = np.array(RGB)
m = rgb.mean(axis=1)
print('\n色彩均值 R/G/B: %.1f / %.1f / %.1f' % (m[0], m[1], m[2]))
print('R/G=%.2f  B/G=%.2f（灰世界应≈1.0；>1.15 明显偏暖/偏黄）' %
      (m[0] / m[1], m[2] / m[1]))

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
生成手机桌面图标（PWA / iOS apple-touch-icon / favicon）。

产物写到 viewer/icons/：
  icon-180.png            iOS「添加到主屏幕」用，苹果自动加圆角
  icon-192.png            Android / manifest
  icon-512.png            Android / manifest 高清
  icon-512-maskable.png   Android 自适应图标（内容收在中间 80% 安全区内）
  icon-32.png             favicon
  favicon.ico             老浏览器兜底

改完重跑：python make_icons.py
依赖：Pillow、numpy（numpy 缺失时退化成纯色背景）
"""
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'icons')

MASTER = 1024            # 先在 1024 上画，再缩到各尺寸，边缘更干净
BLUE = (76, 141, 255)
BLUE_SOFT = (120, 170, 255)
GREEN = (53, 208, 127)


def background(size):
    """深色径向渐变 + 顶部蓝色微光，跟页面 #0b0d10 一脉相承。"""
    try:
        import numpy as np
    except ImportError:
        return Image.new('RGB', (size, size), (11, 13, 16))

    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = cy = size / 2.0
    r = np.clip(np.hypot(xx - cx, yy - cy) / (size * 0.70), 0, 1)
    inner = np.array([20, 26, 35], np.float32)
    outer = np.array([7, 9, 12], np.float32)
    img = inner[None, None, :] + (outer - inner)[None, None, :] * r[..., None]

    glow = np.exp(-(((xx - cx) / (size * 0.52)) ** 2 + ((yy - size * 0.10) / (size * 0.34)) ** 2))
    img += np.array([26, 58, 132], np.float32)[None, None, :] * glow[..., None] * 0.6
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')


def draw_icon(inset=0.0):
    """inset: 内容整体缩放系数，maskable 用 0.18 把内容收进安全区。"""
    S = MASTER
    img = background(S).convert('RGBA')
    d = ImageDraw.Draw(img, 'RGBA')

    cx = cy = S / 2.0
    k = 1.0 - inset                     # 内容缩放
    R = S * 0.300 * k                   # 外环半径

    # 镜头下方的柔光，让蓝色浮起来
    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([cx - R * 1.15, cy - R * 1.15, cx + R * 1.15, cy + R * 1.15], fill=BLUE + (70,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(S * 0.055)))

    # 镜头外环（金属感）
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=(16, 21, 29, 255),
              outline=(38, 50, 66, 255), width=int(S * 0.026))
    d.arc([cx - R, cy - R, cx + R, cy + R], 200, 340, fill=(58, 76, 99, 255), width=int(S * 0.012))

    # 蓝色光圈
    r2 = R * 0.80
    d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], outline=BLUE + (255,), width=int(S * 0.030))
    d.ellipse([cx - r2 * 0.92, cy - r2 * 0.92, cx + r2 * 0.92, cy + r2 * 0.92], fill=(8, 12, 18, 255))

    # 中心镜头玻璃 + 蓝色内芯
    r3 = R * 0.52
    glass = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    gdd = ImageDraw.Draw(glass)
    gdd.ellipse([cx - r3, cy - r3, cx + r3, cy + r3], fill=(18, 46, 108, 255))
    glass = glass.filter(ImageFilter.GaussianBlur(S * 0.012))
    img.alpha_composite(glass)
    d = ImageDraw.Draw(img, 'RGBA')
    d.ellipse([cx - r3, cy - r3, cx + r3, cy + r3], fill=(22, 52, 118, 255), outline=BLUE_SOFT + (190,),
              width=int(S * 0.010))

    # 高光（左上，玻璃反光，边缘要虚）
    hr = r3 * 0.26
    gx, gy = cx - r3 * 0.42, cy - r3 * 0.46
    shine = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shine)
    sd.ellipse([gx - hr, gy - hr * 1.15, gx + hr, gy + hr * 1.15], fill=(214, 232, 255, 170))
    img.alpha_composite(shine.filter(ImageFilter.GaussianBlur(S * 0.018)))

    # 四角 HUD 直角线，和页面里的四角取景框一致
    m = S * 0.155 * k + S * 0.055
    L = S * 0.085 * k
    w = max(2, int(S * 0.011))
    col = BLUE + (120,)
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        px = m if sx > 0 else S - m
        py = m if sy > 0 else S - m
        d.line([px, py, px + sx * L, py], fill=col, width=w)
        d.line([px, py, px, py + sy * L], fill=col, width=w)

    # 底部状态点：绿色，暗示"在线"
    dot_y = S - m * 0.86
    d.ellipse([cx - S * 0.014, dot_y - S * 0.014, cx + S * 0.014, dot_y + S * 0.014], fill=GREEN + (235,))
    return img


def save(img, size, name, alpha_bg=(11, 13, 16, 255)):
    small = img.resize((size, size), Image.LANCZOS)
    if name.endswith('.ico'):
        small.save(os.path.join(OUT, name), sizes=[(16, 16), (32, 32), (48, 48)])
        return
    out = Image.new('RGBA', (size, size), alpha_bg)   # 不透明底，避免 iOS 黑块
    out.alpha_composite(small)
    out.convert('RGB').save(os.path.join(OUT, name), optimize=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    base = draw_icon(0.0)
    mask = draw_icon(0.18)

    save(base, 180, 'icon-180.png')
    save(base, 192, 'icon-192.png')
    save(base, 512, 'icon-512.png')
    save(mask, 512, 'icon-512-maskable.png')
    save(base, 32, 'icon-32.png')
    save(base, 64, 'favicon.ico')

    for f in sorted(os.listdir(OUT)):
        p = os.path.join(OUT, f)
        print('%-24s %6d B' % (f, os.path.getsize(p)))


if __name__ == '__main__':
    main()

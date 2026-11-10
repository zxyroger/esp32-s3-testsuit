# -*- coding: utf-8 -*-
"""对比 加性(当前线上) vs 乘性(gain) flat-field，并扫低通窗口 K。
输出：横纹指标 + 校正后对比图 sim2_*.png"""
import glob, numpy as np
from PIL import Image

def lum(d): return 0.299*d[...,0]+0.587*d[...,1]+0.114*d[...,2]
def smooth_rows(rm,k):
    h=len(rm); out=np.empty(h)
    for y in range(h): out[y]=rm[max(0,y-k):min(h,y+k+1)].mean()
    return out
def hp(d,k=10):
    rows=lum(d).mean(axis=1); lp=smooth_rows(rows,k)
    return rows-lp
def bandseg(d, k_meas=10):
    h=hp(d,k_meas); L=lum(d).mean(axis=1); lp=smooth_rows(L,k_meas)
    return h.std(), h[lp<90].std(), h[lp>150].std()

frames=[np.asarray(Image.open(f).convert('RGB'),dtype=np.float32) for f in sorted(glob.glob('frames_0841/cam_*.jpg'))]
H,W,_=frames[0].shape

def avgseg(rs): return tuple(np.mean([r[i] for r in rs]) for i in range(3))

# ---- 理想 profile（全10帧均值）----
def ideal_additive(K):
    offs=[]
    for d in frames:
        L=lum(d); rows=L.mean(axis=1); lp=smooth_rows(rows,K)
        offs.append(rows-lp)
    return np.mean(offs,0)

def ideal_multiplicative(K):
    gains=[]
    for d in frames:
        rows=[]
        for c in range(3):
            r=d[...,c].mean(axis=1); rows.append(r)
        gs=[]
        for c in range(3):
            lp=smooth_rows(rows[c],K)
            g=lp/np.maximum(rows[c],1)
            gs.append(g)
        gains.append(gs)
    return [np.mean([g[c] for g in gains],0) for c in range(3)]

def apply_mul(d, gains, norm=True):
    dd=d.copy()
    for c in range(3):
        g=gains[c].copy()
        if norm: g=g/g.mean()
        dd[...,c]=np.clip(dd[...,c]*g[:,None],0,255)
    return dd

def apply_add(d, off):
    return np.clip(d-off[:,None,None],0,255)

print('=== 理想 profile 对比（全10帧均值）===')
print('%-24s %8s %8s %8s'%('方案','总体','暗<90','亮>150'))
orig=[bandseg(d) for d in frames]
print('%-24s %8.2f %8.2f %8.2f'%(('原始',)+avgseg(orig)))
for K in [10,20,30,40,50]:
    off=ideal_additive(K)
    r=[bandseg(apply_add(d,off)) for d in frames]
    print('%-24s %8.2f %8.2f %8.2f'%((f'加性 K={K}',)+avgseg(r)))
for K in [10,20,30,40,50]:
    gains=ideal_multiplicative(K)
    r=[bandseg(apply_mul(d,gains)) for d in frames]
    print('%-24s %8.2f %8.2f %8.2f'%((f'乘性 K={K}',)+avgseg(r)))

# 生成对比图（中间帧）
mid=len(frames)//2
d=frames[mid]
Image.fromarray(d.astype(np.uint8)).save('sim2_raw.png')
Image.fromarray(apply_add(d, ideal_additive(10)).astype(np.uint8)).save('sim2_addK10.png')
Image.fromarray(apply_mul(d, ideal_multiplicative(40)).astype(np.uint8)).save('sim2_mulK40.png')
Image.fromarray(apply_mul(d, ideal_multiplicative(50)).astype(np.uint8)).save('sim2_mulK50.png')
print('\n已生成 sim2_raw.png / sim2_addK10.png / sim2_mulK40.png / sim2_mulK50.png')

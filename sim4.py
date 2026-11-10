# -*- coding: utf-8 -*-
"""验证：前 N 帧简单平均快速建立 profile，之后慢 EMA 稳定。
对比固定 alpha 方案，看收敛轨迹。"""
import glob, numpy as np
from PIL import Image

def lum(d): return 0.299*d[...,0]+0.587*d[...,1]+0.114*d[...,2]
def smooth_rows(rm,k):
    h=len(rm); out=np.empty(h)
    for y in range(h): out[y]=rm[max(0,y-k):min(h,y+k+1)].mean()
    return out
def hp(d):
    rows=lum(d).mean(axis=1); lp=smooth_rows(rows,10)
    return rows-lp

frames=[np.asarray(Image.open(f).convert('RGB'),dtype=np.float32) for f in sorted(glob.glob('frames_0841/cam_*.jpg'))]
H,W,_=frames[0].shape

def run_fast_avg(K, MAX, N_FAST, SLOW_A):
    """前 N_FAST 帧简单平均，之后慢 EMA"""
    fsum=np.zeros(H); cnt=0; ffc=np.zeros(H)
    bands=[]
    for d in frames:
        rows=lum(d).mean(axis=1); lp=smooth_rows(rows,K)
        r=np.clip(rows-lp,-MAX,MAX)
        if cnt < N_FAST:
            fsum += r; cnt += 1; ffc = fsum/cnt
        else:
            ffc += (r-ffc)*SLOW_A
        out=np.clip(d-ffc[:,None,None],0,255)
        bands.append(hp(out).std())
    return bands

def run_fixed(K, MAX, A):
    ffc=np.zeros(H); bands=[]
    for d in frames:
        rows=lum(d).mean(axis=1); lp=smooth_rows(rows,K)
        r=np.clip(rows-lp,-MAX,MAX)
        ffc += (r-ffc)*A
        out=np.clip(d-ffc[:,None,None],0,255)
        bands.append(hp(out).std())
    return bands

raw=[hp(d).std() for d in frames]
print('%-4s'%'raw' + ''.join('%-8.2f'%v for v in raw))
print('%-4s %-8s'%('','原始') + ''.join('%-8.2f'%v for v in raw))

schemes = {
  '固定a.25 K25钳45':  lambda: run_fixed(25,45,0.25),
  '前10平均后a.05 K25': lambda: run_fast_avg(25,45,10,0.05),
  '前15平均后a.05 K25': lambda: run_fast_avg(25,45,15,0.05),
  '前20平均后a.05 K25': lambda: run_fast_avg(25,45,20,0.05),
  '前10平均后a.05 K30': lambda: run_fast_avg(30,45,10,0.05),
}
for name,fn in schemes.items():
    bands=fn()
    print('%-20s'%name + ''.join('%-8.2f'%v for v in bands))

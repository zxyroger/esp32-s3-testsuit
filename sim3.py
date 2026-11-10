# -*- coding: utf-8 -*-
"""真实前端管线仿真：模拟 EMA 慢收敛 + 钳位 + 运动门控。
对比当前线上方案 vs 改进方案，看 10 帧收敛轨迹。"""
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

frames=[np.asarray(Image.open(f).convert('RGB'),dtype=np.float32) for f in sorted(glob.glob('frames_0841/cam_*.jpg'))]
H,W,_=frames[0].shape

def run_additive(K, MAX, ALPHA, apply_each_frame=True):
    """加性 flat-field，EMA 收敛。返回每帧校正后的 band 值列表 + 最终 profile"""
    ffc=np.zeros(H)
    bands=[]
    for d in frames:
        L=lum(d); rows=L.mean(axis=1); lp=smooth_rows(rows,K)
        r=rows-lp
        r=np.clip(r,-MAX,MAX)
        ffc += (r-ffc)*ALPHA
        dd=d.copy() if apply_each_frame else d
        out=np.clip(d-ffc[:,None,None],0,255)
        bands.append(hp(out).std())
    return bands, ffc

def run_multiplicative(K, GMAX, GMIN, ALPHA):
    """乘性 flat-field（增益），EMA 收敛，逐帧归1"""
    gffc=np.ones(H)
    bands=[]
    for d in frames:
        L=lum(d); rows=L.mean(axis=1); lp=smooth_rows(rows,K)
        g=lp/np.maximum(rows,1)
        g=np.clip(g,GMIN,GMAX)
        g=g/g.mean()  # 归1保持亮度
        gffc += (g-gffc)*ALPHA
        out=np.clip(d*gffc[:,None,None],0,255)
        bands.append(hp(out).std())
    return bands, gffc

print('逐帧收敛轨迹（band std，越低越好；末帧=收敛结果）')
print('%-4s %-8s'%('帧','原始') + ''.join('%-9s'%f'f{i+1}' for i in range(10)))
raw=[hp(d).std() for d in frames]
print('%-4s %-8s'%('raw','') + ''.join('%-9.2f'%v for v in raw))

schemes = {
  '线上:加性K10钳18a.08': lambda: run_additive(10,18,0.08),
  '加性K25钳45a.08':        lambda: run_additive(25,45,0.08),
  '加性K25钳45a.15':        lambda: run_additive(25,45,0.15),
  '加性K25钳45a.25':        lambda: run_additive(25,45,0.25),
  '乘性K25g.75-1.35a.15':   lambda: run_multiplicative(25,1.35,0.75,0.15),
  '乘性K40g.75-1.35a.15':   lambda: run_multiplicative(40,1.35,0.75,0.15),
}
for name,fn in schemes.items():
    bands,_=fn()
    print('%-22s'%name + ''.join('%-9.2f'%v for v in bands))

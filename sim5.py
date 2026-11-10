# -*- coding: utf-8 -*-
"""精确复刻 app.js 前端降噪管线，对比 当前线上(K10钳18慢EMA) vs 改进(K25钳45快速平均)。
输出 band 指标 + 对比图 sim5_*.png"""
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
def bandseg(d):
    h=hp(d); L=lum(d).mean(axis=1); lp=smooth_rows(L,10)
    return h.std(), h[lp<90].std(), h[lp>150].std()

frames=[np.asarray(Image.open(f).convert('RGB'),dtype=np.float32) for f in sorted(glob.glob('frames_0841/cam_*.jpg'))]
H,W,_=frames[0].shape

def desat(d, thresh=80):
    dd=d.copy()
    L=lum(d)
    mask=L<thresh
    wgt=(L-thresh)/thresh  # 负值
    for c in range(3):
        dd[...,c]=np.where(mask, L + (dd[...,c]-L)*(1+wgt), dd[...,c])
    return dd

def row_norm(d, K=6, lo=0.3, hi=3.0):
    dd=d.copy().astype(np.float32)
    rows=lum(dd).mean(axis=1)
    sm=smooth_rows(rows,K)
    sc=sm/np.maximum(rows,1)
    sc=np.clip(sc,lo,hi)
    return np.clip(dd*sc[:,None,None],0,255)

def pipeline(frames, K, MAX, ALPHA, FAST_N, use_rgb=True):
    """EMA 时间域 + flat-field(加性,分通道) + 行归一化 + 暗部去饱和"""
    acc=None; NR_A=0.55
    ffc=np.zeros((3,H)); fsum=np.zeros((3,H)); cnt=0
    outs=[]
    for d in frames:
        d=d.astype(np.float32)
        # 1. 时域 EMA
        if acc is not None:
            mad=np.abs(d-acc).mean()
            a = 0.12 if mad>14 else (0.3 if mad>5 else 0.55)
            d=acc*a+d*(1-a)
        acc=d.copy()
        # 2. flat-field 分通道
        for c in range(3):
            rows=d[...,c].mean(axis=1)
            lp=smooth_rows(rows,K)
            r=np.clip(rows-lp,-MAX,MAX)
            if FAST_N>0 and cnt<FAST_N:
                fsum[c]+=r
                ffc[c]=fsum[c]/(cnt+1)
            else:
                ffc[c]+=(r-ffc[c])*ALPHA
        cnt+=1
        dd=d.copy()
        for c in range(3):
            dd[...,c]=dd[...,c]-ffc[c][:,None]
        # 3. 行归一化
        dd=row_norm(dd)
        # 4. 暗部去饱和
        dd=desat(dd)
        outs.append(np.clip(dd,0,255))
    return outs

# 当前线上：K10 钳18 α0.08（无快速平均）
cur=pipeline(frames,10,18,0.08,0)
# 改进：K25 钳45 前15帧平均后α0.05
new=pipeline(frames,25,45,0.05,15)

def avgseg(rs): return tuple(np.mean([r[i] for r in rs]) for i in range(3))
print('%-24s %8s %8s %8s'%('方案(完整管线)','总体','暗<90','亮>150'))
print('%-24s %8.2f %8.2f %8.2f'%(('原始',)+avgseg([bandseg(d) for d in frames])))
print('%-24s %8.2f %8.2f %8.2f'%(('线上K10钳18a.08',)+avgseg([bandseg(d) for d in cur])))
print('%-24s %8.2f %8.2f %8.2f'%(('改进K25钳45快均',)+avgseg([bandseg(d) for d in new])))

# 收敛轨迹
print('\n逐帧 band std 轨迹：')
raw=[hp(d).std() for d in frames]
print('原始   '+''.join('%-7.2f'%v for v in raw))
print('线上   '+''.join('%-7.2f'%hp(d).std() for d in cur))
print('改进   '+''.join('%-7.2f'%hp(d).std() for d in new))

mid=len(frames)//2
Image.fromarray(frames[mid].astype(np.uint8)).save('sim5_raw.png')
Image.fromarray(cur[mid].astype(np.uint8)).save('sim5_cur.png')
Image.fromarray(new[mid].astype(np.uint8)).save('sim5_new.png')
print('\n已生成 sim5_raw.png / sim5_cur.png / sim5_new.png')

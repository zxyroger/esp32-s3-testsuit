import sys, glob, numpy as np
from PIL import Image

def to_L(a): return 0.299*a[...,0]+0.587*a[...,1]+0.114*a[...,2]
def acut(a):
    L=to_L(a); gx=np.abs(np.diff(L,axis=1))[:-1,:]; gy=np.abs(np.diff(L,axis=0))[:,:-1]
    g=gx+gy; thr=np.percentile(g,98); return float(g[g>=thr].mean())
def noise(a):
    L=to_L(a); p=np.pad(L,1,mode='edge')
    m=sum(p[dy:dy+L.shape[0],dx:dx+L.shape[1]] for dy in range(3) for dx in range(3))/9.0
    hf=L-m; d=L<45
    return float(hf[d].std()) if d.sum()>500 else float(hf.std())

for d in sys.argv[1:]:
    fs=sorted(glob.glob(d+'/*.jpg'))
    if not fs: print(d,'无帧'); continue
    A=[];N=[];S=[]
    for f in fs:
        a=np.asarray(Image.open(f).convert('RGB'),np.float32)
        A.append(acut(a)); N.append(noise(a)); S.append(len(open(f,'rb').read()))
    print("%-22s n=%d acut=%.2f noise=%.2f 均字节=%d" % (d,len(fs),np.mean(A),np.mean(N),np.mean(S)))

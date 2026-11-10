# -*- coding: utf-8 -*-
"""sim27: 夜间「糊」的四组消融（真实画面，同一时段）。

A 现状：auto 开，alpha≈0.87 / vlpK4 / lam≈0.93
B 关时域平均：长曝光已经积了光，再叠多帧只会拖影
C 弱纵低通：vlpK 4→2、lam 0.9→0.6，条带交给 flickerRowNorm
D B+C 组合，并把锐化提回 1.2

指标：acut = 强边缘（梯度前 2%）平均梯度，抗噪；band4 = 整幅行条带。
"""
import base64
import functools
import http.server
import io
import json
import os
import socketserver
import subprocess
import threading
import time
import urllib.request

import websocket
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, "viewer")
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
HTTP_PORT = 8921
CDP_PORT = 9352

ACUT_JS = """(function(){
  var c=document.getElementById('frame'); if(!c||!c.width) return '{}';
  var w=c.width,h=c.height,d=c.getContext('2d').getImageData(0,0,w,h).data;
  var L=new Float32Array(w*h);
  for(var p=0,i=0;p<w*h;p++,i+=4) L[p]=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];
  var g=[];
  for(var y=1;y<h-1;y++) for(var x=1;x<w-1;x++){
    var p2=y*w+x;
    g.push(Math.abs(L[p2+1]-L[p2-1])/2 + Math.abs(L[p2+w]-L[p2-w])/2);
  }
  g.sort(function(a,b){return b-a;});
  var k=Math.max(1,Math.floor(g.length*0.02)), s=0;
  for(var j=0;j<k;j++) s+=g[j];
  var acut=s/k;
  // 暗区高频残差（近似噪点），用中位数差分压低边缘影响
  var acc=0,n=0;
  for(var yy=1;yy<h-1;yy+=2) for(var xx=1;xx<w-1;xx+=2){
    var q=yy*w+xx; if(L[q]>45) continue;
    acc+=Math.abs(L[q]-(L[q-1]+L[q+1]+L[q-w]+L[q+w])/4); n++;
  }
  return JSON.stringify({w:w,h:h,acut:+acut.toFixed(2),noise:+(acc/Math.max(n,1)).toFixed(2)});
})()"""

VARIANTS = [
    ("A 现状 lam.90 v.35",
     "window.__TUNE.vlpLam=0.90;window.__TUNE.sharpV=0.35;window.__TUNE.vlpK=4;1"),
    ("B lam.75 v.50",
     "window.__TUNE.vlpLam=0.75;window.__TUNE.sharpV=0.50;window.__TUNE.vlpK=4;1"),
    ("C lam.65 v.60 K3",
     "window.__TUNE.vlpLam=0.65;window.__TUNE.sharpV=0.60;window.__TUNE.vlpK=3;1"),
    ("D lam.75 v.50 s1.15",
     "window.__TUNE.vlpLam=0.75;window.__TUNE.sharpV=0.50;window.__TUNE.sharp=1.15;window.__TUNE.vlpK=4;1"),
    ("E 回看 A",
     "window.__TUNE.vlpLam=0.90;window.__TUNE.sharpV=0.35;window.__TUNE.sharp=1.0;window.__TUNE.vlpK=4;1"),
]


def main():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(("127.0.0.1", HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL&diag=1" % HTTP_PORT
    profile = os.path.join(os.environ.get("TEMP", HERE), "edge-soft-%d" % int(time.time()))
    proc = subprocess.Popen([
        EDGE, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
        "--remote-debugging-port=%d" % CDP_PORT, "--user-data-dir=%s" % profile,
        "--window-size=480,860", url,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws_url = None
    for _ in range(60):
        time.sleep(0.25)
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/json" % CDP_PORT, timeout=1) as r:
                for t in json.load(r):
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        ws_url = t["webSocketDebuggerUrl"]
        except Exception:
            continue
        if ws_url:
            break
    if not ws_url:
        print("CDP 失败"); proc.terminate(); httpd.shutdown(); return 1
    ws = websocket.create_connection(ws_url, timeout=120)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({"id": mid[0], "method": method, "params": params}))
        while True:
            m = json.loads(ws.recv())
            if m.get("id") == mid[0]:
                return m

    def ev(e):
        r = call("Runtime.evaluate", expression=e, returnByValue=True)
        return r.get("result", {}).get("result", {}).get("value")

    def shot():
        r = call("Page.captureScreenshot", format="png")
        return Image.open(io.BytesIO(base64.b64decode(r["result"]["data"]))).convert("RGB")

    call("Runtime.enable")
    for _ in range(160):
        time.sleep(0.5)
        if (ev("window.__diag ? window.__diag.n : 0") or 0) >= 3:
            break

    rows, imgs = [], []
    for label, expr in VARIANTS:
        ev(expr)
        n0 = ev("window.__diag ? window.__diag.n : 0") or 0
        t0 = time.time()
        while time.time() - t0 < 70:
            time.sleep(0.5)
            if (ev("window.__diag ? window.__diag.n : 0") or 0) - n0 >= 14:
                break
        m = json.loads(ev(ACUT_JS) or "{}")
        dg = ev("JSON.stringify(window.__diag)") or "{}"
        d = json.loads(dg)
        out = d.get("out", {})
        rows.append((label, m.get("acut", 0), m.get("noise", 0),
                     out.get("band4", 0), out.get("flat", 0), out.get("sharp", 0),
                     out.get("col4", 0)))
        imgs.append((label, shot()))
        print("%-14s acut=%6.2f noise=%5.2f band4=%5.2f flat=%5.3f sharp=%5.2f col4=%5.2f"
              % rows[-1], flush=True)

    W = max(i.width for _, i in imgs)
    H = max(i.height for _, i in imgs)
    out = Image.new("RGB", (W * 2, (H + 26) * 2), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for k, (label, im) in enumerate(imgs):
        x = (k % 2) * W
        y = (k // 2) * (H + 26)
        out.paste(im, (x, y + 24))
        dr.text((x + 8, y + 6), label, fill=(0, 0, 0))
    out.save(os.path.join(HERE, "soft_ab.png"))
    print("已保存 soft_ab.png", flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == "__main__":
    main()

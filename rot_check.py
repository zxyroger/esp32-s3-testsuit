# -*- coding: utf-8 -*-
"""方向 A/B：同一页面同一时刻，切换 rot=0 / rot=1，截图 + 量测画面指标。
用于确认「逆时针 90° 转正」在手机视口里的实际观感与横纹可见度。
"""
import base64
import functools
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time
import urllib.request

import websocket

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, 'viewer')
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
HTTP_PORT = 8911
CDP_PORT = 9341


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-rot-%d' % int(time.time()))
    proc = subprocess.Popen([
        EDGE, '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
        '--remote-debugging-port=%d' % CDP_PORT, '--user-data-dir=%s' % profile,
        '--window-size=480,860', url,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ws_url = None
    for _ in range(40):
        time.sleep(0.25)
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/json' % CDP_PORT, timeout=1) as r:
                for t in json.load(r):
                    if t.get('type') == 'page' and t.get('webSocketDebuggerUrl'):
                        ws_url = t['webSocketDebuggerUrl']
        except Exception:
            continue
        if ws_url:
            break
    if not ws_url:
        print('CDP 失败'); proc.terminate(); httpd.shutdown(); return 1

    ws = websocket.create_connection(ws_url, timeout=60)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({'id': mid[0], 'method': method, 'params': params}))
        while True:
            m = json.loads(ws.recv())
            if m.get('id') == mid[0]:
                return m

    def ev(expr):
        r = call('Runtime.evaluate', expression=expr, returnByValue=True)
        return r.get('result', {}).get('result', {}).get('value')

    def shot(name):
        r = call('Page.captureScreenshot', format='png')
        data = r.get('result', {}).get('data')
        if data:
            p = os.path.join(HERE, name)
            with open(p, 'wb') as f:
                f.write(base64.b64decode(data))
            print('截图:', p)

    call('Runtime.enable')
    time.sleep(wait)
    metric = r"""
    (() => {
      const c=document.getElementById('frame'); const d=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
      const w=c.width,h=c.height;
      const BW=80,BH=80, blocks=[];
      let L=new Float32Array(w*h);
      for(let p=0,i=0;p<w*h;p++,i+=4) L[p]=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];
      for(let by=0;by+BH<=h;by+=BH) for(let bx=0;bx+BW<=w;bx+=BW){
        let s=0,s2=0; for(let y=by;y<by+BH;y++) for(let x=bx;x<bx+BW;x++){const v=L[y*w+x];s+=v;s2+=v*v;}
        const n=BW*BH,m=s/n; blocks.push([Math.sqrt(Math.max(0,s2/n-m*m)),by,bx]);
      }
      blocks.sort((a,b)=>a[0]-b[0]);
      const hp=[];
      for(const b of blocks.slice(0,10)){ const by=b[1],bx=b[2],rr=new Float32Array(BH);
        for(let y=0;y<BH;y++){let s=0;for(let x=0;x<BW;x++)s+=L[(by+y)*w+bx+x];rr[y]=s/BW;}
        for(let y=0;y<BH;y++){let s=0,c=0;for(let j=y-8;j<=y+8;j++)if(j>=0&&j<BH){s+=rr[j];c++;}hp.push(rr[y]-s/c);} }
      let m=0;for(const v of hp)m+=v;m/=hp.length||1;
      let vv=0;for(const v of hp)vv+=(v-m)*(v-m);
      return JSON.stringify({w:w,h:h,flat:+Math.sqrt(vv/(hp.length||1)).toFixed(3),
        cssW:c.getBoundingClientRect().width, cssH:c.getBoundingClientRect().height,
        rot:window.__TUNE?window.__TUNE.rot:-1,
        mean:+(L.reduce((a,b)=>a+b,0)/L.length).toFixed(1)});
    })()
    """
    for rot, nm in [(0, 'rot_0.png'), (1, 'rot_1.png')]:
        ev('window.__TUNE.rot=%d; true' % rot)
        time.sleep(6)
        print('rot=%d -> %s' % (rot, ev(metric)))
        shot(nm)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

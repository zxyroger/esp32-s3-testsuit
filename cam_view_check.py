# -*- coding: utf-8 -*-
"""本地起服务，打开页面连真实 MQTT，等收帧后检查 canvas 是否正常显示 + 截图。

用法：python cam_view_check.py [等待秒数]
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
HTTP_PORT = 8903
CDP_PORT = 9338


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    extra = sys.argv[2] if len(sys.argv) > 2 else ''
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL%s' % (HTTP_PORT, extra)
    profile = os.path.join(os.environ.get('TEMP', HERE),
                           'edge-camview-%d' % int(time.time()))
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
        print('CDP 连接失败'); proc.terminate(); httpd.shutdown(); return 1

    ws = websocket.create_connection(ws_url, timeout=60)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({'id': mid[0], 'method': method, 'params': params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get('id') == mid[0]:
                return msg

    call('Runtime.enable')
    print('等待 %d 秒收帧…' % wait)
    time.sleep(wait)

    expr = r"""
    (() => {
      const c = document.getElementById('frame');
      if (!c) return JSON.stringify({err: 'no canvas'});
      const ctx = c.getContext('2d');
      const d = ctx.getImageData(0, 0, c.width, c.height).data;
      const w = c.width, h = c.height;
      let sum = 0, n = 0, minv = 255, maxv = 0;
      for (let i = 0; i < d.length; i += 4) {
        const v = 0.299*d[i] + 0.587*d[i+1] + 0.114*d[i+2];
        sum += v; n++; if (v < minv) minv = v; if (v > maxv) maxv = v;
      }
      // 每行均值 + 21行低通 -> 高通残差 = 细横纹强度
      const rm = new Float32Array(h), lp = new Float32Array(h);
      for (let y = 0; y < h; y++) {
        let s = 0;
        for (let x = 0; x < w; x++) {
          const i = (y*w + x)*4;
          s += 0.299*d[i] + 0.587*d[i+1] + 0.114*d[i+2];
        }
        rm[y] = s / w;
      }
      for (let y = 0; y < h; y++) {
        let s = 0, nn = 0;
        for (let j = y-10; j <= y+10; j++) { if (j>=0 && j<h) { s += rm[j]; nn++; } }
        lp[y] = s / nn;
      }
      let hp = new Float32Array(h);
      let dark=[], mid=[], bright=[];
      for (let y = 0; y < h; y++) {
        hp[y] = rm[y] - lp[y];
        const seg = lp[y];
        if (seg < 90) dark.push(hp[y]);
        else if (seg <= 150) mid.push(hp[y]);
        else bright.push(hp[y]);
      }
      const sd = (a) => { if (!a.length) return 0; let m=0; for(const v of a) m+=v; m/=a.length;
        let s=0; for(const v of a) s+=(v-m)*(v-m); return Math.sqrt(s/a.length); };
      /* 平坦区横纹：整幅行均值会被楼房的水平结构主导，看起来「很好」但
       * 天空里可能已被整行偏置弄出新的暗带。只取最平坦的 10 个 80x80 块测量。 */
      const BW=80, BH=80, blocks=[];
      for (let by=0; by+BH<=h; by+=BH) for (let bx=0; bx+BW<=w; bx+=BW) {
        let s=0, s2=0, cnt=0;
        for (let y=by; y<by+BH; y++) for (let x=bx; x<bx+BW; x++) {
          const i=(y*w+x)*4, v=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];
          s+=v; s2+=v*v; cnt++;
        }
        const m=s/cnt; blocks.push([Math.sqrt(Math.max(0,s2/cnt-m*m)), by, bx]);
      }
      blocks.sort((a,b)=>a[0]-b[0]);
      const hpv=[];
      for (const blk of blocks.slice(0,10)) {
        const by=blk[1], bx=blk[2], rr=new Float32Array(BH);
        for (let y=0; y<BH; y++) {
          let s=0;
          for (let x=0; x<BW; x++) { const i=((by+y)*w+(bx+x))*4; s+=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2]; }
          rr[y]=s/BW;
        }
        for (let y=0; y<BH; y++) {
          let s=0, cnt=0;
          for (let j=y-8; j<=y+8; j++) if (j>=0 && j<BH) { s+=rr[j]; cnt++; }
          hpv.push(rr[y]-s/cnt);
        }
      }
      const flatBand = sd(hpv);
      /* 纹理区锐度：细节最强的 10 个 80x80 块的平均梯度。时域平均一糊这个数就掉，
       * 只看 band 会误判「降噪越好越干净」，必须同时盯住它。 */
      let sharpSum=0, sharpN=0;
      for (const blk of blocks.slice(-10)) {
        const by=blk[1], bx=blk[2];
        let g=0, cnt=0;
        for (let y=by; y<by+BH-1; y++) for (let x=bx; x<bx+BW-1; x++) {
          const i=(y*w+x)*4, iR=(y*w+x+1)*4, iD=((y+1)*w+x)*4;
          const v  =0.299*d[i]   +0.587*d[i+1]   +0.114*d[i+2];
          const vr =0.299*d[iR]  +0.587*d[iR+1]  +0.114*d[iR+2];
          const vd =0.299*d[iD]  +0.587*d[iD+1]  +0.114*d[iD+2];
          g+=Math.abs(v-vr)+Math.abs(v-vd); cnt++;
        }
        sharpSum+=g/cnt; sharpN++;
      }
      const sharp = sharpSum/sharpN;
      return JSON.stringify({w: w, h: h, mean: +(sum/n).toFixed(1),
        sharp: +sharp.toFixed(2),
        flatBand: +flatBand.toFixed(2),
        min: minv, max: maxv,
        band: +sd(Array.from(hp)).toFixed(2), bandDark: +sd(dark).toFixed(2),
        bandMid: +sd(mid).toFixed(2), bandBright: +sd(bright).toFixed(2),
        ffc: window.__ffc || null,
        nr: window.__nr || null,
        diag: window.__diag || null,
        auto: window.__auto || null,
        rotText: document.getElementById('rotBtn') ? document.getElementById('rotBtn').textContent : '',
        tune: window.__TUNE || null,
        hasFn: typeof fixedRowProfile,
        state: document.getElementById('state') ? document.getElementById('state').textContent : ''});
    })()
    """
    r = call('Runtime.evaluate', expression=expr, returnByValue=True)
    print('画面:', r.get('result', {}).get('result', {}).get('value'))

    shot = call('Page.captureScreenshot', format='png')
    data = shot.get('result', {}).get('data')
    out = os.path.join(HERE, 'shot_denoise.png')
    if data:
        with open(out, 'wb') as f:
            f.write(base64.b64decode(data))
        print('截图:', out)

    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

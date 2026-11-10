# -*- coding: utf-8 -*-
"""同一时段、同一场景下的 A/B 视觉对比：先在页面里注入旧参数截图，
再注入新参数截图，拼成一张并排图，避免不同时刻光照差异干扰判断。
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
from PIL import Image, ImageDraw
import io

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, 'viewer')
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
HTTP_PORT = 8909
CDP_PORT = 9344

OLD = dict(m0=4, span=3, alpha=0.55, sharp=0.30, sharpH=0,
           ffcK=25, ffcAlpha=0.05, ffcFast=20, ffcMotion=12, ffcMax=50,
           ema=1, ffc=1, rn=1, ro=1, sharpOn=1,
           vlp=0, vlpK=1, vlpLam=1.0, vlpG0=2, vlpKg=8, resetFfc=0, rot=0)
NEW = dict(m0=7, span=3, alpha=0.80, sharp=0.80, sharpH=1,
           ffcK=25, ffcAlpha=0.05, ffcFast=20, ffcMotion=12, ffcMax=50,
           ema=1, ffc=0, rn=0, ro=0, sharpOn=1,
           vlp=1, vlpK=2, vlpLam=0.8, vlpG0=2, vlpKg=8, resetFfc=0, rot=0)


def main():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL&diag=1' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-ab-%d' % int(time.time()))
    proc = subprocess.Popen([
        EDGE, '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
        '--remote-debugging-port=%d' % CDP_PORT, '--user-data-dir=%s' % profile,
        '--window-size=480,860', url,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws_url = None
    for _ in range(60):
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
    ws = websocket.create_connection(ws_url, timeout=90)
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

    def shot():
        r = call('Page.captureScreenshot', format='png')
        return Image.open(io.BytesIO(base64.b64decode(r['result']['data']))).convert('RGB')

    call('Runtime.enable')
    print('等待首帧…', flush=True)
    for _ in range(120):
        time.sleep(0.5)
        if (ev('window.__diag ? window.__diag.n : 0') or 0) >= 3:
            break

    imgs = []
    for label, cfg in (('A 旧管线（现状）', OLD), ('B 新管线', NEW)):
        ev('Object.assign(window.__TUNE, %s); 1' % json.dumps(cfg))
        n0 = ev('window.__diag ? window.__diag.n : 0') or 0
        t0 = time.time()
        while time.time() - t0 < 60:
            time.sleep(0.4)
            if (ev('window.__diag ? window.__diag.n : 0') or 0) - n0 >= 10:
                break
        d = ev('JSON.stringify(window.__diag)')
        print('%s  %s' % (label, d), flush=True)
        imgs.append((label, shot()))

    W = max(i.width for _, i in imgs)
    H = max(i.height for _, i in imgs)
    out = Image.new('RGB', (W * len(imgs), H + 28), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for k, (label, im) in enumerate(imgs):
        out.paste(im, (k * W, 28))
        dr.text((k * W + 8, 8), label, fill=(0, 0, 0))
    out.save(os.path.join(HERE, 'ab_visual.png'))
    print('已保存 ab_visual.png', out.size, flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

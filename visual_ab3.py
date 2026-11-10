# -*- coding: utf-8 -*-
"""三组 A/B 视觉对比：现状 K2 / K4 温和 / K4 全激进。
同一页面同一时段依次注入，避免光照漂移；输出整页拼图 + 暗部放大拼图。
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
VIEWER = os.path.join(HERE, 'viewer')
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
HTTP_PORT = 8913
CDP_PORT = 9346

BASE = dict(span=3, ffcK=25, ffcAlpha=0.05, ffcFast=20, ffcMotion=12, ffcMax=50,
            ema=1, ffc=0, rn=0, ro=0, sharpOn=1, sharpH=1,
            vlp=1, vlpG0=2, vlpKg=8, resetFfc=0, rot=1)

GROUPS = [
    ('A 现状 K2', dict(m0=7, alpha=0.80, vlpK=2, vlpLam=0.8, sharp=0.80)),
    ('B K4 温和', dict(m0=7, alpha=0.85, vlpK=4, vlpLam=0.9, sharp=1.00)),
    ('C K4 全激进', dict(m0=4, alpha=0.92, vlpK=4, vlpLam=1.0, sharp=1.20)),
]


def main():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL&diag=1' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-ab3-%d' % int(time.time()))
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
    for _ in range(120):
        time.sleep(0.5)
        if (ev('window.__diag ? window.__diag.n : 0') or 0) >= 3:
            break

    imgs = []
    for label, cfg in GROUPS:
        c = dict(BASE, **cfg)
        ev('Object.assign(window.__TUNE, %s); 1' % json.dumps(c))
        n0 = ev('window.__diag ? window.__diag.n : 0') or 0
        t0 = time.time()
        while time.time() - t0 < 50:
            time.sleep(0.4)
            if (ev('window.__diag ? window.__diag.n : 0') or 0) - n0 >= 9:
                break
        print('%s  %s' % (label, ev('JSON.stringify(window.__diag)')), flush=True)
        imgs.append((label, shot()))

    W = max(i.width for _, i in imgs)
    H = max(i.height for _, i in imgs)
    out = Image.new('RGB', (W * len(imgs), H + 28), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for k, (label, im) in enumerate(imgs):
        out.paste(im, (k * W, 28))
        dr.text((k * W + 8, 8), label, fill=(0, 0, 0))
    out.save(os.path.join(HERE, 'ab3_visual.png'))

    # 暗部放大：转正后左侧暗区大致在 x=10..190, y=300..700
    crop = (10, 300, 190, 700)
    zs = []
    for label, im in imgs:
        p = im.crop(crop).resize((360, 800), Image.LANCZOS)
        zs.append((label, p))
    z = Image.new('RGB', (360 * len(zs), 826), (250, 250, 250))
    dz = ImageDraw.Draw(z)
    for k, (label, p) in enumerate(zs):
        z.paste(p, (k * 360, 26))
        dz.text((k * 360 + 6, 6), label, fill=(0, 0, 0))
    z.save(os.path.join(HERE, 'ab3_dark_zoom.png'))
    print('已保存 ab3_visual.png / ab3_dark_zoom.png', flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

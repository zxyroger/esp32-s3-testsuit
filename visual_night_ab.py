# -*- coding: utf-8 -*-
"""夜间 A/B：同一页面同一时刻，flicker 抑制 关/开 各截一张，并放大暗部看伪影。"""
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
HTTP_PORT = 8917
CDP_PORT = 9348


def main():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL&diag=1' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-night-%d' % int(time.time()))
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

    def ev(e):
        r = call('Runtime.evaluate', expression=e, returnByValue=True)
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
    for label, fl in (('A 关闭工频抑制', 0), ('B 开启工频抑制', 1)):
        ev('window.__TUNE.flicker=%d; 1' % fl)
        n0 = ev('window.__diag ? window.__diag.n : 0') or 0
        t0 = time.time()
        while time.time() - t0 < 45:
            time.sleep(0.4)
            if (ev('window.__diag ? window.__diag.n : 0') or 0) - n0 >= 8:
                break
        print('%s  %s' % (label, ev('JSON.stringify(window.__diag)')), flush=True)
        imgs.append((label, shot()))

    W = max(i.width for _, i in imgs)
    H = max(i.height for _, i in imgs)
    out = Image.new('RGB', (W * 2, H + 28), (250, 250, 250))
    dr = ImageDraw.Draw(out)
    for k, (label, im) in enumerate(imgs):
        out.paste(im, (k * W, 28))
        dr.text((k * W + 8, 8), label, fill=(0, 0, 0))
    out.save(os.path.join(HERE, 'night_ab.png'))

    crop = (10, 120, 250, 560)
    zs = [(label, im.crop(crop).resize((480, 880), Image.LANCZOS)) for label, im in imgs]
    z = Image.new('RGB', (480 * 2, 906), (250, 250, 250))
    dz = ImageDraw.Draw(z)
    for k, (label, p) in enumerate(zs):
        z.paste(p, (k * 480, 26))
        dz.text((k * 480 + 6, 6), label, fill=(0, 0, 0))
    z.save(os.path.join(HERE, 'night_ab_zoom.png'))
    print('已保存 night_ab.png / night_ab_zoom.png', flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

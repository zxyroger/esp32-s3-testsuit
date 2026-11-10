# -*- coding: utf-8 -*-
"""本地验证 PWA manifest：起静态服务 -> Edge headless -> Page.getAppManifest。

用法：python pwa_check.py
"""
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
HTTP_PORT = 8899
CDP_PORT = 9334


def serve():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def main():
    # 不带参数 = 本地起服务自检；带 URL = 直接检查线上
    if len(sys.argv) > 1:
        httpd = None
        url = sys.argv[1]
    else:
        httpd = serve()
        url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL' % HTTP_PORT

    # 静态资源本身先探一遍（仅本地模式）
    for p in (('manifest.webmanifest', 'icons/icon-180.png', 'icons/icon-192.png',
               'icons/icon-512.png', 'icons/icon-512-maskable.png', 'icons/icon-32.png')
              if httpd else ()):
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/%s' % (HTTP_PORT, p), timeout=3) as r:
                print('%-30s %s  %s  %d B' % (p, r.status, r.headers.get('Content-Type'), len(r.read())))
        except Exception as e:
            print('%-30s FAIL %s' % (p, e))

    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-pwa-check')
    proc = subprocess.Popen([
        EDGE, '--headless=new', '--disable-gpu', '--no-sandbox',
        '--remote-debugging-port=%d' % CDP_PORT,
        '--user-data-dir=%s' % profile, url,
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
        print('CDP 连接失败')
        return 1

    ws = websocket.create_connection(ws_url, timeout=30)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({'id': mid[0], 'method': method, 'params': params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get('id') == mid[0]:
                return msg

    time.sleep(3)
    res = call('Page.getAppManifest')
    r = res.get('result', {})
    print('\n--- manifest ---')
    print('url:', r.get('url'))
    print('errors:', json.dumps(r.get('errors'), ensure_ascii=False))
    print('parsed:', json.dumps(r.get('parsed'), ensure_ascii=False, indent=2)[:1200])
    data = r.get('data') or ''
    print('data 长度:', len(data))

    # 可安装性检查：没有 service worker 会报 no-service-worker，图标不合格会报 manifest-missing-suitable-icon
    ins = call('Page.getInstallabilityErrors')
    print('installability:', json.dumps(ins.get('result'), ensure_ascii=False))

    # 图标能否真的取到（浏览器解析出的 src 是绝对 URL）
    try:
        parsed = json.loads(r.get('parsed') or '{}')
        icons = parsed.get('icons') or []
    except Exception:
        icons = []
    for ic in icons:
        src = ic.get('src')
        try:
            with urllib.request.urlopen(src, timeout=5) as resp:
                print('icon %-52s %s %s %d B' % (src, resp.status,
                                                 resp.headers.get('Content-Type'), len(resp.read())))
        except Exception as e:
            print('icon %-52s FAIL %s' % (src, e))

    # 顺便记下 manifest link 是否被识别
    r2 = call('Runtime.evaluate', expression=(
        "JSON.stringify({"
        "links:[...document.querySelectorAll('link[rel*=icon],link[rel=manifest]')].map(e=>e.rel+':'+e.href),"
        "title:document.title,"
        "standalone:matchMedia('(display-mode: standalone)').matches})"),
        returnByValue=True)
    print('\nDOM:', r2.get('result', {}).get('result', {}).get('value'))

    ws.close()
    proc.terminate()
    if httpd:
        httpd.shutdown()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

# -*- coding: utf-8 -*-
"""check_batt_ui: 用 headless Edge 打开本地页面，读电池区 DOM 文本，验证 VBUS 显示。

用法：python check_batt_ui.py [等待秒数]
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
HTTP_PORT = 8907
CDP_PORT = 9341


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-batt-%d' % int(time.time()))
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
        print('CDP 连接失败')
        proc.terminate()
        httpd.shutdown()
        return 1

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
    print('等待 %d 秒收数据…' % wait, flush=True)
    time.sleep(wait)

    expr = r"""
    (() => {
      const g = id => { const e = document.getElementById(id); return e ? e.textContent.trim() : null; };
      return JSON.stringify({
        vbat: g('bVbat'), vsys: g('bVsys'), vbus: g('bVbus'), pct: g('bPct'),
        raw: (window.__lastBatt ? window.__lastBatt : null)
      });
    })()
    """
    r = call('Runtime.evaluate', expression=expr, returnByValue=True)
    print('DOM:', r.get('result', {}).get('result', {}).get('value'), flush=True)

    proc.terminate()
    try:
        ws.close()
    except Exception:
        pass
    httpd.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main())

# -*- coding: utf-8 -*-
"""精确诊断 headless 页面到底收到多少 MQTT 消息 / 处理了多少帧。

在页面加载前 hook WebSocket 的 message 事件，统计条数与字节数，
再与 __ffc.count、页面 fps 文本对照，判断是否存在重复处理。
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
HTTP_PORT = 8905
CDP_PORT = 9340

HOOK = r"""
(() => {
  window.__wsMsg = 0; window.__wsBytes = 0; window.__wsFrames = [];
  const O = window.WebSocket;
  function W(...a) {
    const ws = new O(...a);
    ws.addEventListener('message', (e) => {
      window.__wsMsg++;
      const n = (e.data && e.data.byteLength) || (e.data && e.data.length) || 0;
      window.__wsBytes += n;
      if (window.__wsFrames.length < 200) window.__wsFrames.push(n);
    }, true);
    return ws;
  }
  W.prototype = O.prototype;
  W.OPEN = O.OPEN; W.CLOSED = O.CLOSED; W.CONNECTING = O.CONNECTING; W.CLOSING = O.CLOSING;
  window.WebSocket = W;
})();
"""

EXPR = r"""
JSON.stringify({
  wsMsg: window.__wsMsg, wsBytes: window.__wsBytes,
  wsBig: (window.__wsFrames || []).filter(n => n > 1000).length,
  ffc: window.__ffc, nr: window.__nr,
  fpsText: document.getElementById('fps') ? document.getElementById('fps').textContent : '',
  sizeText: document.getElementById('size') ? document.getElementById('size').textContent : '',
  state: document.getElementById('state') ? document.getElementById('state').textContent : ''
})
"""


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-counter-%d' % int(time.time()))
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

    call('Runtime.enable')
    call('Page.enable')
    call('Page.addScriptToEvaluateOnNewDocument', source=HOOK)
    call('Page.reload', ignoreCache=True)
    print('已注入计数 hook，页面重载，采样 %ds' % wait, flush=True)
    for t in (10, 20, 30, 45, 60, 90, 120):
        if t > wait:
            break
        while True:
            # 用相对起点计时
            pass
            break
    start = time.time()
    marks = [10, 20, 30, 45, 60, 90, 120]
    for m in marks:
        if m > wait:
            break
        while time.time() - start < m:
            time.sleep(0.5)
        r = call('Runtime.evaluate', expression=EXPR, returnByValue=True)
        v = r.get('result', {}).get('result', {}).get('value')
        print('t=%3ds %s' % (m, v), flush=True)
    if wait not in marks:
        time.sleep(max(0, wait - (time.time() - start)))
        r = call('Runtime.evaluate', expression=EXPR, returnByValue=True)
        print('t=%3ds %s' % (wait, r.get('result', {}).get('result', {}).get('value')), flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

# -*- coding: utf-8 -*-
"""本地检查「像真应用」相关的三件事：
  1. service worker 是否注册成功（安卓 WebAPK 的前提）
  2. 移动端 UA 下安装提示条是否出现（桌面 UA 不该出现）
  3. 页面有没有 JS 报错

用法：python ui_check.py [ios|android|desktop]
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
HTTP_PORT = 8901
CDP_PORT = 9336

UA = {
    'ios': 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 '
           '(KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1',
    'android': 'Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 '
               '(KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36',
    'desktop': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
               '(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36',
}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'ios'
    out = os.path.join(HERE, 'shot_install_%s.png' % mode)

    if len(sys.argv) > 2:                       # 直接检查线上 URL
        httpd = None
        url = sys.argv[2]
    else:
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
        httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        url = 'http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL' % HTTP_PORT
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-ui-check-%s' % mode)
    proc = subprocess.Popen([
        EDGE, '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
        '--user-agent=%s' % UA.get(mode, UA['desktop']),
        '--remote-debugging-port=%d' % CDP_PORT,
        '--user-data-dir=%s' % profile,
        '--window-size=390,844', url,
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
        if httpd:
            httpd.shutdown()
        return 1

    ws = websocket.create_connection(ws_url, timeout=30)
    mid = [0]
    errors = []

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({'id': mid[0], 'method': method, 'params': params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get('method') in ('Runtime.exceptionThrown', 'Log.entryAdded'):
                errors.append(json.dumps(msg.get('params'), ensure_ascii=False)[:400])
            if msg.get('id') == mid[0]:
                return msg

    call('Runtime.enable')
    call('Log.enable')
    time.sleep(4)

    expr = r"""
    (async () => {
      const out = {ua: navigator.userAgent.slice(0, 60), standalone:
        (matchMedia('(display-mode: standalone)').matches || navigator.standalone === true),
        barHidden: document.getElementById('installbar') ? document.getElementById('installbar').hidden : null,
        barText: document.getElementById('installTxt') ? document.getElementById('installTxt').textContent : null,
        sw: null, controller: !!navigator.serviceWorker.controller};
      try {
        const regs = await navigator.serviceWorker.getRegistrations();
        out.sw = regs.map(r => ({scope: r.scope, active: !!(r.active || r.installing || r.waiting)}));
        if (!regs.length) {
          const r = await navigator.serviceWorker.ready;
          out.sw = [{scope: r.scope, active: true}];
        }
      } catch (e) { out.sw = 'ERR ' + e.message; }
      out.scrollW = document.documentElement.scrollWidth;
      out.innerW = innerWidth;
      return JSON.stringify(out);
    })()
    """
    r = call('Runtime.evaluate', expression=expr, awaitPromise=True, returnByValue=True)
    print(r.get('result', {}).get('result', {}).get('value'))

    shot = call('Page.captureScreenshot', format='png')
    data = shot.get('result', {}).get('data')
    if data:
        with open(out, 'wb') as f:
            f.write(base64.b64decode(data))
        print('截图:', out)

    if errors:
        print('JS 报错:')
        for e in errors[:5]:
            print(' ', e)

    ws.close()
    proc.terminate()
    if httpd:
        httpd.shutdown()


if __name__ == '__main__':
    main()

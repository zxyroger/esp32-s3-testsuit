# -*- coding: utf-8 -*-
"""真实流参数扫描：同一个 headless 页面里热改 window.__TUNE，逐组测量
同帧「降噪前/后」的平坦区横纹与锐度。比离线仿真更可信（真实 AEC/增益/运动）。

用法: python tune_scan.py [每组采集帧数] [预热帧数]
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
CDP_PORT = 9342

DEFAULT = dict(m0=4, span=3, alpha=0.55, sharp=0.30, sharpH=0,
               ffcK=25, ffcAlpha=0.05, ffcFast=20, ffcMotion=12, ffcMax=50,
               ema=1, ffc=1, rn=1, ro=1, sharpOn=1,
               vlp=0, vlpK=1, vlpLam=1.0, vlpG0=2, vlpKg=8, resetFfc=0, auto=0,
               guardMode=0, locG0=2, locK=24)

GROUPS = [
    ('baseline',            {}),
    ('no EMA',              {'ema': 0}),
    ('no FFC',              {'ffc': 0}),
    ('no RN/RO',            {'rn': 0, 'ro': 0}),
    ('no sharpen',          {'sharpOn': 0}),
    ('raw (all off)',       {'ema': 0, 'ffc': 0, 'rn': 0, 'ro': 0, 'sharpOn': 0}),
    ('m0=6.5',              {'m0': 6.5}),
    ('m0=8',                {'m0': 8}),
    ('alpha=0.75',          {'alpha': 0.75}),
    ('alpha=0.85',          {'alpha': 0.85}),
    ('vlp K1 lam0.6',       {'vlp': 1, 'vlpK': 1, 'vlpLam': 0.6}),
    ('vlp K2 lam0.6',       {'vlp': 1, 'vlpK': 2, 'vlpLam': 0.6}),
    ('vlp K2 lam1.0',       {'vlp': 1, 'vlpK': 2, 'vlpLam': 1.0}),
    ('m0=6.5+vlpK1',        {'m0': 6.5, 'vlp': 1, 'vlpK': 1, 'vlpLam': 0.6}),
    ('m0=6.5+vlpK2',        {'m0': 6.5, 'vlp': 1, 'vlpK': 2, 'vlpLam': 0.6}),
    ('m0=6.5+vlpK2+sh0.4',  {'m0': 6.5, 'vlp': 1, 'vlpK': 2, 'vlpLam': 0.6, 'sharp': 0.40}),
    ('m0=6.5+alpha0.7+vlpK2', {'m0': 6.5, 'alpha': 0.70, 'vlp': 1, 'vlpK': 2, 'vlpLam': 0.6}),
]

# 需要重建 profile 的组：改 ffcK 时必须 reset 并预热
FFC_GROUPS = [
    ('ffcK=12',  {'ffcK': 12}),
    ('ffcK=40',  {'ffcK': 40}),
    ('ffcK=12+vlpK2', {'ffcK': 12, 'vlp': 1, 'vlpK': 2, 'vlpLam': 0.6, 'm0': 6.5}),
]

NFR = int(sys.argv[1]) if len(sys.argv) > 1 else 7
WARM = int(sys.argv[2]) if len(sys.argv) > 2 else 3
OUT = sys.argv[4] if len(sys.argv) > 4 else 'tune_scan_result.json'

if len(sys.argv) > 3:
    with open(sys.argv[3], encoding='utf-8') as _f:
        _g = json.load(_f)
    GROUPS = _g.get('groups', GROUPS)
    FFC_GROUPS = _g.get('ffc_groups', [])


def main():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=VIEWER)
    httpd = socketserver.TCPServer(('127.0.0.1', HTTP_PORT), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = ('http://127.0.0.1:%d/index.html?sharecode=ECSHHOLNDL&diag=1' % HTTP_PORT)
    profile = os.path.join(os.environ.get('TEMP', HERE), 'edge-tune-%d' % int(time.time()))
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

    call('Runtime.enable')
    print('等待首帧…', flush=True)
    for _ in range(120):
        time.sleep(0.5)
        n = ev('window.__diag ? window.__diag.n : 0')
        if n and n >= 3:
            break
    print('页面已收帧, 开始扫描。每组 %d 帧 + 预热 %d 帧' % (NFR, WARM), flush=True)
    print('%-26s %8s %8s %8s %8s %8s %7s' %
          ('group', 'rawFlat', 'outFlat', 'gain%', 'sharp', 'rawShrp', 'mad'), flush=True)

    def run_group(name, override, reset=False):
        cfg = dict(DEFAULT)
        cfg.update(override)
        cfg['resetFfc'] = 1 if reset else 0
        ev('Object.assign(window.__TUNE, %s); 1' % json.dumps(cfg))
        # 预热
        n0 = ev('window.__diag ? window.__diag.n : 0') or 0
        warm = 22 if reset else WARM
        t0 = time.time()
        while time.time() - t0 < 90:
            time.sleep(0.4)
            n = ev('window.__diag ? window.__diag.n : 0') or 0
            if n - n0 >= warm:
                break
        # 采样 NFR 帧
        samples = []
        last = ev('window.__diag ? window.__diag.n : 0') or 0
        t0 = time.time()
        while len(samples) < NFR and time.time() - t0 < 90:
            time.sleep(0.35)
            n = ev('window.__diag ? window.__diag.n : 0') or 0
            if n > last:
                last = n
                v = ev('JSON.stringify(window.__diag)')
                if v:
                    try:
                        samples.append(json.loads(v))
                    except Exception:
                        pass
        if not samples:
            print('%-26s 无数据' % name, flush=True)
            return None
        rf = sum(s['raw']['flat'] for s in samples) / len(samples)
        of = sum(s['out']['flat'] for s in samples) / len(samples)
        sh = sum(s['out']['sharp'] for s in samples) / len(samples)
        rs = sum(s['raw']['sharp'] for s in samples) / len(samples)
        mad = sum(s['mad'] for s in samples) / len(samples)
        gain = (1 - of / rf) * 100 if rf else 0
        print('%-26s %8.3f %8.3f %+7.1f%% %8.2f %8.2f %7.2f' %
              (name, rf, of, gain, sh, rs, mad), flush=True)
        return dict(name=name, raw=rf, out=of, gain=gain, sharp=sh, rawsharp=rs, mad=mad)

    results = []
    for name, ov in GROUPS:
        r = run_group(name, ov)
        if r:
            results.append(r)
    for name, ov in FFC_GROUPS:
        r = run_group(name, ov, reset=True)
        if r:
            results.append(r)
    with open(os.path.join(HERE, OUT), 'w', encoding='utf-8') as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    print('已保存 %s' % OUT, flush=True)
    ws.close(); proc.terminate(); httpd.shutdown()


if __name__ == '__main__':
    main()

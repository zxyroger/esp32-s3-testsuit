# -*- coding: utf-8 -*-
"""在真实浏览器里点一次“发送”，验证网页 -> 板子的命令通道是否真的通。

用法：python shot_cdp_send.py <url> <输出png> <命令>
"""
import base64
import json
import os
import subprocess
import sys
import time
import urllib.request

import websocket

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
PORT = 9334


def main():
    url, out, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    w, h = 492, 900
    profile = os.path.join(os.environ.get("TEMP", r"D:\s3demo"), "edge-cdp-profile2")
    proc = subprocess.Popen([
        EDGE, "--headless=new", "--disable-gpu", "--no-sandbox",
        "--hide-scrollbars", "--mute-audio",
        "--remote-debugging-port=%d" % PORT,
        "--user-data-dir=%s" % profile,
        "--window-size=%d,%d" % (w, h), url,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ws_url = None
    for _ in range(40):
        time.sleep(0.25)
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT, timeout=1) as r:
                for t in json.load(r):
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        ws_url = t["webSocketDebuggerUrl"]
        except Exception:
            continue
        if ws_url:
            break

    ws = websocket.create_connection(ws_url, timeout=30)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({"id": mid[0], "method": method, "params": params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get("id") == mid[0]:
                return msg.get("result", {})

    def js(expr):
        r = call("Runtime.evaluate", expression=expr, returnByValue=True,
                 awaitPromise=True)
        return r.get("result", {}).get("value")

    call("Page.enable")
    call("Runtime.enable")
    call("Emulation.setDeviceMetricsOverride", width=w, height=h,
         deviceScaleFactor=1, mobile=True)
    time.sleep(10)

    print("状态:", js("document.getElementById('serState').textContent"))
    js("document.getElementById('cmd').value=%s;" % json.dumps(cmd))
    js("document.getElementById('send').click(); 1")
    time.sleep(8)
    print("终端内容(前 400 字):")
    print((js("document.getElementById('term').textContent") or "")[:400])

    shot = call("Page.captureScreenshot", format="png")
    with open(out, "wb") as f:
        f.write(base64.b64decode(shot["data"]))
    ws.close()
    proc.terminate()
    print("已保存", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

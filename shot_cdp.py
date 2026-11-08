# -*- coding: utf-8 -*-
"""用 CDP 在真实时间里等页面加载完，再截图（--virtual-time-budget 会卡住 WebSocket）。

用法：python shot_cdp.py <url> <输出png> [等待秒数] [宽] [高]
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
PORT = 9333


def main():
    url = sys.argv[1]
    out = sys.argv[2]
    wait = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
    w = int(sys.argv[4]) if len(sys.argv) > 4 else 492
    h = int(sys.argv[5]) if len(sys.argv) > 5 else 900

    profile = os.path.join(os.environ.get("TEMP", r"D:\s3demo"), "edge-cdp-profile")
    proc = subprocess.Popen([
        EDGE, "--headless=new", "--disable-gpu", "--no-sandbox",
        "--hide-scrollbars", "--mute-audio",
        "--remote-debugging-port=%d" % PORT,
        "--user-data-dir=%s" % profile,
        "--window-size=%d,%d" % (w, h),
        url,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ws_url = None
    for _ in range(40):
        time.sleep(0.25)
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT, timeout=1) as r:
                tabs = json.load(r)
            for t in tabs:
                if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                    ws_url = t["webSocketDebuggerUrl"]
                    break
            if ws_url:
                break
        except Exception:
            continue

    if not ws_url:
        proc.terminate()
        print("CDP 连接失败")
        return 1

    ws = websocket.create_connection(ws_url, timeout=30)
    mid = [0]

    def call(method, **params):
        mid[0] += 1
        ws.send(json.dumps({"id": mid[0], "method": method, "params": params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get("id") == mid[0]:
                return msg.get("result", {})

    call("Page.enable")
    call("Emulation.setDeviceMetricsOverride", width=w, height=h,
         deviceScaleFactor=1, mobile=True)
    time.sleep(wait)
    shot = call("Page.captureScreenshot", format="png")
    with open(out, "wb") as f:
        f.write(base64.b64decode(shot["data"]))
    ws.close()
    proc.terminate()
    print("已保存 %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

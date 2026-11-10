# -*- coding: utf-8 -*-
"""gain_ab: 增益上限 A/B/C/A 交错抓帧，排除场景随时间变化的影响。"""
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from day2night import AES, TOPIC, open_sub, read_exact, read_remlen  # noqa: E402


def set_gain(g):
    subprocess.run([sys.executable, "serial_flow.py", "cam gain %d" % g],
                   capture_output=True, timeout=90)


def grab_retry(n=4, tries=4):
    for k in range(tries):
        try:
            r = grab(n)
            if len(r) >= max(2, n - 1):
                return r
        except Exception as e:
            print("  retry %d: %s" % (k + 1, type(e).__name__), flush=True)
            time.sleep(3)
    return []


def grab(n=4):
    s = open_sub()
    out, t = [], time.time()
    while len(out) < n and time.time() - t < 60:
        h0 = read_exact(s, 1)
        ln = read_remlen(s)
        body = read_exact(s, ln)
        if (h0[0] >> 4) == 3:
            tl = int.from_bytes(body[:2], "big")
            pl = body[2 + tl:]
            try:
                out.append(AES.decrypt(pl[2:14], pl[14:], TOPIC.encode()))
            except Exception:
                pass
    s.close()
    return out


def stats(jpg):
    import io
    a = np.asarray(Image.open(io.BytesIO(jpg)).convert("RGB"), dtype=np.float32)
    L = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    gx = np.abs(np.diff(L, axis=1)) / 2
    gy = np.abs(np.diff(L, axis=0)) / 2
    g = gx[:-1, :] + gy[:, :-1]
    acut = float(np.percentile(g, 98))
    lap = np.abs(L[1:-1, 1:-1] - (L[:-2, 1:-1] + L[2:, 1:-1] + L[1:-1, :-2] + L[1:-1, 2:]) / 4)
    m = L[1:-1, 1:-1] < 60
    noise = float(np.median(lap[m])) if m.sum() > 100 else float("nan")
    return L.mean(), acut, noise


def main():
    plan = [("A1 4x", 4), ("B 8x", 8), ("C 16x", 16), ("A2 4x", 4)]
    res = {}
    for name, g in plan:
        set_gain(g)
        time.sleep(8)
        fs = grab_retry(4)
        if not fs:
            print("%-7s 抓帧失败" % name, flush=True)
            continue
        arr = np.array([stats(f) for f in fs])
        res[name] = arr
        print("%-7s mean=%5.1f acut=%5.2f noise=%4.2f (n=%d)"
              % (name, arr[:, 0].mean(), arr[:, 1].mean(), arr[:, 2].mean(), len(fs)), flush=True)
    if "A1 4x" in res and "A2 4x" in res:
        print("\n对比 A1/A2 判断漂移: acut %.2f -> %.2f"
              % (res["A1 4x"][:, 1].mean(), res["A2 4x"][:, 1].mean()))


if __name__ == "__main__":
    main()

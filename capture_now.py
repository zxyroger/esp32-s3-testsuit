# -*- coding: utf-8 -*-
"""capture_now: 抓 N 帧原始 JPEG 存盘（只订阅，不打扰板子）。"""
import os
import sys
import time

import numpy as np
from PIL import Image
import io

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from day2night import open_sub, read_exact, read_remlen  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 4
OUT = sys.argv[2] if len(sys.argv) > 2 else "frames_now"
os.makedirs(OUT, exist_ok=True)

s = open_sub()
got = 0
t0 = time.time()
while got < N and time.time() - t0 < 90:
    h0 = read_exact(s, 1)
    ln = read_remlen(s)
    body = read_exact(s, ln)
    typ = h0[0] >> 4
    if typ == 3:
        tl = int.from_bytes(body[:2], "big")
        payload = body[2 + tl:]
        if payload[:2] == b"\xff\xd8":
            p = os.path.join(OUT, "cap_%d.jpg" % got)
            open(p, "wb").write(payload)
            print("saved", p, len(payload))
            got += 1
    elif typ == 12:
        # PINGREQ 不需要回（服务器发的是 PINGRESP type 13）
        pass
s.close()

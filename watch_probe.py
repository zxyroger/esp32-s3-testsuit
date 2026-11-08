#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""盯着串口，把外部请求来源记到 net_probe.log。

为什么要它：手机 4G 打进来失败时，只有板子自己知道包有没有到。
有 "来源" 行 = 包到了板子，问题在回程；没有 = 拦截在板子之前。

用法：python watch_probe.py [秒数]   默认 600 秒
"""
import serial
import sys
import time

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 600
OUT = r"D:\s3demo\net_probe.log"

s = serial.Serial("COM14", 115200, timeout=0.5)
s.reset_input_buffer()
t0 = time.time()
buf = b""

with open(OUT, "a", encoding="utf-8") as f:
    f.write("=== 开始监听 %s ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    f.flush()
    while time.time() - t0 < DUR:
        d = s.read(4096)
        if not d:
            continue
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf8", "replace").strip()
            if "来源" in txt or "cam_web" in txt or "ddns" in txt:
                f.write("%s  %s\n" % (time.strftime("%H:%M:%S"), txt))
                f.flush()
s.close()

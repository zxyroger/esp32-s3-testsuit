#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""统计板子推上来的 JPEG 帧率，判断板子是否还在运行。

用途：拔掉 USB 后如果串口断了、但这里还在出帧，就证明板子是靠电池在跑。
用法：python frames_watch.py [秒数]
"""
import socket
import ssl
import sys
import time

from lab_conf import HOST, PORT, BASE     # broker 与主题前缀来自 viewer/secrets.js

TOPIC = BASE + "/jpg"
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 150


def enc_remlen(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def pkt_str(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        d = sock.recv(n - len(buf))
        if not d:
            raise ConnectionError("peer closed")
        buf += d
    return buf


def read_remlen(sock):
    mult, val = 1, 0
    while True:
        b = read_exact(sock, 1)[0]
        val += (b & 127) * mult
        if not b & 0x80:
            return val
        mult *= 128


ctx = ssl.create_default_context()
sock = ctx.wrap_socket(socket.create_connection((HOST, PORT), timeout=15),
                       server_hostname=HOST)

vh = pkt_str("MQTT") + bytes([4, 0x02]) + (60).to_bytes(2, "big")
pl = pkt_str("s3-frames-%d" % (int(time.time()) % 100000))
body = vh + pl
sock.sendall(bytes([0x10]) + enc_remlen(len(body)) + body)
hdr = read_exact(sock, 1)
read_exact(sock, read_remlen(sock))
if hdr[0] != 0x20:
    print("CONNECT 失败")
    sys.exit(1)

body = (1).to_bytes(2, "big") + pkt_str(TOPIC) + bytes([0])
sock.sendall(bytes([0x82]) + enc_remlen(len(body)) + body)
hdr = read_exact(sock, 1)
read_exact(sock, read_remlen(sock))
print("[frames] 已订阅 %s，统计 %d 秒" % (TOPIC, DUR), flush=True)

sock.settimeout(1.0)
start = time.time()
win = start
last = start
cnt = 0
total = 0
gap = 0.0
worst = 0.0
while time.time() - start < DUR:
    try:
        h = sock.recv(1)
        if not h:
            break
        if (h[0] & 0xF0) != 0x30:
            n = read_remlen(sock)
            read_exact(sock, n)
            continue
        n = read_remlen(sock)
        read_exact(sock, n)
        now = time.time()
        d = now - last
        if cnt > 0:
            worst = max(worst, d)
        last = now
        cnt += 1
        total += 1
    except socket.timeout:
        pass
    if time.time() - last > 3.0:
        gap = time.time() - last
        print("[frames] 断流 %.1f 秒（无新帧）" % gap, flush=True)
        last = time.time()
    el = time.time() - win
    if el >= 5:
        print("[frames] t=%3.0fs  本段 %2d 帧 (%.1f fps)  累计 %d"
              % (time.time() - start, cnt, cnt / el, total), flush=True)
        cnt = 0
        win = time.time()

print("[frames] 结束，累计 %d 帧，最长帧间隔 %.2f s" % (total, worst), flush=True)
sock.close()

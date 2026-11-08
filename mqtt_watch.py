#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""订阅 MQTT topic，把收到的 JPEG 帧存下来看看板子到底推没推上来。

不依赖 paho：直接手写 MQTT 3.1.1 的 CONNECT / SUBSCRIBE，读 PUBLISH。
用法：python mqtt_watch.py [秒数] [topic]
"""
import socket
import ssl
import sys
import time

from lab_conf import HOST, PORT, BASE     # broker 与主题前缀来自 viewer/secrets.js

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 20
TOPIC = sys.argv[2] if len(sys.argv) > 2 else BASE + "/jpg"
OUT = r"D:\s3demo\mqtt_frame.jpg"


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
        if mult > 128 ** 3:
            raise ValueError("bad remaining length")


ctx = ssl.create_default_context()
raw = socket.create_connection((HOST, PORT), timeout=10)
s = ctx.wrap_socket(raw, server_hostname=HOST)

# CONNECT
cid = "watch-%d" % int(time.time() % 100000)
vh = pkt_str("MQTT") + b"\x04\x02\x00\x3c" + pkt_str(cid)
s.sendall(b"\x10" + enc_remlen(len(vh)) + vh)
h = read_exact(s, 1)[0]
rl = read_remlen(s)
body = read_exact(s, rl)
print("CONNACK type=0x%02x rc=%d" % (h, body[1] if len(body) > 1 else -1))

# SUBSCRIBE
vh = b"\x00\x01" + pkt_str(TOPIC) + b"\x00"
s.sendall(b"\x82" + enc_remlen(len(vh)) + vh)
h = read_exact(s, 1)[0]
rl = read_remlen(s)
read_exact(s, rl)
print("SUBACK type=0x%02x topic=%s" % (h, TOPIC))

t0 = time.time()
frames = 0
total = 0
last = b""
while time.time() - t0 < DUR:
    s.settimeout(DUR)
    h = read_exact(s, 1)[0]
    rl = read_remlen(s)
    body = read_exact(s, rl)
    if h >> 4 != 3:          # 只处理 PUBLISH
        continue
    qos = (h >> 1) & 3
    tlen = int.from_bytes(body[:2], "big")
    payload = body[2 + tlen:]
    if qos:
        payload = payload[:-2]
    frames += 1
    total += len(payload)
    last = payload
    if frames <= 3 or frames % 5 == 0:
        print("  frame#%d %d bytes  head=%s" % (frames, len(payload), payload[:4].hex()))

s.close()
print("收到 %d 帧，共 %.1f KB，平均 %.1f KB/帧" % (frames, total / 1024, (total / frames / 1024) if frames else 0))
if last:
    with open(OUT, "wb") as f:
        f.write(last)
    print("最后一帧已存:", OUT, "SOI ok:", last[:2] == b"\xff\xd8")

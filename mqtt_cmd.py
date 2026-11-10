#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""通过 MQTT 给板子发串口命令并收集回显（桥接在跑时用）。

用法：python mqtt_cmd.py "batt" [等待秒数]
不依赖 paho，手写 MQTT 3.1.1 的 CONNECT/SUBSCRIBE/PUBLISH。
"""
import socket
import ssl
import sys
import time

from lab_conf import HOST, PORT, BASE     # broker 与主题前缀来自 viewer/secrets.js

CMD = sys.argv[1] if len(sys.argv) > 1 else "help"
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0


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


ctx.maximum_version = ssl.TLSVersion.TLSv1_2  # 本机出口会重置 TLS 1.3 的 ClientHello，强制 1.2 才能握手
raw = socket.create_connection((HOST, PORT), timeout=10)
sock = ctx.wrap_socket(raw, server_hostname=HOST)

# CONNECT
vh = pkt_str("MQTT") + bytes([4, 0x02]) + (30).to_bytes(2, "big")
pl = pkt_str("s3-cmd-%d" % (int(time.time()) % 100000))
body = vh + pl
sock.sendall(bytes([0x10]) + enc_remlen(len(body)) + body)
hdr = read_exact(sock, 1)
n = read_remlen(sock)
read_exact(sock, n)
if hdr[0] != 0x20:
    print("CONNECT 失败", hdr.hex())
    sys.exit(1)
print("[mqtt] connected to %s" % HOST)

# SUBSCRIBE serial/tx（板子输出） + serial/st（桥状态）
topics = [BASE + "/serial/tx", BASE + "/serial/st"]
body = (len(topics)).to_bytes(2, "big")
for t in topics:
    body += pkt_str(t) + bytes([0])
sock.sendall(bytes([0x82]) + enc_remlen(len(body)) + body)
hdr = read_exact(sock, 1)
n = read_remlen(sock)
read_exact(sock, n)
print("[mqtt] subscribed %s" % ", ".join(topics))

# PUBLISH 命令到 serial/rx（网页 -> 板子）
topic = BASE + "/serial/rx"
payload = (CMD + "\r\n").encode()
body = pkt_str(topic) + payload
sock.sendall(bytes([0x30]) + enc_remlen(len(body)) + body)
print("[mqtt] >> %s" % CMD)
sock.settimeout(0.5)

# 读 PUBLISH
t0 = time.time()
while time.time() - t0 < DUR:
    try:
        h = sock.recv(1)
        if not h:
            break
        if (h[0] & 0xF0) != 0x30:
            n = read_remlen(sock)
            read_exact(sock, n)
            continue
        n = read_remlen(sock)
        data = read_exact(sock, n)
        tl = int.from_bytes(data[:2], "big")
        t = data[2:2 + tl].decode("utf8", "replace")
        p = data[2 + tl:]
        if t.endswith("/tx"):
            sys.stdout.write(p.decode("utf8", "replace"))
            sys.stdout.flush()
        elif t.endswith("/st"):
            print("[bridge] " + p.decode("utf8", "replace"))
    except socket.timeout:
        continue
sock.close()
print("\n[mqtt] done")

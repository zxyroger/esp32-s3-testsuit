#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""从 PC 侧验证板子的 MQTT 命令通道（不经过串口桥接，载荷为密文）。

用法：python mqtt_rcmd.py "batt" [等待秒数]
      python mqtt_rcmd.py            # 不带参数只监听 6 秒

线格式与固件 main/sec_crypto.c 一致：
    [0]=0x01  [1]=0x00  [2..13]=IV  [14..]=密文‖16B tag
AAD 用 topic 本身。
"""
import os
import socket
import ssl
import sys
import time

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from lab_conf import HOST, PORT, BASE, key_bytes   # 全部来自 viewer/secrets.js

CMD = sys.argv[1] if len(sys.argv) > 1 else ""
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

KEY = key_bytes()
AES = AESGCM(KEY)


def seal(aad, data):
    iv = os.urandom(12)
    return b"\x01\x00" + iv + AES.encrypt(iv, data, aad.encode())


def open_pkt(aad, buf):
    if len(buf) < 30 or buf[0] != 0x01:
        return None
    try:
        return AES.decrypt(buf[2:14], buf[14:], aad.encode())
    except Exception:
        return None


def enc(n):
    o = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        o.append(b)
        if not n:
            return bytes(o)


def ps(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def rx(sk, n):
    b = b""
    while len(b) < n:
        d = sk.recv(n - len(b))
        if not d:
            raise ConnectionError("closed")
        b += d
    return b


def rl(sk):
    m, v = 1, 0
    while True:
        b = rx(sk, 1)[0]
        v += (b & 127) * m
        if not b & 0x80:
            return v
        m *= 128


ctx = ssl.create_default_context()
sk = ctx.wrap_socket(socket.create_connection((HOST, PORT), timeout=10),
                     server_hostname=HOST)

body = ps("MQTT") + bytes([4, 0x02]) + (60).to_bytes(2, "big") + ps("pcrx%d" % (int(time.time()) % 10000))
sk.sendall(bytes([0x10]) + enc(len(body)) + body)
rx(sk, 1)
rx(sk, rl(sk))

T_TX = BASE + "/serial/tx"
T_CHAL = BASE + "/chal"
T_CMD = BASE + "/cmd"
for i, topic in enumerate((T_TX, T_CHAL), start=1):
    sub = i.to_bytes(2, "big") + ps(topic) + bytes([0])
    sk.sendall(bytes([0x82]) + enc(len(sub)) + sub)
    rx(sk, 1)
    rx(sk, rl(sk))
print("[rcmd] 已订阅 %s / %s" % (T_TX, T_CHAL), flush=True)

sk.settimeout(1.0)
chal = None
started = time.time()
sent = False
last = time.time()

while time.time() - started < DUR:
    try:
        h = sk.recv(1)
        if not h:
            break
        if (h[0] & 0xF0) != 0x30:
            rx(sk, rl(sk))
            continue
        n = rl(sk)
        d = rx(sk, n)
        tl = int.from_bytes(d[:2], "big")
        topic = d[2:2 + tl].decode()
        pt = open_pkt(topic, d[2 + tl:])
        if pt is None:
            print("[rcmd] %s 解密失败" % topic, flush=True)
            continue
        if topic.endswith("/chal"):
            chal = pt.decode(errors="replace").strip()
            if CMD and not sent:
                payload = seal(T_CMD, ("c=%s|%s" % (chal, CMD)).encode())
                pub = ps(T_CMD) + payload
                sk.sendall(bytes([0x30]) + enc(len(pub)) + pub)
                print("[rcmd] 已发送命令: %s（挑战码 %s）" % (CMD, chal), flush=True)
                sent = True
            continue
        txt = pt.decode("utf-8", "replace")
        sys.stdout.write(txt if txt.endswith("\n") else txt + "\n")
        sys.stdout.flush()
        last = time.time()
    except socket.timeout:
        if time.time() - last >= 20:
            sk.sendall(b"\xc0\x00")
            last = time.time()

if CMD and not sent:
    print("[rcmd] 没等到挑战码，命令未发送", flush=True)
sk.close()
print("[rcmd] 结束", flush=True)

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""从 MQTT 抓加密帧、解密存盘，用来做离线分析。

用法：python grab_frame.py [帧数] [输出目录]
默认抓 6 帧到 D:\s3demo\frames\
"""
import os
import socket
import ssl
import sys
import time

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from lab_conf import HOST, PORT, BASE, key_bytes

N = int(sys.argv[1]) if len(sys.argv) > 1 else 6
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else r"D:\s3demo\frames"
TOPIC = BASE + "/jpg"

AES = AESGCM(key_bytes())


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


def open_sub(topic, cid):
    ctx = ssl.create_default_context()
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    raw = socket.create_connection((HOST, PORT), timeout=15)
    s = ctx.wrap_socket(raw, server_hostname=HOST)

    vh = pkt_str("MQTT") + b"\x04\x02\x00\x3c" + pkt_str(cid)
    s.sendall(b"\x10" + enc_remlen(len(vh)) + vh)
    read_exact(s, 1)
    read_exact(s, read_remlen(s))

    vh = b"\x00\x01" + pkt_str(topic) + b"\x00"
    s.sendall(b"\x82" + enc_remlen(len(vh)) + vh)
    read_exact(s, 1)
    read_exact(s, read_remlen(s))
    return s


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    s = open_sub(TOPIC, "grab%d" % (int(time.time()) % 100000))
    print("已订阅", TOPIC, "，等待", N, "帧…")

    got = 0
    t0 = time.time()
    while got < N and time.time() - t0 < 90:
        s.settimeout(30)
        h = read_exact(s, 1)[0]
        rl = read_remlen(s)
        body = read_exact(s, rl)
        if h >> 4 != 3:
            continue
        tlen = int.from_bytes(body[:2], "big")
        payload = body[2 + tlen:]
        if (h >> 1) & 3:
            payload = payload[:-2]

        if payload[:2] != b"\x01\x00":
            print("  非加密载荷，跳过", payload[:4].hex())
            continue
        try:
            jpg = AES.decrypt(payload[2:14], payload[14:], TOPIC.encode())
        except Exception as e:
            print("  解密失败:", e)
            continue
        got += 1
        path = os.path.join(OUTDIR, "cam_%02d.jpg" % got)
        with open(path, "wb") as f:
            f.write(jpg)
        print("  %s  %d B  t=%.2fs  SOI=%s" %
              (path, len(jpg), time.time() - t0, jpg[:2] == b"\xff\xd8"))

    s.close()
    print("完成，共 %d 帧 -> %s" % (got, OUTDIR))


if __name__ == "__main__":
    main()

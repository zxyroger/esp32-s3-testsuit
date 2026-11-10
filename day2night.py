#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""跨光照连续采样：从下午一直采到天黑，记录每帧的亮度/横纹/锐度/帧间隔。

只订阅、不发命令、不碰串口，绝不打扰板子。断线自动重连。
用法： python day2night.py [结束HH:MM] [存帧间隔秒] [输出目录]
例：   python day2night.py 18:30 20 D:/s3demo/frames_day2night
"""
import csv
import io
import os
import socket
import ssl
import sys
import time
from datetime import datetime, date

import numpy as np
from PIL import Image
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from lab_conf import HOST, PORT, BASE, key_bytes

TOPIC = BASE + "/jpg"
AES = AESGCM(key_bytes())


def parse_end(hm):
    end_h, end_m = [int(v) for v in hm.split(":")]
    _t = date.today()
    return datetime(_t.year, _t.month, _t.day, end_h, end_m).timestamp()


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


def open_sub():
    ctx = ssl.create_default_context()
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    raw = socket.create_connection((HOST, PORT), timeout=15)
    s = ctx.wrap_socket(raw, server_hostname=HOST)
    s.settimeout(30)
    vh = pkt_str("MQTT") + b"\x04\x02\x00\x3c" + pkt_str("d2n%d" % (int(time.time()) % 100000))
    s.sendall(b"\x10" + enc_remlen(len(vh)) + vh)
    read_exact(s, 1)
    read_exact(s, read_remlen(s))
    vh = b"\x00\x01" + pkt_str(TOPIC) + b"\x00"
    s.sendall(b"\x82" + enc_remlen(len(vh)) + vh)
    read_exact(s, 1)
    read_exact(s, read_remlen(s))
    return s


def metrics(jpg):
    """与前端 cam_view_check.py 完全一致的 flatBand / sharp / band 定义。"""
    im = Image.open(io.BytesIO(jpg)).convert("RGB")
    a = np.asarray(im, dtype=np.float32)
    lum = 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]
    h, w = lum.shape
    rm = lum.mean(axis=1)
    k = 10
    lp = np.array([rm[max(0, y - k):min(h, y + k + 1)].mean() for y in range(h)])
    hp = rm - lp
    band = float(hp.std())

    BW = BH = 80
    blocks = []
    for by in range(0, h - BH + 1, BH):
        for bx in range(0, w - BW + 1, BW):
            blk = lum[by:by + BH, bx:bx + BW]
            blocks.append((float(blk.std()), by, bx))
    blocks.sort()
    if not blocks:
        return dict(mean=float(lum.mean()), std=float(lum.std()), flatBand=0,
                    sharp=0, band=band, dark=0, w=w, h=h)

    hpv = []
    for _, by, bx in blocks[:10]:
        rr = lum[by:by + BH, bx:bx + BW].mean(axis=1)
        kk = 8
        sm = np.array([rr[max(0, y - kk):min(BH, y + kk + 1)].mean() for y in range(BH)])
        hpv.extend((rr - sm).tolist())
    flatBand = float(np.std(hpv))

    g = []
    for _, by, bx in blocks[-10:]:
        blk = lum[by:by + BH, bx:bx + BW]
        g.append(float(np.abs(np.diff(blk, axis=1)).mean() +
                       np.abs(np.diff(blk, axis=0)).mean()))
    sharp = float(np.mean(g))
    return dict(mean=float(lum.mean()), std=float(lum.std()), flatBand=flatBand,
                sharp=sharp, band=band, dark=float((lum < 60).mean()), w=w, h=h)


def main():
    END_HM = sys.argv[1] if len(sys.argv) > 1 else "18:30"
    SAVE_EVERY = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    OUTDIR = sys.argv[3] if len(sys.argv) > 3 else r"D:\s3demo\frames_day2night"
    END_EPOCH = parse_end(END_HM)
    os.makedirs(OUTDIR, exist_ok=True)
    csv_path = os.path.join(OUTDIR, "metrics.csv")
    new = not os.path.exists(csv_path)
    fcsv = open(csv_path, "a", newline="", encoding="utf-8")
    wr = csv.writer(fcsv)
    if new:
        wr.writerow(["time", "epoch", "bytes", "dt", "w", "h", "mean", "std",
                     "flatBand", "sharp", "band", "dark", "saved"])
        fcsv.flush()
    print("采样启动 -> %s，结束 %s，每 %.0fs 存帧，输出 %s" %
          (datetime.now().strftime("%H:%M:%S"), END_HM, SAVE_EVERY, OUTDIR), flush=True)

    last_save = 0.0
    last_frame = 0.0
    last_ping = 0.0
    n = 0
    sock = None
    while time.time() < END_EPOCH:
        if sock is None:
            try:
                sock = open_sub()
                last_ping = time.time()
                print("[%s] 已订阅" % datetime.now().strftime("%H:%M:%S"), flush=True)
            except Exception as e:
                print("连接失败 %s，5s 后重试" % e, flush=True)
                time.sleep(5)
                continue
        try:
            hd = read_exact(sock, 1)[0]
            rl = read_remlen(sock)
            body = read_exact(sock, rl)
        except Exception as e:
            print("[%s] 链路断开 %s，重连" % (datetime.now().strftime("%H:%M:%S"), e), flush=True)
            try:
                sock.close()
            except Exception:
                pass
            sock = None
            time.sleep(2)
            continue
        if hd >> 4 != 3:
            continue
        tl = int.from_bytes(body[:2], "big")
        payload = body[2 + tl:]
        if (hd >> 1) & 3:
            payload = payload[:-2]
        if payload[:2] != b"\x01\x00":
            continue
        try:
            jpg = AES.decrypt(payload[2:14], payload[14:], TOPIC.encode())
        except Exception:
            continue
        if jpg[:2] != b"\xff\xd8":
            continue

        now = time.time()
        # CONNECT 里 keepalive=60s，不发 PINGREQ 会被 broker 定时踢掉，
        # 造成约每 90s 一次重连 + 2s 空窗，污染帧间隔统计。25s 发一次心跳。
        if now - last_ping >= 25:
            try:
                sock.sendall(b"\xc0\x00")
                last_ping = now
            except Exception:
                pass
        dt = now - last_frame if last_frame else 0
        last_frame = now
        n += 1
        try:
            m = metrics(jpg)
        except Exception as e:
            print("解码失败", e, flush=True)
            continue
        saved = ""
        if now - last_save >= SAVE_EVERY:
            last_save = now
            ts = datetime.now().strftime("%H%M%S")
            saved = "f_%s_%03d.jpg" % (ts, n)
            with open(os.path.join(OUTDIR, saved), "wb") as f:
                f.write(jpg)
        wr.writerow([datetime.now().strftime("%H:%M:%S"), "%.1f" % now, len(jpg),
                     "%.2f" % dt, m["w"], m["h"], "%.2f" % m["mean"], "%.2f" % m["std"],
                     "%.3f" % m["flatBand"], "%.3f" % m["sharp"], "%.3f" % m["band"],
                     "%.3f" % m["dark"], saved])
        fcsv.flush()
        print("[%s] #%d %dx%d %dB dt=%.2fs L=%.1f flat=%.3f sharp=%.2f band=%.2f dark=%.2f %s" %
              (datetime.now().strftime("%H:%M:%S"), n, m["w"], m["h"], len(jpg), dt,
               m["mean"], m["flatBand"], m["sharp"], m["band"], m["dark"], saved), flush=True)
    fcsv.close()
    print("采样结束，共 %d 帧" % n, flush=True)


if __name__ == "__main__":
    main()

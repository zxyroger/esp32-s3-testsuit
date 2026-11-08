#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""电池充放电周期记录器（PC 侧，走 MQTT，不碰串口）。

用途：做「拔掉 USB → 靠电池跑 → 插回 → 充电」这个循环时，
      把板子每 3 秒上报的电池数据全程录下来，并在时间线上标出
      掉线 / 恢复 / 重启三种事件。

判据（关键）：
  板子上的 up = 上电以来的秒数。插回 USB 之后看这个值——
    · up 接着往下走（比掉线前大一点）→ 板子没断电，电池真的供住了
    · up 变成个位数（重新从 0 开始）→ 板子被断电重启，电池没进电路
  这一条比看电压可靠得多，因为 VBAT 现在被系统轨拉高，读数不可信。

用法：
  python batt_cycle.py            # 默认录 20 分钟
  python batt_cycle.py 600        # 录 10 分钟
"""
import socket
import ssl
import sys
import time
import json
import datetime

from lab_conf import HOST, PORT, BASE     # broker 与主题前缀来自 viewer/secrets.js

TOPIC = BASE + "/batt"

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 1200.0
GAP_WARN = 10.0          # 超过这么久没数据就开始提示
PING_EVERY = 20.0        # MQTT keepalive=30 s，必须自己发 PINGREQ，否则 broker 会踢
CSV = "D:/s3demo/batt_cycle_%s.csv" % time.strftime("%Y%m%d_%H%M%S")


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


def connect():
    ctx = ssl.create_default_context()
    raw = socket.create_connection((HOST, PORT), timeout=10)
    sk = ctx.wrap_socket(raw, server_hostname=HOST)
    cid = "s3cyc%d" % (int(time.time() * 1000) % 1000000)
    body = pkt_str("MQTT") + bytes([4, 0x02]) + (60).to_bytes(2, "big") + pkt_str(cid)
    sk.sendall(bytes([0x10]) + enc_remlen(len(body)) + body)
    read_exact(sk, 1)
    read_exact(sk, read_remlen(sk))
    sub = (1).to_bytes(2, "big") + pkt_str(TOPIC) + bytes([0])
    sk.sendall(bytes([0x82]) + enc_remlen(len(sub)) + sub)
    read_exact(sk, 1)
    read_exact(sk, read_remlen(sk))
    sk.settimeout(1.0)
    return sk


def main():
    print("=" * 74, flush=True)
    print("[记录] topic = %s  时长 %.0f 秒" % (TOPIC, DUR), flush=True)
    print("[记录] CSV  -> %s" % CSV, flush=True)
    print("=" * 74, flush=True)

    f = open(CSV, "w", encoding="utf-8")
    f.write("ts,iso,up,vbat,vsys,vbus,chg,dir,pct,st1,st2,bp,vg,event\n")
    f.flush()

    t_start = time.time()
    now = t_start
    last_rx = t_start
    last_ping = t_start
    last_up = None
    warned = False
    vbat_hist = []           # (t, vbat) 用于算变化率
    vmin, vmax = 10 ** 9, -1
    events = []
    samples = 0
    minute_acc = {}          # "HH:MM" -> [sum_vbat, n, min, max]
    sk = None
    errors = 0

    while time.time() - t_start < DUR:
        try:
            if sk is None:
                sk = connect()
                last_ping = time.time()
                print("[记录] MQTT 已连上", flush=True)

            h = sk.recv(1)
            if not h:
                raise ConnectionError("broker closed")
            if (h[0] & 0xF0) != 0x30:      # 不是 PUBLISH（多半是 PINGRESP）
                if (h[0] & 0xF0) != 0xD0:
                    read_exact(sk, read_remlen(sk))
                continue
            n = read_remlen(sk)
            data = read_exact(sk, n)
            tl = int.from_bytes(data[:2], "big")
            topic = data[2:2 + tl].decode()
            if topic != TOPIC:
                continue
            try:
                d = json.loads(data[2 + tl:].decode("utf-8", "replace"))
            except Exception:
                continue

            now = time.time()
            gap = now - last_rx
            up = d.get("up")
            ev = ""

            # —— 数据恢复：用 up 判断这中间板子有没有断电 ——
            if gap > GAP_WARN and last_up is not None and isinstance(up, int):
                if up < last_up - 5:
                    ev = "重启"
                    msg = ("[事件] 中断 %.0f 秒后恢复，up=%s（中断前 %s）→ 板子断电重启了，"
                           "说明这段是靠不了的" % (gap, up, last_up))
                else:
                    ev = "延续"
                    msg = ("[事件] 中断 %.0f 秒后恢复，up=%s（中断前 %s，差 %+d）→ "
                           "板子全程没断电" % (gap, up, last_up, up - last_up))
                events.append("%s  %s" % (time.strftime("%H:%M:%S"), msg))
                print("\n" + msg, flush=True)
                warned = False

            last_rx = now
            samples += 1
            vbat = d.get("vbat")

            iso = datetime.datetime.fromtimestamp(now).isoformat(" ")
            f.write("%.0f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" % (
                now, iso, up, vbat, d.get("vsys"), d.get("vbus"),
                d.get("chg"), d.get("dir"), d.get("pct"),
                d.get("st1"), d.get("st2"), d.get("bp"), d.get("vg"), ev))
            f.flush()

            if isinstance(vbat, int) and 0 < vbat < 6000:
                vmin = min(vmin, vbat)
                vmax = max(vmax, vbat)
                vbat_hist.append((now, vbat))
                if len(vbat_hist) > 400:
                    vbat_hist.pop(0)
                k = time.strftime("%H:%M", time.localtime(now))
                a = minute_acc.setdefault(k, [0, 0, 9999, 0])
                a[0] += vbat
                a[1] += 1
                a[2] = min(a[2], vbat)
                a[3] = max(a[3], vbat)

            rate = ""
            if len(vbat_hist) >= 2:
                t0, v0 = vbat_hist[0]
                if now - t0 >= 30:
                    rate = "  %+.1f mV/min" % ((vbat - v0) * 60.0 / (now - t0))
            print("%s  up=%-6s VBAT %4s  VSYS %4s  VBUS %4s  chg=%s dir=%s %3s%%%s"
                  % (time.strftime("%H:%M:%S"), up, vbat, d.get("vsys"),
                     d.get("vbus"), d.get("chg"), d.get("dir"), d.get("pct"), rate),
                  flush=True)
            if isinstance(up, int):
                last_up = up

        except socket.timeout:
            now = time.time()
            if now - last_ping >= PING_EVERY:
                try:
                    sk.sendall(b"\xc0\x00")
                    last_ping = now
                except Exception:
                    sk = None
                    continue
            if not warned and now - last_rx > GAP_WARN:
                warned = True
                ev2 = "[告警] 已 %.0f 秒收不到电池数据（板子可能已断电）" % (now - last_rx)
                events.append("%s  %s" % (time.strftime("%H:%M:%S"), ev2))
                print("\n" + ev2, flush=True)
                f.write("%.0f,%s,,,,,,,,,,,,无数据\n"
                        % (now, datetime.datetime.fromtimestamp(now).isoformat(" ")))
                f.flush()
            continue
        except Exception as e:
            errors += 1
            print("[记录] 连接异常 %s，重连（第 %d 次）" % (e, errors), flush=True)
            try:
                sk.close()
            except Exception:
                pass
            sk = None
            time.sleep(1)
            continue

    try:
        sk.close()
    except Exception:
        pass
    f.close()

    print("\n" + "=" * 74, flush=True)
    print("[汇总] 样本 %d 条，CSV: %s" % (samples, CSV), flush=True)
    if vmax >= 0:
        print("[汇总] VBAT 区间 %d ~ %d mV" % (vmin, vmax), flush=True)
    if len(minute_acc) >= 2:
        print("[汇总] 按分钟平均 VBAT：", flush=True)
        for k in sorted(minute_acc):
            a = minute_acc[k]
            print("       %s  平均 %4d mV（%d~%d，%d 点）"
                  % (k, a[0] // a[1], a[2], a[3], a[1]), flush=True)
    if events:
        for e in events:
            print("[汇总] " + e, flush=True)
    else:
        print("[汇总] 期间无中断事件", flush=True)
    print("=" * 74, flush=True)


if __name__ == "__main__":
    main()

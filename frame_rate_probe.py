#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""帧到达诊断：订阅 /jpg，记录每条 PUBLISH 的到达时刻/长度/指纹，
用来判断是否有重复帧、多个发布者，以及真实发布速率。

用法: python frame_rate_probe.py [秒数]
"""
import hashlib
import sys
import time
from datetime import datetime

from day2night import open_sub, read_exact, read_remlen, TOPIC

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 35.0


def main():
    s = open_sub()
    s.settimeout(35)
    t0 = time.time()
    hits = []
    print("开始 %s，订阅 %s，持续 %.0fs" %
          (datetime.now().strftime("%H:%M:%S"), TOPIC, DUR), flush=True)
    while time.time() - t0 < DUR:
        try:
            hd = read_exact(s, 1)[0]
            rl = read_remlen(s)
            body = read_exact(s, rl)
        except Exception as e:
            # 读一半超时会破坏 MQTT 帧边界，必须重连而不是继续读旧 socket
            print("  [%6.2fs] 链路异常 %s -> 重连" % (time.time() - t0, e), flush=True)
            try:
                s.close()
            except Exception:
                pass
            s = open_sub()
            s.settimeout(35)
            continue
        if hd >> 4 != 3:
            continue
        tl = int.from_bytes(body[:2], "big")
        topic = body[2:2 + tl].decode("utf8", "replace")
        payload = body[2 + tl:]
        qos = (hd >> 1) & 3
        if qos:
            payload = payload[2:]          # QoS1/2: packet id 在 topic 之后
        hits.append((time.time() - t0, topic, len(payload),
                     hashlib.md5(payload).hexdigest()[:8], qos))
    s.close()
    print("共 %d 条 PUBLISH，耗时 %.1fs，平均 %.2f 条/秒" %
          (len(hits), DUR, len(hits) / DUR), flush=True)
    for i, (t, tp, n, fp, q) in enumerate(hits):
        print("  #%02d t=%6.2fs %-28s %6dB qos=%d fp=%s" %
              (i + 1, t, tp, n, q, fp), flush=True)
    fps = [h for h in hits if h[1] == TOPIC]
    uniq = len(set(h[3] for h in fps))
    print("帧 channel: %d 条, 去重后 %d 条, 重复 %d 条" %
          (len(fps), uniq, len(fps) - uniq), flush=True)
    if len(fps) >= 2:
        dts = [fps[i][0] - fps[i - 1][0] for i in range(1, len(fps))]
        print("帧间隔 min=%.2f max=%.2f avg=%.2f" %
              (min(dts), max(dts), sum(dts) / len(dts)), flush=True)


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""黄昏→入夜定时验证：在多个时间点抓真实帧 + 跑 headless 页面体检，
记录 亮度 / 原始横纹 / 处理后横纹 / 锐度 / 自适应档位，一直采到天黑之后。

用法: python dusk_verify.py   （后台运行，时间表见 TIMES）
输出: dusk_report.csv, dusk_verify.log（stdout 重定向）, frames_dusk_<HHMM>/
"""
import csv
import datetime as dt
import glob
import io
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TIMES = ['17:00', '17:20', '17:40', '18:00', '18:20', '18:36']
REPORT = os.path.join(HERE, 'dusk_report.csv')
PY = sys.executable


def log(s):
    print('[%s] %s' % (dt.datetime.now().strftime('%H:%M:%S'), s), flush=True)


def frame_metrics(path):
    fs = sorted(glob.glob(os.path.join(path, '*.jpg')))[:6]
    out = []
    for f in fs:
        a = np.asarray(Image.open(f).convert('RGB'), np.float32)
        L = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
        r = L.mean(axis=1)
        k = 10
        lp = np.array([r[max(0, y - k):min(len(r), y + k + 1)].mean() for y in range(len(r))])
        bw = bh = 80
        blks = sorted((float(L[y:y + bh, x:x + bw].std()), y, x)
                      for y in range(0, L.shape[0] - bh + 1, bh)
                      for x in range(0, L.shape[1] - bw + 1, bw))
        hp = []
        for _, y, x in blks[:10]:
            rr = L[y:y + bh, x:x + bw].mean(axis=1)
            hp.extend((rr - np.array([rr[max(0, j - 8):min(bh, j + 9)].mean()
                                     for j in range(bh)])).tolist())
        out.append((float(L.mean()), float(np.std(hp)), float((L < 10).mean())))
    if not out:
        return (0, 0, 0, 0)
    return (sum(v[0] for v in out) / len(out),
            sum(v[1] for v in out) / len(out),
            sum(v[2] for v in out) / len(out), len(out))


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True,
                           timeout=timeout, encoding='utf-8', errors='replace')
        return p.stdout + '\n' + p.stderr
    except subprocess.TimeoutExpired as e:
        return (e.stdout or '') + '\nTIMEOUT'


def main():
    new = not os.path.exists(REPORT)
    with open(REPORT, 'a', newline='', encoding='utf-8') as fc:
        w = csv.writer(fc)
        if new:
            w.writerow(['time', 'rawL', 'rawFlat', 'darkRatio', 'nFrames',
                        'pageL', 'pageFlat', 'pageSharp', 'bandDark',
                        'autoL', 'autoT', 'effAlpha', 'effLam', 'effSharp',
                        'diagRawFlat', 'diagOutFlat', 'diagGainPct',
                        'diagRawSharp', 'diagOutSharp', 'fpsNote'])
            fc.flush()
        for hhmm in TIMES:
            h, m = [int(v) for v in hhmm.split(':')]
            now = dt.datetime.now()
            target = dt.datetime(now.year, now.month, now.day, h, m)
            wait = (target - now).total_seconds()
            if wait > 0:
                log('等待至 %s（%.0f 秒）' % (hhmm, wait))
                time.sleep(wait)
            log('=== 开始 %s 采样 ===' % hhmm)
            outdir = os.path.join(HERE, 'frames_dusk_%s' % hhmm.replace(':', ''))
            os.makedirs(outdir, exist_ok=True)
            g = run([PY, 'grab_frame.py', '8', outdir], 150)
            log('抓帧完成: %s' % ('OK' if 'Traceback' not in g else '异常'))
            L, flat, dark, nf = frame_metrics(outdir)
            log('原始帧: L=%.1f flat=%.3f dark=%.3f n=%d' % (L, flat, dark, nf))
            c = run([PY, 'cam_view_check.py', '40', '&diag=1'], 150)
            page = {}
            for line in c.splitlines():
                if line.startswith('画面:'):
                    try:
                        page = json.loads(line.split('画面:', 1)[1].strip())
                    except Exception:
                        pass
            d = page.get('diag') or {}
            a = page.get('auto') or {}
            eff = a.get('eff') or {}
            raw_f = (d.get('raw') or {}).get('flat')
            out_f = (d.get('out') or {}).get('flat')
            gain = (100.0 * (raw_f - out_f) / raw_f) if raw_f and out_f is not None and raw_f else ''
            row = [hhmm, '%.2f' % L, '%.3f' % flat, '%.3f' % dark, nf,
                   page.get('mean'), page.get('flatBand'), page.get('sharp'), page.get('bandDark'),
                   a.get('L'), a.get('t'), eff.get('alpha'), eff.get('lam'), eff.get('sharp'),
                   raw_f, out_f, '%.1f' % gain if gain != '' else '',
                   (d.get('raw') or {}).get('sharp'), (d.get('out') or {}).get('sharp'),
                   'ok' if page else 'no-page']
            w.writerow(row); fc.flush()
            log('页面: L=%s flat=%s sharp=%s auto=%s diag=%s' %
                (page.get('mean'), page.get('flatBand'), page.get('sharp'), a, d))
            log('写入 %s' % REPORT)
    log('全部时间点完成')


if __name__ == '__main__':
    main()

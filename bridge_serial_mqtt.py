#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
串口 <-> MQTT 桥接（PC 侧常驻）

为什么要桥接而不是开隧道：
    手机要直连 PC 的 8765 端口，就必须有公网地址 —— cloudflared 的免费域名
    每次重启都变，还要额外跑一个进程。而板子本来就在用 MQTT 推画面，那条
    TLS 长连接（板子主动外连）已经证明能过关。于是把串口也挂到同一套通道上：
    手机网页 -> broker -> 本脚本 -> COM14 -> 板子。

话题（prefix 默认取 viewer/secrets.js 里的 BASE，与摄像头一致）：
    <prefix>/serial/tx   板子 -> 网页（纯文本，可能一次多行）
    <prefix>/serial/rx   网页 -> 板子（纯文本；末尾没换行也会自动补 \\r\\n）
    <prefix>/serial/st   状态 JSON，retain=1，网页据此判断桥是否在线

用法：
    python bridge_serial_mqtt.py COM14
    python bridge_serial_mqtt.py COM14 --prefix esp32cam/你的前缀 --baud 115200

注意：本脚本会独占总口。要刷机先按 Ctrl+C，刷完再起。
"""
import argparse
import json
import os
import ssl
import sys
import threading
import time

import serial
from paho.mqtt import client as mqtt

from lab_conf import BASE     # 主题前缀来自 viewer/secrets.js

# ---- 全局状态 ----
_args = None
_ser = None
_ser_lock = threading.Lock()
_mq = None
_running = True

HERE = os.path.dirname(os.path.abspath(__file__))
PID_FILE = os.path.join(HERE, 'bridge.pid')
# 刷机前由 flash.ps1 建这个文件：桥接看到就松手，刷完删掉它桥接自动收回
RELEASE_FLAG = os.path.join(HERE, 'bridge.release')


def log(*a):
    print(time.strftime('[%H:%M:%S]'), *a, flush=True)


# ---------------- 串口 ----------------

def open_port(quiet=False):
    """打开串口。已打开就直接返回。失败不抛异常，让调用方决定重试。"""
    global _ser
    with _ser_lock:
        if _ser is not None and _ser.is_open:
            return True
        try:
            _ser = serial.Serial(_args.port, _args.baud, timeout=0.05)
            if not quiet:
                log('串口已打开', _args.port, _args.baud)
            return True
        except Exception as e:
            _ser = None
            if not quiet:
                log('打开串口失败:', e)
            return False


def close_port():
    global _ser
    with _ser_lock:
        if _ser is not None:
            try:
                _ser.close()
            except Exception:
                pass
            _ser = None
            log('串口已释放')


def serial_reader():
    """读串口 -> 攒 80 ms 再发，避免一屏日志变成几百个小报文。"""
    buf = []
    pause = 0.0
    while _running:
        # 有人要刷机：立刻松手，别跟 esptool 抢串口
        if os.path.exists(RELEASE_FLAG):
            if _ser is not None:
                close_port()
                publish_status()
            time.sleep(0.8)
            continue
        if not open_port(quiet=True):
            time.sleep(2.0)
            continue
        try:
            n = _ser.in_waiting
            data = _ser.read(n if n else 1)
        except Exception:
            close_port()
            time.sleep(1.0)
            continue
        if data:
            buf.append(data)
            pause = time.time() + 0.08
        if buf and time.time() >= pause:
            raw = b''.join(buf)
            buf = []
            text = raw.decode('utf-8', 'replace')
            if _mq is not None:
                _mq.publish(_args.prefix + '/serial/tx', text, qos=0)


def serial_write(text):
    if not text:
        return False
    with _ser_lock:
        if _ser is None or not _ser.is_open:
            return False
        try:
            _ser.write(text.encode('utf-8'))
            _ser.flush()
            return True
        except Exception as e:
            log('写串口失败:', e)
            return False


# ---------------- MQTT ----------------

def publish_status():
    if _mq is None:
        return
    st = {
        'ok': True,
        'port': _args.port,
        'baud': _args.baud,
        'open': bool(_ser is not None and _ser.is_open),
        'ts': int(time.time()),
        'host': 'PC',
    }
    _mq.publish(_args.prefix + '/serial/st', json.dumps(st), qos=0, retain=True)


def on_connect(client, userdata, flags, reason_code, properties=None):
    if str(reason_code) not in ('0', 'Success'):
        log('MQTT 连接失败:', reason_code)
        return
    log('MQTT 已连上', _args.host)
    client.subscribe(_args.prefix + '/serial/rx', qos=0)
    publish_status()


def on_disconnect(client, userdata, *args):
    # paho 1.x 回调是 3 个参数，2.x 是 5 个 —— 这个脚本两边都得能跑
    log('MQTT 断开:', args[-1] if args else '?', '（paho 会自动重连）')


def on_message(client, userdata, msg):
    try:
        text = msg.payload.decode('utf-8', 'replace')
    except Exception:
        return
    # 控制指令：网页点「释放串口」时用，方便本地刷机
    cmd = text.strip()
    if cmd == '__close':
        close_port()
        publish_status()
        return
    if cmd == '__open':
        open_port()
        publish_status()
        return
    if not text.endswith('\n'):
        text += '\r\n'
    if serial_write(text):
        # 回显，让网页上看到自己发了什么（板子的回显有时会被日志淹没）
        if _mq is not None:
            _mq.publish(_args.prefix + '/serial/tx', '> ' + cmd + '\r\n', qos=0)
    else:
        log('串口没开，丢弃命令:', cmd[:40])


def mqtt_main():
    global _mq
    # WorkBuddy 的 python 装的是 paho 2.x，而 ESP-IDF 的 venv 里是 1.6.1；
    # 巡守进程用 venv 里的解释器，所以这里必须两种 API 都支持。
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='pc-serial-bridge')
    except AttributeError:
        c = mqtt.Client(client_id='pc-serial-bridge')
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.on_connect = on_connect
    c.on_disconnect = on_disconnect
    c.on_message = on_message
    c.reconnect_delay_set(2, 15)
    _mq = c
    c.connect(_args.host, _args.port_mqtt, keepalive=45)
    c.loop_start()
    while _running:
        time.sleep(10)
        publish_status()


def main():
    global _args, _running
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?', default='COM14')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--host', default='broker.emqx.io')
    ap.add_argument('--port-mqtt', type=int, default=8883)
    ap.add_argument('--prefix', default=BASE)
    _args = ap.parse_args()

    log('桥接启动：', _args.port, '@', _args.baud, '->', _args.host)
    log('话题：', _args.prefix + '/serial/{tx,rx,st}')
    try:
        with open(PID_FILE, 'w', encoding='utf-8') as f:
            f.write(str(os.getpid()))
    except OSError:
        pass
    open_port()

    t = threading.Thread(target=serial_reader, daemon=True)
    t.start()
    try:
        mqtt_main()
    except KeyboardInterrupt:
        log('收到 Ctrl+C，退出')
    except Exception:
        import traceback
        traceback.print_exc()      # 别吞异常：无窗口运行时这是唯一能看到的线索
        raise
    finally:
        _running = False
        try:
            if _mq is not None:
                _mq.publish(_args.prefix + '/serial/st',
                            json.dumps({'ok': False, 'ts': int(time.time())}), retain=True)
                _mq.loop_stop()
        except Exception:
            pass
        close_port()
        try:
            os.remove(PID_FILE)
        except OSError:
            pass


if __name__ == '__main__':
    main()

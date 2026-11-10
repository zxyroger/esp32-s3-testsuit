#!/usr/bin/env python3
"""通过本地串口给板子发一条命令（MQTT 命令通道死锁时的兜底恢复）。
用法: python serial_cmd.py "<命令>" [端口] [波特率]
"""
import sys, time
import serial

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "mqtt on"
    port = sys.argv[2] if len(sys.argv) > 2 else "COM3"
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.5
    s.write_timeout = 2
    try:
        s.open()
    except Exception as e:
        print("串口打开失败:", e)
        return 1

    # 不主动拉 DTR/RTS，避免触发自动复位进入 bootloader
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass

    wait = float(sys.argv[4]) if len(sys.argv) > 4 else 15.0
    print("[serial] 等待 %.0f 秒让板子完成复位/自检/联网..." % wait)
    time.sleep(wait)
    # 清空积压
    try:
        s.reset_input_buffer()
    except Exception:
        pass

    line = cmd.strip() + "\n"
    s.write(line.encode("utf-8"))
    print("[serial] 已发送:", repr(line.strip()))

    # 读回显，最多 4 秒
    buf = b""
    t0 = time.time()
    while time.time() - t0 < 4:
        n = s.in_waiting
        if n:
            buf += s.read(n)
        else:
            time.sleep(0.1)
    print("[serial] 回显:\n" + buf.decode("utf-8", "replace"))
    s.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""串口一次性执行命令序列（保持串口不关，避免多次打开触发复位）。
用法: python serial_flow.py "命令1" "命令2" ...
每两条命令之间默认等 2 秒；打开串口后默认等 15 秒再发第一条。
"""
import sys, time
import serial

def main():
    cmds = sys.argv[1:]
    if not cmds:
        print("用法: python serial_flow.py <cmd1> <cmd2> ...")
        return 1
    port = "COM3"
    baud = 115200

    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.5
    s.write_timeout = 2
    # 关键：必须在 open() 之前把 DTR/RTS 置为 False（不拉低）。
    # ESP32-S3 原生 USB 串口：DTR 拉低=复位(EN)、RTS 拉低=进下载模式(GPIO0)。
    # pyserial 的 open() 默认会拉低 DTR/RTS，瞬间就把板子复位了 —— 这就是
    # 之前"发一次串口命令分辨率就掉回 QVGA"的根因。open 前先置 False 可避免。
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False

    print("[serial] 打开 %s（DTR/RTS 已置低，不触发复位），等 3 秒..." % port)
    time.sleep(3)
    try:
        s.reset_input_buffer()
    except Exception:
        pass

    for i, c in enumerate(cmds):
        line = c.strip() + "\n"
        s.write(line.encode("utf-8"))
        print("\n[serial] 第 %d 条已发送: %s" % (i + 1, c.strip()))
        # 读这条命令的回显，最多 2.5 秒
        buf = b""
        t0 = time.time()
        while time.time() - t0 < 2.5:
            n = s.in_waiting
            if n:
                buf += s.read(n)
            else:
                time.sleep(0.1)
        print(buf.decode("utf-8", "replace").rstrip())
        if i < len(cmds) - 1:
            time.sleep(2)

    s.close()
    print("\n[serial] 完成")
    return 0

if __name__ == "__main__":
    sys.exit(main())

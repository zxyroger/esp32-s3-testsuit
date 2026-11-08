"""向串口发一条命令并读取一段时间的回显。

用法: python send_cmd.py COM3 "run lcd" 8 20
      参数依次为 端口 命令 读取秒数 发送前等待秒数

为什么要等待：USB-Serial-JTAG 上，pyserial 打开端口的那一瞬间
DTR/RTS 会从默认高电平被拉到低，这个跳变足以让芯片复位一次。
所以每次打开端口都会重新跑一遍开机自测（约 13 s），
必须等它跑完、进入 tf> 提示符，再发命令，否则命令会被启动日志吞掉。
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
cmd = sys.argv[2] if len(sys.argv) > 2 else "help"
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0

s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = False

if wait > 0:
    t0 = time.time()
    while time.time() - t0 < wait:
        s.read(8192)

s.reset_input_buffer()
s.write((cmd + "\r\n").encode())

buf = bytearray()
t0 = time.time()
while time.time() - t0 < dur:
    chunk = s.read(8192)
    if chunk:
        buf += chunk
s.close()

sys.stdout.write(buf.decode("utf-8", "replace"))

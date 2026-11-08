"""直接从 COM3 读取一段时间，不依赖串口服务的缓冲。

用法: python read_raw.py COM3 45
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

s = serial.Serial(port, 115200, timeout=0.2)
# USB-Serial-JTAG: 不要动 DTR/RTS，否则会把芯片按在复位或下载模式
s.dtr = False
s.rts = False

buf = bytearray()
t0 = time.time()
while time.time() - t0 < dur:
    chunk = s.read(8192)
    if chunk:
        buf += chunk
s.close()

sys.stdout.write(buf.decode("utf-8", "replace"))

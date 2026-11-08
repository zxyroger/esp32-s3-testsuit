"""直接占用 COM3，发一次 USB-JTAG 复位脉冲并抓启动 log。"""
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()
time.sleep(0.2)
s.reset_input_buffer()

# USB-Serial-JTAG: RTS 有效 -> EN 拉低 -> 复位
if "--no-reset" not in sys.argv:
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False

buf = bytearray()
end = time.time() + SECS
while time.time() < end:
    n = s.in_waiting
    buf += s.read(n if n else 1)

s.close()
data = bytes(buf)
print(f"[read {len(data)} bytes]")
sys.stdout.write(data.decode("utf-8", "replace"))

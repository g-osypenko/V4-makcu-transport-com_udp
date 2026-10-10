#!/usr/bin/env python3
# 25 s of 0x6A(id=0) at 1 ms pacing; per second: answered/s and physical cursor motion. No injection.
import ctypes, socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
DUR = int(sys.argv[2]) if len(sys.argv) > 2 else 25
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
u32 = ctypes.windll.user32

class P(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

pt = P(); u32.GetCursorPos(ctypes.byref(pt)); last = (pt.x, pt.y)
seq = 0; answered = 0; sec_ans = 0; motion = 0
t0 = time.perf_counter(); nxt = t0; next_print = t0 + 1; next_pos = t0
print("sec | answered/s | cursor motion px")
while time.perf_counter() - t0 < DUR:
    now = time.perf_counter()
    try:
        while True:
            d, _ = s.recvfrom(4096); sec_ans += 1
    except BlockingIOError:
        pass
    if now >= nxt:
        seq += 1; s.sendto(b"\x55" + struct.pack("<Q", seq) + META, T); nxt += 0.001
    if now >= next_pos:
        u32.GetCursorPos(ctypes.byref(pt)); motion += abs(pt.x - last[0]) + abs(pt.y - last[1]); last = (pt.x, pt.y)
        next_pos += 0.005
    if now >= next_print:
        print(f"{int(now - t0):3d} | {sec_ans:10d} | {motion}", flush=True)
        sec_ans = 0; motion = 0; next_print += 1

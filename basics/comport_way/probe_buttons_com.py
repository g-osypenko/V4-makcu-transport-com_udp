# Buttons over COM: what the board actually sends. Enables INPUT_STREAM mouse (0x52 01 01),
# polls GET PHYS_BUTTONS 0x55 every --poll-ms, and prints every incoming frame/byte run for
# --seconds while you press buttons. Restores the stream state at the end. Cursor does not move.
#
#   python probe_buttons_com.py [--port COM5] [--baud 4000000] [--seconds 10] [--poll-ms 50]
import argparse
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM5")
ap.add_argument("--baud", type=int, default=4000000)
ap.add_argument("--seconds", type=float, default=10.0)
ap.add_argument("--poll-ms", type=float, default=50.0)
a = ap.parse_args()

s = serial.Serial(a.port, a.baud, timeout=0, dsrdtr=False, rtscts=False)
s.dtr = False
s.rts = False


def frame(op, pl=b""):
    return b"\xDE\xAD" + len(pl).to_bytes(2, "little") + bytes([op]) + pl


def drain(t):
    end, buf = time.perf_counter() + t, bytearray()
    while time.perf_counter() < end:
        buf += s.read(4096)
        time.sleep(0.001)
    return bytes(buf)


def ask(op, pl=b""):
    s.reset_input_buffer()
    s.write(frame(op, pl))
    return drain(0.05)


print("GET INPUT_STREAM mouse:", ask(0x52, b"\x01").hex(" "))
print("GET BUTTONS 0x10      :", ask(0x10).hex(" "))
print("GET PHYS_BUTTONS 0x55 :", ask(0x55).hex(" "))
s.write(frame(0x52, b"\x01\x01"))
time.sleep(0.05)
print("after SET stream=1    :", ask(0x52, b"\x01").hex(" "))

print(f"\n>>> press buttons now ({a.seconds:.0f} s)")
t0 = time.perf_counter()
next_poll = t0
buf = bytearray()
last_mask = None
events = polls = 0
while time.perf_counter() - t0 < a.seconds:
    now = time.perf_counter()
    if now >= next_poll:
        s.write(frame(0x55))
        polls += 1
        next_poll += a.poll_ms / 1000
    buf += s.read(4096)
    # Split complete DE AD frames; print anything that is not a 0x55 reply with an unchanged mask.
    while True:
        i = buf.find(b"\xDE\xAD")
        if i < 0:
            if buf:
                print(f"  +{now - t0:6.3f}  non-frame bytes: {bytes(buf).hex(' ')}  {bytes(buf)!r}")
            buf.clear()
            break
        if i > 0:
            print(f"  +{now - t0:6.3f}  non-frame bytes: {bytes(buf[:i]).hex(' ')}  {bytes(buf[:i])!r}")
            del buf[:i]
        if len(buf) < 5:
            break
        n = 5 + (buf[2] | buf[3] << 8)
        if len(buf) < n:
            break
        f = bytes(buf[:n])
        del buf[:n]
        op, p = f[4], f[5:]
        if op == 0x55 and len(p) == 1:
            if p[0] != last_mask:
                print(f"  +{now - t0:6.3f}  0x55 mask {p[0]:05b}")
                last_mask = p[0]
        else:
            if op == 0x53:
                events += 1
            print(f"  +{now - t0:6.3f}  frame op {op:02X}: {f.hex(' ')}")
    time.sleep(0.001)

print(f"\npolls {polls}, 0x53 events {events}")
print("GET INPUT_STREAM mouse:", ask(0x52, b"\x01").hex(" "))
s.write(frame(0x52, b"\x01\x00"))
s.close()

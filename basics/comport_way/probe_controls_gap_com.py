# How long must the board rest after a SET without reply over COM before the next command?
# For each gap G: SET LOCK (toggle a random target) -> busy-wait G -> GET 0x56. Snapshot is
# compared with the model; on mismatch the snapshot is re-read after 30 ms to tell "lost" from
# "applied late". A second series sends SET right after the previous snapshot reply (the
# pattern of a closed loop) and SET -> SET with a gap. Locks only: invisible to Windows while
# nobody touches the mouse. Leaves all locks off.
#
#   python probe_controls_gap_com.py [--port COM5] [--rounds 300]
import argparse
import random
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM5")
ap.add_argument("--baud", type=int, default=4000000)
ap.add_argument("--rounds", type=int, default=300)
a = ap.parse_args()

s = serial.Serial(a.port, a.baud, timeout=0, dsrdtr=False, rtscts=False)
s.dtr = False
s.rts = False
SENDABLE = [0, 1, 2, 3, 4, 6, 7, 9, 10, 12, 13]


def frame(op, pl=b""):
    return b"\xDE\xAD" + len(pl).to_bytes(2, "little") + bytes([op]) + pl


def snapshot(timeout=0.1):
    s.reset_input_buffer()
    s.write(frame(0x56))
    t0, buf = time.perf_counter(), bytearray()
    while time.perf_counter() - t0 < timeout:
        buf += s.read(4096)
        i = buf.find(b"\xDE\xAD\x28\x00\x56")
        if i >= 0 and len(buf) >= i + 45:
            p = buf[i + 5:i + 45]
            return normalize(p[1] | p[2] << 8)
    return None


def normalize(m):
    m &= 0x3FFF
    for axis in (5, 8, 11):
        m &= ~(1 << axis)
        if m & (1 << (axis + 1)) and m & (1 << (axis + 2)):
            m |= 1 << axis
    return m


def spin(sec):
    t = time.perf_counter() + sec
    while time.perf_counter() < t:
        pass


def toggle(model):
    t = random.choice(SENDABLE)
    on = not (model >> t) & 1
    return frame(0x60, bytes([t, 1 if on else 0])), normalize(model | (1 << t) if on else model & ~(1 << t))


random.seed(7)
model = snapshot() or 0

print("A) SET -> gap -> GET 0x56")
for gap_ms in (0.0, 0.1, 0.25, 0.5, 1.0, 1.5, 2.0):
    ok = late = lost = 0
    for _ in range(a.rounds):
        f, want = toggle(model)
        s.write(f)
        spin(gap_ms / 1000)
        got = snapshot()
        if got == want:
            ok += 1
        else:
            spin(0.03)
            again = snapshot()
            if again == want:
                late += 1
            else:
                lost += 1
            want = again if again is not None else want
        model = want
    print(f"  gap {gap_ms:4.2f} ms: ok {ok}, applied late {late}, lost {lost} of {a.rounds}")

print("B) SET -> gap -> SET -> 5 ms -> GET 0x56 (is the SECOND one lost?)")
for gap_ms in (0.0, 0.25, 0.5, 1.0, 2.0):
    ok = bad = 0
    for _ in range(a.rounds):
        f1, m1 = toggle(model)
        f2, m2 = toggle(m1)
        s.write(f1)
        spin(gap_ms / 1000)
        s.write(f2)
        spin(0.005)
        got = snapshot()
        if got == m2:
            ok += 1
        else:
            bad += 1
        model = got if got is not None else m2
    print(f"  gap {gap_ms:4.2f} ms: both applied {ok}, not {bad} of {a.rounds}")

s.write(b"".join(frame(0x60, bytes([t, 0])) for t in SENDABLE))
spin(0.01)
print("after cleanup: locks on board", f"{snapshot():04X}")
s.close()

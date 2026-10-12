# Commands without reply over COM: is a pause after SET needed (as over UDP), and what does
# GET INJECT_SNAPSHOT 0x56 look like. Uses only LOCK 0x60 (invisible to Windows while nobody
# touches the mouse) and "release left" SET 0x11 0 (harmless). Cursor does not move, nothing clicks.
# Leaves the board with all locks off.
#
#   python probe_controls_com.py [--port COM5] [--baud 4000000] [--rounds 300]
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

SENDABLE = [0, 1, 2, 3, 4, 6, 7, 9, 10, 12, 13]  # buttons and axis directions (axis bits are derived)


def frame(op, pl=b""):
    return b"\xDE\xAD" + len(pl).to_bytes(2, "little") + bytes([op]) + pl


def frames(buf):
    """Split received bytes into (op, payload) frames, skipping junk."""
    out, i = [], 0
    while i + 5 <= len(buf):
        if buf[i] != 0xDE or buf[i + 1] != 0xAD:
            i += 1
            continue
        n = buf[i + 2] | buf[i + 3] << 8
        if i + 5 + n > len(buf):
            break
        out.append((buf[i + 4], bytes(buf[i + 5:i + 5 + n])))
        i += 5 + n
    return out


def read_until(pred, timeout):
    t0, buf = time.perf_counter(), bytearray()
    while time.perf_counter() - t0 < timeout:
        buf += s.read(4096)
        fr = frames(buf)
        if pred(fr):
            return fr, time.perf_counter() - t0
    return frames(buf), None


def normalize(m):
    m &= 0x3FFF
    for axis in (5, 8, 11):
        m &= ~(1 << axis)
        if m & (1 << (axis + 1)) and m & (1 << (axis + 2)):
            m |= 1 << axis
    return m


def snapshot_in(fr):
    for op, pl in fr:
        if op == 0x56:
            return pl
    return None


s.reset_input_buffer()
s.write(frame(0x04))
fr, _ = read_until(lambda f: any(op == 0x04 for op, _ in f), 0.2)
fw = [pl for op, pl in fr if op == 0x04]
print("FIRMWARE 0x04:", fw[0].hex(" ") if fw else None,
      "->", int.from_bytes(fw[0], "little") if fw and len(fw[0]) == 4 else "?")

s.reset_input_buffer()
s.write(frame(0x56))
fr, t = read_until(lambda f: snapshot_in(f) is not None, 0.2)
snap = snapshot_in(fr)
print(f"GET 0x56: len {len(snap) if snap else None}, rtt {t * 1000 if t else -1:.2f} ms:", snap.hex(" ") if snap else None)

# Does a SET reply anything over COM?
for name, f in [("SET 0x11 0 (left up)", frame(0x11, b"\x00")), ("SET 0x60 3 0 (unlock side1)", frame(0x60, b"\x03\x00"))]:
    s.reset_input_buffer()
    s.write(f)
    fr, _ = read_until(lambda f_: False, 0.05)
    print(f"{name}: reply frames within 50 ms: {[(hex(op), pl.hex(' ')) for op, pl in fr] or 'none'}")

# Back-to-back: k SETs and GET 0x56 in ONE write, no pause. Model must match the snapshot.
random.seed(1)
model = 0
stats = {k: [0, 0] for k in (1, 2, 5)}
rtts = []
mism = []
for r in range(a.rounds):
    k = (1, 2, 5)[r % 3]
    payload = b""
    for _ in range(k):
        t = random.choice(SENDABLE)
        on = random.random() < 0.5
        payload += frame(0x60, bytes([t, 1 if on else 0]))
        model = normalize(model | (1 << t) if on else model & ~(1 << t))
    payload += frame(0x56)
    s.reset_input_buffer()
    s.write(payload)
    fr, t = read_until(lambda f: snapshot_in(f) is not None, 0.1)
    snap = snapshot_in(fr)
    stats[k][1] += 1
    if snap is None or len(snap) < 3:
        mism.append((r, k, "no snapshot"))
        continue
    rtts.append(t * 1000)
    board = normalize(snap[1] | snap[2] << 8)
    if board == model:
        stats[k][0] += 1
    else:
        mism.append((r, k, f"model {model:04X} board {board:04X}"))
        model = board  # resync

# Clean up: unlock everything that can be sent, verify.
s.write(b"".join(frame(0x60, bytes([t, 0])) for t in SENDABLE) + frame(0x56))
fr, _ = read_until(lambda f: snapshot_in(f) is not None, 0.1)
snap = snapshot_in(fr)
for k, (ok, n) in stats.items():
    print(f"{k} SET + GET 0x56 in one write, no pause: {ok}/{n} snapshots match")
if rtts:
    rtts.sort()
    print(f"snapshot rtt: median {rtts[len(rtts) // 2]:.2f} ms, max {rtts[-1]:.2f} ms")
for m in mism[:10]:
    print("  mismatch:", m)
print("after cleanup: locks on board", f"{(snap[1] | snap[2] << 8):04X}" if snap else "?")
s.close()

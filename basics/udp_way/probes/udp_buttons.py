# Buttons over UDP: does the board send INPUT_CHANGE 0x53 events to the UDP client, and
# does GET PHYS_BUTTONS 0x55 answer? Enables INPUT_STREAM mouse (0x52 01 01), reads the
# physical mask (0x55), then prints every datagram for --seconds while you press buttons.
# Restores the stream state at the end. Cursor does not move.
#
#   python udp_buttons.py [--host 192.168.50.175] [--seconds 15]
import argparse
import socket
import struct
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="192.168.50.175")
ap.add_argument("--port", type=int, default=8080)
ap.add_argument("--seconds", type=float, default=15.0)
a = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect((a.host, a.port))
s.setblocking(False)
seq = int(time.time() * 1000) << 8


def send(op, pl=b""):
    global seq
    seq += 1
    s.send(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl)
    return seq


def recv_all(t):
    out, end = [], time.perf_counter() + t
    while time.perf_counter() < end:
        try:
            out.append((time.perf_counter(), s.recv(2048)))
        except BlockingIOError:
            time.sleep(0.0005)
    return out


def query(op, pl=b"", t=0.1):
    k = send(op, pl)
    for _, d in recv_all(t):
        if len(d) >= 14 and d[0] == 0x55 and struct.unpack("<Q", d[1:9])[0] == k:
            return d[14:14 + (d[11] | d[12] << 8)]
    return None


NAMES = {0: "left", 1: "right", 2: "middle", 3: "side1", 4: "side2"}

stream0 = query(0x52, b"\x01")
print("GET INPUT_STREAM mouse:", stream0.hex() if stream0 is not None else None)
print("SET INPUT_STREAM mouse=1:", (query(0x52, b"\x01\x01") or b"").hex() or "(no reply)")
print("GET INPUT_STREAM mouse:", (query(0x52, b"\x01") or b"").hex())
m = query(0x55)
print("GET PHYS_BUTTONS 0x55:", m.hex() if m is not None else None)
print("GET BUTTONS 0x10 (legacy text stream mode):", (query(0x10) or b"").hex())

print(f"\n>>> press left, right, middle, side1, side2 now ({a.seconds:.0f} s) ...")
t0 = time.perf_counter()
mask = m[0] if m else 0
for t, d in recv_all(a.seconds):
    nonce = struct.unpack("<Q", d[1:9])[0] if len(d) >= 9 and d[0] == 0x55 else None
    body = d[9:] if nonce is not None else d
    note = ""
    if len(body) >= 5 and body[0] == 0xDE and body[1] == 0xAD:
        op = body[4]
        p = body[5:5 + (body[2] | body[3] << 8)]
        if op == 0x53 and len(p) == 3 and p[0] == 1:
            if p[1] == 0xFF and p[2] == 0xFF:
                note = "OVERFLOW"
            else:
                mask = mask | (1 << p[1]) if p[2] else mask & ~(1 << p[1])
                note = f"{NAMES.get(p[1], p[1])} {'DOWN' if p[2] else 'up'} -> mask {mask:05b}"
    print(f"  +{t - t0:6.3f} s  nonce {nonce}  {d.hex(' ')}  {note}")
print("\nGET PHYS_BUTTONS 0x55 now:", (query(0x55) or b"").hex())
if stream0 is not None:
    query(0x52, bytes([1, stream0[0]]))
    print("INPUT_STREAM mouse restored to", stream0[0])

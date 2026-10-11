#!/usr/bin/env python3
# Does any 0x6A form free more than one Raw record per datagram? (cumulative ack)
# Creates 4 Raw records (x +1/-1/+1/-1, net zero: cursor twitches by 1 px), then tries
# 0x6A variants and reads free_records after each. Cleans up with normal per-id acks.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.settimeout(0.05)
seq = 7_000_000


def q(op, pl=b""):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl, T)
    end = time.perf_counter() + 0.05
    while time.perf_counter() < end:
        try:
            d = s.recv(2048)
        except (socket.timeout, ConnectionResetError):
            break
        if len(d) >= 14 and d[0] == 0x55 and struct.unpack("<Q", d[1:9])[0] == seq:
            f = d[9:]
            time.sleep(0.003)
            return f[5:5 + struct.unpack("<H", f[2:4])[0]]
    time.sleep(0.003)
    return None


def meta():
    r = q(0x6A, struct.pack("<IB", 0, 0))
    return (struct.unpack("<I", r[2:6])[0], r[6], r[7]) if r and len(r) == 8 else None


def st(r):
    if r is None: return "no reply"
    if len(r) == 6: return f"state {r[0]} reason {r[1]} id {struct.unpack('<I', r[2:6])[0]}"
    return r.hex(" ")


m = meta()
print("meta (last_id, free, cap):", m)
if not m or m[1] < 8:
    sys.exit("need >= 8 free records")
base = m[0] + 1
ids = [base + i for i in range(4)]
for i, rid in enumerate(ids):
    print(f"0x69 id {rid} x {1 if i % 2 == 0 else -1}:", st(q(0x69, struct.pack("<Ihhh", rid, 1 if i % 2 == 0 else -1, 0, 0))))
time.sleep(0.03)
print("after 4 moves:", meta())

tests = [
    ("A consume=2 on newest id", struct.pack("<IB", ids[3], 2)),
    ("B id=0 consume=1", struct.pack("<IB", 0, 1)),
    ("C id=0xFFFFFFFF consume=1", struct.pack("<IB", 0xFFFFFFFF, 1)),
    ("D two records in one 0x6A (10 B)", struct.pack("<IBIB", ids[0], 1, ids[1], 1)),
    ("E consume=255 on newest id", struct.pack("<IB", ids[3], 255)),
]
for name, pl in tests:
    r = q(0x6A, pl)
    print(f"{name}: {st(r)} | meta {meta()}")

for rid in ids:
    print(f"cleanup ack {rid}:", st(q(0x6A, struct.pack("<IB", rid, 1))))
print("final:", meta())
s.close()

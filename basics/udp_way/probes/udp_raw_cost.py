#!/usr/bin/env python3
# (1) intake of 0x69-only bursts at 1.0 / 0.5 ms gaps (acks later, slowly);
# (2) capacity: 0x69 every 2 ms with NO acks - does #17 get 255/3?
# MOVES THE CURSOR by +-1 (net 0). All records are consumed at the end.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 0
replies = {}  # seq -> (t, payload)

def send(op, pl):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl, T)
    return seq, time.perf_counter()

def pump(until):
    while time.perf_counter() < until:
        try:
            d, _ = s.recvfrom(4096)
        except BlockingIOError:
            continue
        if len(d) >= 14 and d[0] == 0x55:
            replies[struct.unpack("<Q", d[1:9])[0]] = (time.perf_counter(), d[14:])

def q(op, pl, wait=0.02):
    n, t = send(op, pl); pump(time.perf_counter() + wait)
    return replies.get(n, (None, None))[1]

def meta():
    p = q(0x6A, b"\0" * 5, 0.05)
    return dict(last=struct.unpack("<I", p[2:6])[0], free=p[6]) if p else None

def drain(ids):
    ok = 0
    for i in ids:
        for _ in range(3):
            p = q(0x6A, struct.pack("<IB", i, 1), 0.006)
            if p and p[0] in (2, 3): ok += 1; break
            if p and p[0] == 255 and p[1] == 5: break
            time.sleep(0.003)
    return ok

m = meta(); print("meta", m)
nid = m["last"] + 1

def burst(gap_ms, n):
    global nid
    ids, seqs = [], []
    t0 = time.perf_counter(); dx = 1
    for k in range(n):
        tgt = t0 + k * gap_ms / 1000
        pump(tgt)
        sq, _ = send(0x69, struct.pack("<IhhH", nid, dx, 0, 0)); seqs.append((sq, nid)); ids.append(nid)
        nid += 1; dx = -dx
    pump(time.perf_counter() + 0.02)
    acc = sum(1 for sq, i in seqs if sq in replies and replies[sq][1][0] == 1)
    rej = [(replies[sq][1][0], replies[sq][1][1]) for sq, i in seqs if sq in replies and replies[sq][1][0] != 1]
    lost = sum(1 for sq, i in seqs if sq not in replies)
    time.sleep(0.01)
    consumed = drain(ids)
    return acc, lost, rej, consumed

for gap in (2.0, 1.0, 0.5):
    tot = [0, 0, 0]
    for r in range(10):
        a, l, rj, c = burst(gap, 10)
        tot[0] += a; tot[1] += l; tot[2] += c
        time.sleep(0.03)
    print(f"0x69-only bursts of 10 at {gap} ms x10: accepted {tot[0]}/100, no reply {tot[1]}, consumed later {tot[2]}")

# (2) capacity without acks
seqs = []; ids = []
t0 = time.perf_counter(); dx = 1
for k in range(24):
    pump(t0 + k * 0.002)
    sq, _ = send(0x69, struct.pack("<IhhH", nid, dx, 0, 0)); seqs.append(sq); ids.append(nid); nid += 1; dx = -dx
pump(time.perf_counter() + 0.03)
res = [replies[sq][1][:2].hex() if sq in replies else "--" for sq in seqs]
print("no-ack stream 24 @2ms, state/reason per move:", " ".join(res))
print("meta now", meta())
print("consumed", drain(ids), "meta after drain", meta())

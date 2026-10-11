#!/usr/bin/env python3
# Distribution of the board's per-datagram service cycle: pairs A,B (B 1.0 ms after A), interval between replies.
# Also a saturated stream (1 datagram / 0.5 ms) - spacing between consecutive replies.
# Only 0x6A(id=0) queries. Cursor does not move.
import collections, socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 11_000_000


def send():
    global seq
    seq += 1
    t = time.perf_counter()
    s.sendto(b"\x55" + struct.pack("<Q", seq) + META, T)
    return seq, t


def poll(got):
    while True:
        try:
            d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError):
            return
        if len(d) >= 9 and d[0] == 0x55:
            got[struct.unpack("<Q", d[1:9])[0]] = time.perf_counter()


def wait(until, got):
    while time.perf_counter() < until:
        poll(got)


def hist(vals, lo, hi, step):
    c = collections.Counter(int((v - lo) / step) for v in vals if lo <= v < hi)
    out = []
    for i in range(int((hi - lo) / step)):
        n = c.get(i, 0)
        if n:
            out.append(f"  {lo + i * step:5.2f}-{lo + (i + 1) * step:5.2f} ms: {n:4d} {'#' * min(60, n // 2 or 1)}")
    return "\n".join(out)


rtt_a, ab = [], []
for _ in range(500):
    got = {}
    a, ta = send()
    wait(ta + 0.0010, got)
    b, tb = send()
    wait(time.perf_counter() + 0.006, got)
    if a in got: rtt_a.append((got[a] - ta) * 1e3)
    if a in got and b in got: ab.append((got[b] - got[a]) * 1e3)
    wait(time.perf_counter() + 0.002 + (_ % 7) * 0.00013, got)  # vary phase
print(f"pairs: A RTT n={len(rtt_a)}  B-A reply interval n={len(ab)}")
print("A RTT:\n" + hist(rtt_a, 0.8, 1.6, 0.05))
print("B-A reply interval:\n" + hist(ab, 1.2, 2.4, 0.05))

got = {}
sent = []
t0 = time.perf_counter()
for i in range(4000):
    wait(t0 + i * 0.0005, got)
    sent.append(send()[0])
wait(time.perf_counter() + 0.05, got)
ts = sorted(got[k] for k in sent if k in got)
gaps = [(b - a) * 1e3 for a, b in zip(ts, ts[1:])]
print(f"saturated 2000/s for 2 s: replies {len(ts)} = {len(ts) / 2.0:.0f}/s")
print("reply spacing:\n" + hist(gaps, 1.2, 2.6, 0.05))
s.close()

#!/usr/bin/env python3
# Model of the board's UDP intake: exact dead window after an accepted datagram,
# when B's reply comes relative to A's, triples, and whether the window is per source socket.
# Only 0x6A(id=0) metadata queries. Cursor does not move.
import socket, statistics, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"


def mk():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
    except Exception: pass
    s.setblocking(False)
    return s


socks = [mk(), mk()]
seq = 7_000_000


def poll(got):
    for s in socks:
        while True:
            try:
                d, _ = s.recvfrom(4096)
            except (BlockingIOError, ConnectionResetError):
                break
            if len(d) >= 9 and d[0] == 0x55:
                got[struct.unpack("<Q", d[1:9])[0]] = time.perf_counter()


def wait(until, got):
    while time.perf_counter() < until:
        poll(got)


def send(si=0):
    global seq
    seq += 1
    t = time.perf_counter()
    socks[si].sendto(b"\x55" + struct.pack("<Q", seq) + META, T)
    return seq, t


def series(gaps_us, srcs=None, reps=60):
    """Send len(gaps)+1 datagrams: first at 0, next after each gap. Return accept counts and reply offsets."""
    n = len(gaps_us) + 1
    srcs = srcs or [0] * n
    acc = [0] * n
    rel = [[] for _ in range(n)]   # reply time of k-th minus reply time of first (ms)
    rtt = [[] for _ in range(n)]
    for _ in range(reps):
        got = {}
        ids = []
        k, t0 = send(srcs[0]); ids.append((k, t0))
        tnext = t0
        for i, g in enumerate(gaps_us):
            tnext += g / 1e6
            wait(tnext, got)
            ids.append(send(srcs[i + 1]))
        wait(time.perf_counter() + 0.008, got)
        first = got.get(ids[0][0])
        for j, (k, ts) in enumerate(ids):
            if k in got:
                acc[j] += 1
                rtt[j].append((got[k] - ts) * 1e3)
                if first is not None:
                    rel[j].append((got[k] - first) * 1e3)
        wait(time.perf_counter() + 0.004, got)
    med = lambda v: f"{statistics.median(v):.2f}" if v else "  - "
    return acc, [med(v) for v in rel], [med(v) for v in rtt]


print("== pairs A,B (one socket): accepted B / 60, B reply after A reply (ms), RTT A / RTT B")
for g in (500, 550, 600, 650, 700, 750, 800, 900, 1000, 1200, 1500, 1700, 1900):
    acc, rel, rtt = series([g])
    print(f"gap {g:5d} us: A {acc[0]:2d} B {acc[1]:2d} | B-A reply {rel[1]} | RTT A {rtt[0]} B {rtt[1]}")

print("== pairs A,B from two different source sockets")
for g in (200, 500, 700, 900):
    acc, rel, rtt = series([g], srcs=[0, 1])
    print(f"gap {g:5d} us: A {acc[0]:2d} B {acc[1]:2d} | B-A reply {rel[1]} | RTT A {rtt[0]} B {rtt[1]}")

print("== triples A,B,C with equal gaps: accepted counts, reply offsets after A")
for g in (800, 900, 1000, 1200):
    acc, rel, rtt = series([g, g])
    print(f"gaps {g:4d} us: A {acc[0]:2d} B {acc[1]:2d} C {acc[2]:2d} | B {rel[1]} C {rel[2]} | RTT {rtt}")

print("== A then B,C,D,E back-to-back 100 us after a gap g: which one survives?")
for g in (300, 900):
    acc, rel, rtt = series([g, 100, 100, 100])
    print(f"g {g} us: accepted {acc} | reply offsets {rel}")
for s in socks: s.close()

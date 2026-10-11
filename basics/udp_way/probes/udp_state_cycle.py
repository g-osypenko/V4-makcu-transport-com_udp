#!/usr/bin/env python3
# Does the board's ~1.72 ms UDP command cycle depend on board state? Live settings only, restored at the end:
#   interpolation 0x1F (0 / AUTO / 100), buttons stream 0x10 (off / on).
# Also the cost of MOVE 0x18 with a zero delta (explains 0x18 path loss over UDP?).
# Queries and zero moves only. Cursor does not move.
import socket, statistics, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 14_000_000


def frame(op, pl=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl


META = frame(0x6A, b"\0" * 5)


def raw(body):
    global seq
    seq += 1
    return seq, b"\x55" + struct.pack("<Q", seq) + body


def wait(until, got):
    while time.perf_counter() < until:
        try:
            d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError):
            continue
        if len(d) >= 9 and d[0] == 0x55:
            got[struct.unpack("<Q", d[1:9])[0]] = (time.perf_counter(), d[9:])


def q(op, pl=b"", t=0.05):
    got = {}
    k, p = raw(frame(op, pl)); s.sendto(p, T)
    wait(time.perf_counter() + t, got)
    if k not in got:
        return None
    d = got[k][1]
    return d[5:5 + struct.unpack("<H", d[2:4])[0]] if d[:2] == b"\xDE\xAD" else d


def cycle(label, x=META, pairs=80):
    """B (meta) 1.0 ms after X: median reply(B) - reply(X) or B RTT; plus saturated meta rate."""
    ab, brtt = [], []
    for i in range(pairs):
        got = {}
        kx, px = raw(x); tx = time.perf_counter(); s.sendto(px, T)
        wait(tx + 0.0010, got)
        kb, pb = raw(META); tb = time.perf_counter(); s.sendto(pb, T)
        wait(time.perf_counter() + 0.008, got)
        if kb in got:
            brtt.append((got[kb][0] - tb) * 1e3)
            if kx in got: ab.append((got[kb][0] - got[kx][0]) * 1e3)
        wait(time.perf_counter() + 0.003 + (i % 5) * 0.0002, got)
    got, sent = {}, []
    t0 = time.perf_counter()
    for i in range(1000):
        wait(t0 + i / 1000, got)
        k, p = raw(META); s.sendto(p, T); sent.append(k)
    wait(time.perf_counter() + 0.05, got)
    rate = sum(1 for k in sent if k in got)
    med = lambda v: f"{statistics.median(v):.2f}" if v else "  - "
    # with X answered: cycle = B-X reply gap; with X silent (SET): cycle ~ 1.0 + RTT_B - 1.05
    cyc = statistics.median(ab) if ab else (1.0 + statistics.median(brtt) - 1.05 if brtt else float("nan"))
    print(f"{label:34s} cycle ~{cyc:4.2f} ms | B accepted {len(brtt):2d}/{pairs}, RTT_B {med(brtt)} | saturated 1000/s -> {rate}/s")


interp0 = q(0x1F)
btn0 = q(0x10)
print("initial: interp 0x1F =
#!/usr/bin/env python3
# How long does each kind of datagram occupy the board's UDP server?
# Pair: X at t=0, then a 0x6A(id=0) probe B at gap g. If B is accepted, the server
# was busy with X for about C = g + (RTT_B - RTT_idle). If X is dropped early, B is answered at RTT_idle.
# Only reads and no-op SETs (MOVE_MASK 0,0,0,0 which is the current state). Cursor does not move.
import socket, statistics, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 8_000_000


def frame(op, pl=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl


def raw(body):
    global seq
    seq += 1
    return seq, b"\x55" + struct.pack("<Q", seq) + body


def poll(got, other):
    while True:
        try:
            d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError):
            return
        if len(d) >= 9 and d[0] == 0x55:
            got[struct.unpack("<Q", d[1:9])[0]] = (time.perf_counter(), d[9:])
        else:
            other.append(d)


def wait(until, got, other):
    while time.perf_counter() < until:
        poll(got, other)


X = {
    "meta 0x6A (reply)": lambda: raw(frame(0x6A, b"\0" * 5)),
    "SET 0x16 no-op (no reply)": lambda: raw(frame(0x16, b"\0\0\0\0")),
    "unknown op 0xEE": lambda: raw(frame(0xEE)),
    "valid frame + 1 trailing byte": lambda: raw(frame(0x6A, b"\0" * 5) + b"\0"),
    "bad magic DE AE": lambda: raw(b"\xDE\xAE\x05\x00\x6A" + b"\0" * 5),
    "bare frame (HOST)": lambda: (None, frame(0x6A, b"\0" * 5)),
    "1 byte 0x00": lambda: (None, b"\0"),
    "0x55 + 8 nonce only": lambda: raw(b""),
    "1400 B garbage": lambda: (None, b"\xAB" * 1400),
}

base = []
for _ in range(40):
    got, other = {}, []
    k, pkt = raw(META); t = time.perf_counter(); s.sendto(pkt, T)
    wait(t + 0.006, got, other)
    if k in got: base.append((got[k][0] - t) * 1e3)
idle = statistics.median(base)
print(f"idle RTT p50 {idle:.3f} ms")

for name, mkx in X.items():
    print(f"== X = {name}")
    xr = []
    for g in (300, 600, 900, 1200, 1500):
        acc = 0; rtt = []; xrep = 0
        for _ in range(30):
            got, other = {}, []
            kx, px = mkx(); t0 = time.perf_counter(); s.sendto(px, T)
            wait(t0 + g / 1e6, got, other)
            kb, pb = raw(META); tb = time.perf_counter(); s.sendto(pb, T)
            wait(time.perf_counter() + 0.006, got, other)
            if kx is not None and kx in got: xrep += 1
            if other: xrep += 0
            if kb in got:
                acc += 1; rtt.append((got[kb][0] - tb) * 1e3)
            if kx is not None and kx in got and not xr:
                xr.append(got[kx][1].hex(" "))
            wait(time.perf_counter() + 0.003, got, other)
        r = statistics.median(rtt) if rtt else float("nan")
        busy = g / 1e3 + (r - idle) if rtt else float("nan")
        print(f"  gap {g:5d} us: B accepted {acc:2d}/30, RTT_B {r:5.2f} ms -> X busy ~{busy:4.2f} ms | X replied {xrep}/30")
    if xr: print(f"  X reply: {xr[0]}")
s.close()

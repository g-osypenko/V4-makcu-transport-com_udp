#!/usr/bin/env python3
# Does a no-reply SET consume the board's ~1.72 ms UDP cycle? Do several source sockets add up?
# Only 0x6A(id=0) queries and 0x16 MOVE_MASK(0,0,0,0) (already all 0). Cursor does not move.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
NOOP = b"\xDE\xAD\x04\x00\x16\0\0\0\0"

def mk():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
    except Exception: pass
    s.setblocking(False)
    return s

socks = [mk() for _ in range(4)]
seq = 900000

def poll(got):
    for s in socks:
        while True:
            try:
                d, _ = s.recvfrom(4096); got.append((time.perf_counter(), d))
            except (BlockingIOError, ConnectionResetError):
                break

def run(name, pattern, n=1000, iv=1.0):
    """pattern(i) -> (socket index, frame, expect_reply)"""
    global seq
    sent, got = {}, []
    t0 = time.perf_counter()
    for i in range(n):
        tgt = t0 + i * iv / 1000
        while time.perf_counter() < tgt: poll(got)
        si, fr, exp = pattern(i)
        seq += 1
        if exp: sent[seq] = time.perf_counter()
        socks[si].sendto(b"\x55" + struct.pack("<Q", seq) + fr, T)
    end = time.perf_counter() + 0.2
    while time.perf_counter() < end: poll(got)
    r = {struct.unpack("<Q", d[1:9])[0] for t, d in got if len(d) >= 9 and d[0] == 0x55}
    ok = sum(1 for k in sent if k in r)
    extra = sum(1 for t, d in got if len(d) >= 9 and d[0] == 0x55 and struct.unpack("<Q", d[1:9])[0] not in sent)
    dur = n * iv / 1000
    print(f"{name}: queries {len(sent)} answered {ok} ({100*ok/max(1,len(sent)):.0f}%), {ok/dur:.0f} replies/s, "
          f"other replies {extra}")
    time.sleep(0.15)

run("A: meta only @2ms (baseline)", lambda i: (0, META, True), 500, 2.0)
run("B: meta@2ms + noop between (1000 dgram/s)", lambda i: (0, META, True) if i % 2 == 0 else (0, NOOP, False))
run("C: meta@4ms + noop x3 between (1000 dgram/s)", lambda i: (0, META, True) if i % 4 == 0 else (0, NOOP, False))
run("D: meta@1ms one socket", lambda i: (0, META, True))
run("E: meta@1ms round-robin 2 sockets", lambda i: (i % 2, META, True))
run("F: meta@1ms round-robin 4 sockets", lambda i: (i % 4, META, True))
run("G: meta@0.5ms round-robin 4 sockets", lambda i: (i % 4, META, True), 2000, 0.5)
for s in socks: s.close()

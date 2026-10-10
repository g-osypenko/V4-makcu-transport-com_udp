#!/usr/bin/env python3
# Per-opcode service rate and dead time after a request. Read-only opcodes. Cursor does not move.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
OPS = {"0x02": b"\xDE\xAD\x00\x00\x02", "0x04": b"\xDE\xAD\x00\x00\x04", "0x6A": b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"}
seq = 50000

def spin_until(t, got):
    while time.perf_counter() < t:
        try:
            d, _ = s.recvfrom(4096); got.append((time.perf_counter(), d))
        except BlockingIOError:
            pass

def send(frame, sent):
    global seq
    seq += 1
    sent[seq] = time.perf_counter()
    s.sendto(b"\x55" + struct.pack("<Q", seq) + frame, T)
    return seq

def replied(got):
    return {struct.unpack("<Q", d[1:9])[0]: t for t, d in got if len(d) >= 9 and d[0] == 0x55}

print("-- service rate per opcode (500 req) --")
for name, fr in OPS.items():
    for iv in (1.0, 1.5):
        sent, got = {}, []
        t0 = time.perf_counter()
        for i in range(500):
            spin_until(t0 + i * iv / 1000, got)
            send(fr, sent)
        spin_until(time.perf_counter() + 0.2, got)
        r = replied(got)
        ok = sum(1 for k in sent if k in r)
        # reply spacing
        ts = sorted(r[k] for k in sent if k in r)
        gaps = sorted((b - a) * 1e3 for a, b in zip(ts, ts[1:]))
        print(f"{name} interval {iv} ms: replied {ok}/500 = {ok/((500*iv)/1000):.0f}/s,"
              f" reply gap min {gaps[0]:.2f} p50 {gaps[len(gaps)//2]:.2f} ms")
        time.sleep(0.1)

print("-- dead time: pair A,B with gap g; is B answered? (100 pairs each) --")
for g_us in (200, 500, 800, 1000, 1200, 1400, 1600, 1800, 2000):
    okA = okB = 0
    for i in range(100):
        sent, got = {}, []
        a = send(OPS["0x6A"], sent)
        spin_until(sent[a] + g_us / 1e6, got)
        b = send(OPS["0x6A"], sent)
        spin_until(time.perf_counter() + 0.006, got)
        r = replied(got)
        okA += a in r; okB += b in r
    print(f"gap {g_us:5d} us: A {okA}/100 B {okB}/100")
s.close()

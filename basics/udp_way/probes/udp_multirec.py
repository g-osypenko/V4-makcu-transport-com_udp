#!/usr/bin/env python3
# Untested item 1 of HANDOFF: several FULL raw records (0x55 + nonce + frame) in one datagram, padding,
# alignment, LEN covering extra bytes, IP fragmentation. Only 0x6A(id=0) metadata queries. Cursor does not move.
# For every variant: which nonces get a reply, and does the datagram occupy the board (probe B 0.3 ms later).
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 12_000_000


def nonce():
    global seq
    seq += 1
    return seq, struct.pack("<Q", seq)


def rec(fr=META):
    k, n = nonce()
    return k, b"\x55" + n + fr


def collect(sec):
    out, end = [], time.perf_counter() + sec
    while time.perf_counter() < end:
        try:
            d, _ = s.recvfrom(65535)
            out.append((time.perf_counter(), d))
        except (BlockingIOError, ConnectionResetError):
            pass
    return out


def run(name, keys, data, probe=True):
    t0 = time.perf_counter()
    s.sendto(data, T)
    kb = None
    if probe:
        while time.perf_counter() < t0 + 0.0003:
            pass
        kb, pb = rec(); s.sendto(pb, T)
    out = collect(0.03)
    rep = {}
    for t, d in out:
        if len(d) >= 9 and d[0] == 0x55:
            rep[struct.unpack("<Q", d[1:9])[0]] = ((t - t0) * 1e3, d[9:])
    hits = [f"#{i}:{rep[k][0]:.2f}ms" for i, k in enumerate(keys) if k in rep]
    extra = [d.hex(" ") for t, d in out if not (len(d) >= 9 and d[0] == 0x55)]
    bstate = "-" if kb is None else (f"B answered {rep[kb][0] - 0.3:.2f} ms after send (" +
                                    ("free" if rep[kb][0] - 0.3 < 1.3 else "busy") + ")" if kb in rep else "B lost (busy)")
    print(f"{name:52s} len {len(data):5d}: replies {len(hits)}/{len(keys)} {hits} | {bstate}" + (f" | other {extra}" if extra else ""))
    for k in keys:
        if k in rep and rep[k][1] != rep[keys[0]][1] if keys[0] in rep else False:
            print("      reply body", rep[k][1].hex(" "))
    time.sleep(0.02)


k1, r1 = rec()
run("control: one record", [k1], r1)
ka, ra = rec(); kb2, rb = rec()
run("two full records 55|nA|fA|55|nB|fB", [ka, kb2], ra + rb)
ks, data = [], b""
for _ in range(4):
    k, r = rec(); ks.append(k); data += r
run("four full records", ks, data)
k, r = rec(); run("record + 1 zero byte", [k], r + b"\0")
k, r = rec(); run("record + 3 zero bytes (align 4)", [k], r + b"\0" * 3)
k, r = rec(); run("record padded to 32 bytes", [k], r + b"\0" * (32 - len(r)))
k, r = rec(); run("record padded to 64 bytes", [k], r + b"\0" * (64 - len(r)))
k, r = rec(); run("record + bare 0x55", [k], r + b"\x55")
ka, ra = rec(); kb2, rb = rec()
run("two records, 3-byte pad between", [ka, kb2], ra + b"\0\0\0" + rb)
ka, ra = rec(); kb2, rb = rec()
run("two records, second without 0x55", [ka, kb2], ra + rb[1:])
k, n = nonce()
run("one header, LEN covers 2 frames (0x6A, LEN 19)", [k], b"\x55" + n + b"\xDE\xAD\x13\x00\x6A" + b"\0" * 5 + META[:9])
k, n = nonce()
run("one header, 0x6A with LEN 6 (extra payload byte)", [k], b"\x55" + n + b"\xDE\xAD\x06\x00\x6A" + b"\0" * 6)
ks, data = [], b""
for _ in range(10):
    k, r = rec(); ks.append(k); data += r
run("ten full records (230 B)", ks, data)
ks, data = [], b""
for _ in range(100):
    k, r = rec(); ks.append(k); data += r
run("100 full records (2300 B, IP-fragmented)", ks, data, probe=False)
k, r = rec()
run("one record padded to 2000 B (IP-fragmented)", [k], r + b"\0" * (2000 - len(r)), probe=False)
k, r = rec()
run("control again", [k], r)
s.close()

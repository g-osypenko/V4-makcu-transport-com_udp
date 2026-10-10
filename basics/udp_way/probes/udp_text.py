#!/usr/bin/env python3
# 0x6B 'k' text tunnel over UDP: one / several KM queries per datagram. Then streams on/off vs intake rate.
# Only queries and stream toggles (restored). Cursor does not move.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 0

def send_raw(body):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + body, T)
    return seq

def frame(op, payload=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(payload)) + bytes([op]) + payload

def collect(sec):
    out, end = [], time.perf_counter() + sec
    while time.perf_counter() < end:
        try:
            d, _ = s.recvfrom(4096); out.append(d)
        except BlockingIOError:
            pass
    return out

def show(name, body, sec=0.15):
    n = send_raw(body)
    out = collect(sec)
    print(f"{name}: tx {len(body)} B -> {len(out)} datagrams")
    for d in out:
        tail = d[9:]
        print(f"    nonce {'OK ' if struct.unpack('<Q', d[1:9])[0] == n else 'other'} {tail.hex(' ')} | {tail!r}")

show("0x6B 'm.version()'", frame(0x6B, b"m.version()"))
show("0x6B 'm.device()'", frame(0x6B, b"m.device()"))
show("0x6B two cmds CRLF", frame(0x6B, b"m.version()\r\nkm.device()"))
show("0x6B two cmds ';'", frame(0x6B, b"m.version();km.device()"))
show("0x6B with trailing CRLF", frame(0x6B, b"m.version()\r\n"))
show("plain text km.version()", b"km.version()\r\n")

# Streams vs intake rate
def get(op, pl=b""):
    n = send_raw(frame(op, pl))
    for d in collect(0.05):
        if struct.unpack("<Q", d[1:9])[0] == n:
            return d[14:]
    return None

def rate(label):
    sent, got, t0 = set(), [], time.perf_counter()
    for i in range(1000):
        tgt = t0 + i / 1000
        while time.perf_counter() < tgt:
            try:
                d, _ = s.recvfrom(4096); got.append(struct.unpack("<Q", d[1:9])[0])
            except BlockingIOError:
                pass
        sent.add(send_raw(frame(0x6A, b"\0" * 5)))
    got += [struct.unpack("<Q", d[1:9])[0] for d in collect(0.2)]
    print(f"{label}: 1000 meta @1ms -> answered {len(sent & set(got))}")

b0 = get(0x10); m0 = get(0x52, b"\x01")
print("BUTTONS", b0.hex() if b0 else None, "INPUT_STREAM mouse", m0.hex() if m0 else None)
rate("streams as is")
if b0 is not None and m0 is not None:
    get(0x10, b"\x00"); get(0x52, b"\x01\x00")
    print("after off: BUTTONS", get(0x10).hex(), "INPUT_STREAM mouse", get(0x52, b"\x01").hex())
    rate("streams off")
    get(0x10, bytes([b0[0]])); get(0x52, bytes([1, m0[0]]))
    print("restored: BUTTONS", get(0x10).hex(), "INPUT_STREAM mouse", get(0x52, b"\x01").hex())

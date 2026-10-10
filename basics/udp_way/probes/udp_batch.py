#!/usr/bin/env python3
# Try batch / alternative framings over UDP with 0x6A(id=0) meta only. Cursor does not move.
import os, socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.settimeout(0.3)
FRAME = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
REC = b"\x6A\0\0\0\0\0"  # CMD+PAYLOAD (BLE record)

def mbat(n, bid=1):
    r = b"MBAT" + bytes([1, 0]) + struct.pack("<H", bid) + bytes([n])
    for _ in range(n):
        r += bytes([len(REC)]) + REC
    return r

def tryit(name, data):
    s.sendto(data, T)
    out = []
    while True:
        try:
            d, _ = s.recvfrom(4096); out.append(d); s.settimeout(0.05)
        except socket.timeout:
            break
    s.settimeout(0.3)
    print(f"{name}: tx {len(data)} B -> {len(out)} replies")
    for d in out:
        print("   ", d.hex(" "))
    time.sleep(0.05)

n = os.urandom(8)
tryit("raw 1 frame (control)", b"\x55" + n + FRAME)
tryit("raw + 2 frames", b"\x55" + os.urandom(8) + FRAME * 2)
tryit("raw + MBAT(2) of records", b"\x55" + os.urandom(8) + mbat(2))
tryit("raw + MBAT(1)", b"\x55" + os.urandom(8) + mbat(1))
tryit("plain MBAT(2)", mbat(2, 7))
tryit("raw + record only (CMD+PAYLOAD)", b"\x55" + os.urandom(8) + REC)
tryit("plain 2 frames", FRAME * 2)

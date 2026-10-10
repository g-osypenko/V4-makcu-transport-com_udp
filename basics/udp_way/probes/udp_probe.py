#!/usr/bin/env python3
# Шаг 0: read-only проба MAKCU по UDP. Курсор не двигается.
import os, socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
T = (HOST, PORT)

def frame(cmd, payload=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(payload)) + bytes([cmd]) + payload

def wrap(mode, f):
    if mode == "host":
        return f, None
    n = os.urandom(8)
    return b"\x55" + n + f, n

def xfer(sock, mode, cmd, payload=b"", wait=0.3):
    pkt, nonce = wrap(mode, frame(cmd, payload))
    t0 = time.perf_counter()
    sock.sendto(pkt, T)
    out = []
    sock.settimeout(wait)
    while True:
        try:
            d, a = sock.recvfrom(2048)
            out.append((time.perf_counter() - t0, d))
            sock.settimeout(0.05)
        except socket.timeout:
            break
        except ConnectionResetError as e:
            out.append((time.perf_counter() - t0, b"<ICMP reset>"))
            break
    return pkt, nonce, out

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    print(f"local {s.getsockname()} -> {T}")
    for mode in ("raw", "host"):
        print(f"\n=== mode {mode} ===")
        for name, cmd, pl in (("DEVICE 0x02", 0x02, b""), ("FIRMWARE 0x04", 0x04, b""),
                              ("RAW_ACK meta 0x6A id=0", 0x6A, b"\0\0\0\0\0"),
                              ("INTERP 0x1F get", 0x1F, b"")):
            pkt, nonce, out = xfer(s, mode, cmd, pl)
            print(f"{name}: tx {pkt.hex(' ')}")
            if not out:
                print("   (нет ответа)")
            for dt, d in out:
                tag = ""
                if nonce and d[:1] == b"\x55":
                    tag = " nonce=OK" if d[1:9] == nonce else " nonce=CHUZHOI"
                print(f"   rx {dt*1000:.2f} мс len {len(d)}: {d.hex(' ')}{tag}")
    s.close()

main()

# Buttons over UDP, wide net: are there ANY unsolicited datagrams from the board when buttons
# change? Unlike udp_buttons.py the command socket is NOT connect()-ed (receives from any source
# address/port), and extra sockets listen on local ports 8080-8082. Phases enable each stream
# variant in turn while 0x55 is polled every 100 ms to prove the buttons were pressed.
# Restores INPUT_STREAM mouse and BUTTONS at the end. Cursor does not move.
#
#   python udp_buttons_any.py [--host 192.168.50.175] [--phase-s 8]
import argparse
import select
import socket
import struct
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="192.168.50.175")
ap.add_argument("--port", type=int, default=8080)
ap.add_argument("--phase-s", type=float, default=8.0)
a = ap.parse_args()
board = (a.host, a.port)

cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
cmd.bind(("0.0.0.0", 0))
socks = [cmd]
for p in (8080, 8081, 8082):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", p))
        socks.append(s)
    except OSError as e:
        print(f"(local port {p} not available: {e})")
print("listening on", [s.getsockname()[1] for s in socks], "- command socket", cmd.getsockname()[1])

seq = int(time.time() * 1000) << 8
pending = {}


def send(op, pl=b"", tag=None):
    global seq
    seq += 1
    cmd.sendto(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl, board)
    pending[seq] = tag
    return seq


def query(op, pl=b"", t=0.15):
    k = send(op, pl, "q")
    end = time.perf_counter() + t
    while time.perf_counter() < end:
        r, _, _ = select.select(socks, [], [], 0.01)
        for s in r:
            d, src = s.recvfrom(2048)
            if s is cmd and src == board and len(d) >= 14 and struct.unpack("<Q", d[1:9])[0] == k:
                return d[14:14 + (d[11] | d[12] << 8)]
            print(f"  (other datagram on :{s.getsockname()[1]} from {src}: {d.hex(' ')})")
    return None


stream0 = query(0x52, b"\x01")
buttons0 = query(0x10)
print("before: INPUT_STREAM mouse", stream0.hex() if stream0 else None, "| BUTTONS 0x10", buttons0.hex() if buttons0 else None)

phases = [
    ("INPUT_STREAM mouse = 1 (0x52 01 01)", [(0x52, b"\x01\x01")]),
    ("+ legacy BUTTONS 0x10 = 1 (km. prefixed)", [(0x10, b"\x01")]),
    ("+ legacy BUTTONS 0x10 = 3 (bare mask byte)", [(0x10, b"\x03")]),
]
names = ["L", "R", "M", "S1", "S2"]
print(f"\n>>> press buttons continuously for {len(phases) * a.phase_s:.0f} s")
for title, sets in phases:
    for op, pl in sets:
        send(op, pl, "set")
    time.sleep(0.05)
    st = (query(0x52, bytes([1])) or b"").hex()
    bt = (query(0x10) or b"").hex()
    print(f"\n== {title}: stream GET {st}, BUTTONS GET {bt}")
    t0 = time.perf_counter()
    next_poll = t0
    changes = other = 0
    last = None
    while time.perf_counter() - t0 < a.phase_s:
        if time.perf_counter() >= next_poll:
            send(0x55, b"", "poll")
            next_poll += 0.1
        r, _, _ = select.select(socks, [], [], 0.005)
        for s in r:
            d, src = s.recvfrom(2048)
            k = struct.unpack("<Q", d[1:9])[0] if len(d) >= 14 and d[0] == 0x55 else None
            tag = pending.pop(k, None) if (s is cmd and src == board and k is not None) else None
            if tag == "poll" and len(d) == 15:
                if d[14] != last:
                    changes += 1
                    last = d[14]
            elif tag in ("set", "q"):
                pass
            else:
                other += 1
                print(f"  +{time.perf_counter() - t0:6.3f} UNSOLICITED on :{s.getsockname()[1]} from {src}: {d.hex(' ')}  {d!r}")
    print(f"   -> mask changes seen by 0x55 polling: {changes} | unsolicited datagrams: {other}")

if stream0 is not None:
    send(0x52, bytes([1, stream0[0]]), "set")
if buttons0 is not None:
    send(0x10, bytes([buttons0[0]]), "set")
time.sleep(0.1)
print("\nrestored: INPUT_STREAM mouse", (query(0x52, b"\x01") or b"").hex(), "| BUTTONS 0x10", (query(0x10) or b"").hex())

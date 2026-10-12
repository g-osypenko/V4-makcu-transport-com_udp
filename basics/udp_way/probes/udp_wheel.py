# Physical wheel over UDP: does fw accept INPUT_STREAM mouse with flags=1 (wheel events,
# 0x53 kind 1 id 0x20 + i16 delta, documented for V4.087 test) and do wheel events arrive?
# Unconnected socket (events come from a different board port). Restores the stream.
#   python udp_wheel.py [--host 192.168.50.175] [--seconds 10]
import argparse, select, socket, struct, time
ap = argparse.ArgumentParser(); ap.add_argument("--host", default="192.168.50.175"); ap.add_argument("--seconds", type=float, default=10)
a = ap.parse_args(); board = (a.host, 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 0))
seq = int(time.time() * 1000) << 8
def send(op, pl=b""):
    global seq; seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl, board); return seq
def query(op, pl=b"", t=0.15):
    k = send(op, pl); end = time.perf_counter() + t
    while time.perf_counter() < end:
        r, _, _ = select.select([s], [], [], 0.01)
        if r:
            d, src = s.recvfrom(2048)
            if len(d) >= 14 and struct.unpack("<Q", d[1:9])[0] == k and d[13] == op: return d[14:14 + (d[11] | d[12] << 8)]
    return None
st0 = query(0x52, b"\x01"); print("GET stream mouse:", st0.hex() if st0 else None)
send(0x52, b"\x01\x01\x01"); time.sleep(0.05)   # kind=mouse, enabled=1, flags=1 (wheel)
print("after SET 01 01 01 -> GET:", (query(0x52, b"\x01") or b"").hex())
print(f">>> scroll the wheel ({a.seconds:.0f} s)")
t0 = time.perf_counter(); wheel = buttons = other = 0; total = 0
while time.perf_counter() - t0 < a.seconds:
    r, _, _ = select.select([s], [], [], 0.05)
    if not r: continue
    d, src = s.recvfrom(2048)
    if len(d) >= 14 and d[13] == 0x53:
        p = d[14:14 + (d[11] | d[12] << 8)]
        if len(p) == 4 and p[0] == 1 and p[1] == 0x20:
            dv = struct.unpack("<h", p[2:4])[0]; wheel += 1; total += dv
            if wheel <= 12: print(f"  +{time.perf_counter()-t0:6.3f} WHEEL delta {dv:+d}  ({d[9:].hex(' ')})")
        else:
            buttons += 1
    else:
        other += 1
print(f"wheel events {wheel} (sum {total:+d}), button events {buttons}, other {other}")
if st0 is not None: send(0x52, bytes([1, st0[0]])); time.sleep(0.05)
print("restored stream:", (query(0x52, b"\x01") or b"").hex())

# How long must the client wait after a NO-REPLY command (SET 0x11 LEFT / SET 0x60 LOCK) before the
# next command is accepted? Sends the harmless SET (release left = 0 while not pressed; unlock target
# 0 while not locked), then after `gap` us a GET 0x55 (has a reply); counts how often the GET is
# answered. Also reads INJECT_SNAPSHOT 0x56 layout. Cursor does not move, nothing is clicked.
#   python udp_noreply_gap.py [--host 192.168.50.175]
import argparse, select, socket, struct, time
ap = argparse.ArgumentParser(); ap.add_argument("--host", default="192.168.50.175"); a = ap.parse_args()
board = (a.host, 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 0))
seq = int(time.time() * 1000) << 8
def send(op, pl=b""):
    global seq; seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl, board); return seq
def wait_reply(k, op, t=0.02):
    end = time.perf_counter() + t
    while time.perf_counter() < end:
        r, _, _ = select.select([s], [], [], 0.002)
        if r:
            d, _ = s.recvfrom(2048)
            if len(d) >= 14 and struct.unpack("<Q", d[1:9])[0] == k and d[13] == op: return d[14:14 + (d[11] | d[12] << 8)]
    return None
def spin(us):
    end = time.perf_counter() + us / 1e6
    while time.perf_counter() < end: pass
snap = None
k = send(0x56); snap = wait_reply(k, 0x56, 0.1)
print("INJECT_SNAPSHOT 0x56:", len(snap) if snap else None, "bytes:", snap.hex(" ") if snap else None)
k = send(0x60, b"\x00"); print("GET LOCK left:", (wait_reply(k, 0x60, 0.1) or b"").hex())
k = send(0x11); print("GET LEFT (injected):", (wait_reply(k, 0x11, 0.1) or b"").hex())
time.sleep(0.05)
for name, op, pl in [("SET LEFT=0", 0x11, b"\x00"), ("SET LOCK left=0", 0x60, b"\x00\x00")]:
    print(f"== after {name}:")
    for gap in (0, 500, 1000, 1500, 2000, 2500, 3000, 4000, 6000):
        ok = 0
        for _ in range(40):
            send(op, pl); spin(gap)
            k = send(0x55)
            if wait_reply(k, 0x55, 0.02) is not None: ok += 1
            time.sleep(0.008)  # let everything settle before the next trial
        print(f"   gap {gap:5d} us: GET answered {ok}/40")

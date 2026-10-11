# Read-only: the board's built-in transport performance test (opcode 0x3F), driven over UDP.
# Toolkit runs this over WebSocket with 62-64 records per message. Question: does ONE UDP datagram
# carrying a 0x3F batch of N records make the board count N commands? If yes, the UDP server can
# parse several records per datagram for this opcode. No mouse movement; perf counters only.
import socket, struct, sys, time
sys.stdout.reconfigure(encoding="utf-8")
T = ("192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 0))
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 400_000
def frame(op, pl): return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl
def xfer(op, pl, wait=0.05):
    global seq; seq += 1
    pkt = b"\x55" + struct.pack("<Q", seq) + frame(op, pl)
    s.sendto(pkt, T)
    end = time.perf_counter() + wait
    while time.perf_counter() < end:
        try:
            d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError):
            continue
        if len(d) >= 9 and d[0] == 0x55 and struct.unpack("<Q", d[1:9])[0] == seq:
            body = d[9:]
            if body[:2] == b"\xDE\xAD":
                ln = struct.unpack("<H", body[2:4])[0]
                return body[5:5+ln]
    return None
u32 = lambda b,o: struct.unpack_from("<I", b, o)[0]
def parse(p):
    if not p or len(p) != 63: return None
    return dict(op=p[0], active=p[1]!=0, caller=p[2], stop=p[3], received=u32(p,19),
                completed=u32(p,23), curRate=u32(p,27), peakRate=u32(p,31), avgRate=u32(p,35),
                timerHz=u32(p,7), elapsedMs=u32(p,15))
def batch(start_id, n):
    pl = bytes([6, n]) + b"".join(struct.pack("<I", (start_id+i)&0xffffffff) for i in range(n))
    return pl

print("status before:", parse(xfer(0x3F, bytes([3]))))
st = parse(xfer(0x3F, bytes([1, 0xE8, 0x03])))  # start, 1000 ms
print("start ->", st)
if not st or not st["active"]:
    print("perf test did not start over UDP; nothing more to do"); sys.exit()
cid = 1
# One datagram with a 62-record batch; measure received delta.
for n in (1, 10, 62):
    before = parse(xfer(0x3F, bytes([3])))["received"]
    r = xfer(0x3F, batch(cid, n)); cid += n
    time.sleep(0.01)
    after = parse(xfer(0x3F, bytes([3])))["received"]
    print(f"batch of {n:2d} in ONE datagram: received {before} -> {after}  (delta {after-before})  reply len {len(r) if r else None}")
# Sustained: 60 datagrams x 62 records in ~0.5 s, see received and rate.
cid += 100
base = parse(xfer(0x3F, bytes([3])))["received"]; t0 = time.perf_counter()
for i in range(60):
    while time.perf_counter() < t0 + i*0.008: pass
    xfer(0x3F, batch(cid, 62), wait=0.003); cid += 62
time.sleep(0.05)
fin = parse(xfer(0x3F, bytes([3])))
print(f"60 datagrams x 62 = 3720 submitted; received delta {fin['received']-base}, peakRate {fin['peakRate']}, avgRate {fin['avgRate']}, caller {fin['caller']}")
print("stop ->", parse(xfer(0x3F, bytes([4]))))

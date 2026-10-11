# Does ONE UDP datagram carry several raw-move records (0x69)? The board already parses 62 records
# per datagram for 0x3F; test whether 0x69 accepts a multi-record payload. Small cursor motion (net ~0).
# After each try we query every id via 0x6A(id,0) to see which entered the FIFO, then consume them.
import socket, struct, sys, time
sys.stdout.reconfigure(encoding="utf-8")
T = ("192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 0))
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 500_000
def frame(op, pl): return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl
def xfer(op, pl, wait=0.05):
    global seq; seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + frame(op, pl), T)
    end = time.perf_counter() + wait
    out = []
    while time.perf_counter() < end:
        try: d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError): continue
        if len(d) >= 9 and d[0] == 0x55 and struct.unpack("<Q", d[1:9])[0] == seq:
            body = d[9:]
            if body[:2] == b"\xDE\xAD":
                ln = struct.unpack("<H", body[2:4])[0]; return body[5:5+ln]
    return None
def meta():
    p = xfer(0x6A, b"\0"*5)
    return dict(last=struct.unpack("<I",p[2:6])[0], free=p[6]) if p and len(p)==8 else None
def rec(i, x, y): return struct.pack("<IhhH", i, x, y, 0)
def query(i):
    p = xfer(0x6A, struct.pack("<IB", i, 0))
    return (p[0], p[1]) if p and len(p)==6 else None
def consume(i):
    xfer(0x6A, struct.pack("<IB", i, 1))
m = meta(); print("meta:", m)
nid = m["last"] + 1
def trial(name, payload, ids):
    r = xfer(0x69, payload)
    st = r[:2].hex() if r else "no-reply"
    time.sleep(0.01)
    qs = {i: query(i) for i in ids}
    for i in ids: consume(i)
    print(f"{name}: 0x69 payload {len(payload)}B -> reply {st} | per-id state {qs}")
# 1: two move records concatenated (20B)
trial("two records concat", rec(nid,4,0)+rec(nid+1,-4,0), [nid, nid+1]); nid += 2
# 2: four records concatenated (40B)
trial("four records concat", b"".join(rec(nid+k, 3 if k%2==0 else -3, 0) for k in range(4)), [nid+k for k in range(4)]); nid += 4
# 3: count-prefixed [N][records]
pl = bytes([2]) + rec(nid,4,0)+rec(nid+1,-4,0)
trial("count-prefix 2", pl, [nid, nid+1]); nid += 2
# 4: single record with trailing extra record after valid 10B (len says 10, extra 10 ignored?)
r = xfer(0x69, rec(nid,4,0)+rec(nid+1,-4,0))  # same as #1 but check only first
print("note: #1 already covered multi; controls below")
# 5: control - one normal record
trial("control one record", rec(nid,5,0), [nid]); nid += 1
m2 = meta(); print("meta after:", m2)

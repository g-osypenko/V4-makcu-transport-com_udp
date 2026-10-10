#!/usr/bin/env python3
# Raw 0x69 over UDP: one move, then paced move/ack streams. MOVES THE CURSOR by +-1 (net ~0).
# Per id we learn: 0x69 reply state, ack result, and afterwards (query consume=0) whether
# the board has it -> request lost vs reply lost.
import socket, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 0

def frame(op, payload):
    return b"\xDE\xAD" + struct.pack("<H", len(payload)) + bytes([op]) + payload

def send(op, payload):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + frame(op, payload), T)
    return seq

def rx_all(out):
    while True:
        try:
            d, _ = s.recvfrom(4096)
        except BlockingIOError:
            return
        t = time.perf_counter()
        if len(d) >= 14 and d[0] == 0x55 and d[9:11] == b"\xDE\xAD":
            n = struct.unpack("<Q", d[1:9])[0]
            ln = d[11] | d[12] << 8
            out.append((t, n, d[13], d[14:14 + ln]))

def wait(sec, out):
    end = time.perf_counter() + sec
    while time.perf_counter() < end:
        rx_all(out)

def meta():
    out = []
    n = send(0x6A, b"\0\0\0\0\0")
    wait(0.05, out)
    for t, k, op, p in out:
        if k == n and op == 0x6A and len(p) == 8:
            return dict(schema=p[1], last=struct.unpack("<I", p[2:6])[0], free=p[6], cap=p[7])
    return None

def mv(i, dx, dy=0):
    return send(0x69, struct.pack("<IhhH", i, dx, dy, 0))

def ack(i, consume=1):
    return send(0x6A, struct.pack("<IB", i, consume))

m = meta()
print("meta before:", m)
if not m:
    sys.exit("no meta")
nid = m["last"] + 1

# --- step 4: single move (1,0) + ack ---
out = []
t0 = time.perf_counter(); n1 = mv(nid, 1, 0); wait(0.003, out)
n2 = ack(nid, 1); wait(0.05, out)
for t, k, op, p in out:
    st, rs, rid = p[0], p[1], struct.unpack("<I", p[2:6])[0]
    which = "0x69 reply" if k == n1 else "0x6A reply" if k == n2 else "?"
    print(f"single: {which}: op 0x{op:02X} state {st} reason {rs} id {rid} at {1e3*(t-t0):.2f} ms")
nid += 1

def stream(gap_ms, n_moves, label):
    """Alternate datagrams: move, ack(oldest due), move, ack ... one datagram per gap_ms."""
    global nid
    out, sent69, acked = [], {}, {}
    pending = []          # ids that answered state 1 and are not yet acked
    reply69 = {}          # id -> (state, reason)
    ackres = {}           # id -> (state, reason)
    dx = 1
    t0 = time.perf_counter(); k = 0; moves = 0
    seq2id = {}
    while moves < n_moves or pending:
        tgt = t0 + k * gap_ms / 1000
        while time.perf_counter() < tgt:
            rx_all(out)
            for t, sq, op, p in out:
                if sq in seq2id and len(p) == 6:
                    kind, i = seq2id.pop(sq)
                    st, rs = p[0], p[1]
                    if kind == "mv":
                        reply69[i] = (st, rs)
                        if st == 1: pending.append(i)
                    else:
                        ackres[i] = (st, rs)
                        if st == 255 and rs == 6: pending.append(i)  # still queued: re-ack later
            out.clear()
        if time.perf_counter() - t0 > 5: break
        if k % 2 == 0 and moves < n_moves:
            i = nid; nid += 1
            seq2id[mv(i, dx, 0)] = ("mv", i); sent69[i] = time.perf_counter()
            dx = -dx; moves += 1
        elif pending:
            i = pending.pop(0)
            seq2id[ack(i, 1)] = ("ack", i); acked[i] = True
        k += 1
    wait(0.1, out)
    for t, sq, op, p in out:
        if sq in seq2id and len(p) == 6:
            kind, i = seq2id.pop(sq)
            (reply69 if kind == "mv" else ackres)[i] = (p[0], p[1])
    dur = time.perf_counter() - t0
    # post-mortem: ids without a 0x69 reply -> does the board know them?
    noreply = [i for i in sent69 if i not in reply69]
    exists, missing = 0, 0
    for i in noreply:
        o = []; q = ack(i, 0); wait(0.004, o)
        r = [p for t, sq, op, p in o if sq == q and len(p) == 6]
        if r and r[0][0] in (1, 2, 3):
            exists += 1
            o2 = []; ack(i, 1); wait(0.004, o2)
        else:
            missing += 1
    # also: acked ids whose ack reply was lost -> query
    ack_lost = [i for i in acked if i not in ackres]
    still = 0
    for i in ack_lost:
        o = []; q = ack(i, 0); wait(0.004, o)
        r = [p for t, sq, op, p in o if sq == q and len(p) == 6]
        if r and r[0][0] in (1, 2, 3):
            still += 1; o2 = []; ack(i, 1); wait(0.004, o2)
    st1 = sum(1 for v in reply69.values() if v[0] == 1)
    other = {}
    for v in reply69.values():
        if v[0] != 1: other[v] = other.get(v, 0) + 1
    ok2 = sum(1 for v in ackres.values() if v[0] == 2)
    print(f"[{label}] gap {gap_ms} ms, {dur:.2f} s: 0x69 sent {len(sent69)}, replied {len(reply69)} "
          f"(state1 {st1}, other {other}); no reply {len(noreply)} -> board HAS {exists} (reply lost), "
          f"NOT {missing} (request lost)")
    print(f"     acks sent {len(acked)}, replied {len(ackres)} (state2 {ok2}), ack reply lost {len(ack_lost)} "
          f"-> still on board {still}; moves/s accepted ~{(st1 + exists) / dur:.0f}")

stream(4.0, 200, "slow")
time.sleep(0.2)
stream(2.0, 300, "medium")
time.sleep(0.2)
stream(1.0, 500, "fast")
time.sleep(0.2)
print("meta after:", meta())

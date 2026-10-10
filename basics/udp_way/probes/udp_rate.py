#!/usr/bin/env python3
# Step 0.5b: loss vs send interval; multi-frame datagrams. 0x6A(id=0) only - cursor does not move.
import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175"
T = (HOST, 8080)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 1000

def rx_all(got):
    while True:
        try:
            d, a = s.recvfrom(4096); got.append((time.perf_counter(), d))
        except BlockingIOError:
            return

def run(interval_ms, n, frames=1):
    global seq
    sent, got = {}, []
    t0 = time.perf_counter()
    for i in range(n):
        tgt = t0 + i * interval_ms / 1000
        while time.perf_counter() < tgt: rx_all(got)
        seq += 1
        sent[seq] = time.perf_counter()
        s.sendto(b"\x55" + struct.pack("<Q", seq) + META * frames, T)
    end = time.perf_counter() + 0.2
    while time.perf_counter() < end: rx_all(got)
    seqs, frames_in_reply, rtt = [], [], []
    for t, d in got:
        q = struct.unpack("<Q", d[1:9])[0] if len(d) >= 9 and d[0] == 0x55 else -1
        if q in sent:
            seqs.append(q); rtt.append((t - sent[q]) * 1e3)
            frames_in_reply.append(d[9:].count(b"\xDE\xAD\x08\x00\x6A"))
    lost = n - len(set(seqs))
    # longest run of consecutive lost seq
    ok = set(seqs); run_ = best = 0
    for k in sorted(sent):
        run_ = 0 if k in ok else run_ + 1; best = max(best, run_)
    rtt.sort()
    p = lambda x: rtt[min(len(rtt)-1, int(x*len(rtt)))] if rtt else float('nan')
    print(f"interval {interval_ms:5.2f} ms frames/dgram {frames}: sent {n} replied {len(set(seqs))} lost {lost} "
          f"({100*lost/n:.1f}%) max_lost_run {best} dup {len(seqs)-len(ok)} | frames per reply "
          f"{sorted(set(frames_in_reply))} | RTT p50 {p(.5):.2f} p99 {p(.99):.2f} max {rtt[-1] if rtt else 0:.2f}")

for iv in (5.0, 3.0, 2.5, 2.0, 1.75, 1.5, 1.25, 1.0, 0.5):
    run(iv, 500)
    time.sleep(0.2)
print("--- multi-frame datagrams ---")
for fr in (2, 3, 4):
    run(5.0, 100, fr)
    time.sleep(0.2)
run(2.0, 500, 2)
s.close()

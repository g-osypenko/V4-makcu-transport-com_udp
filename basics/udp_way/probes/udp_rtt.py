#!/usr/bin/env python3
# Шаг 0.5: RTT, потери, порядок ответов на 0x6A(id=0). Курсор не двигается.
import os, socket, struct, sys, time, statistics as st

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
T = (HOST, PORT)
META = b"\xDE\xAD\x05\x00\x6A\0\0\0\0\0"

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try:
    s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception:
    pass
s.setblocking(False)

def pkt(seq):
    # nonce = порядковый номер (8 байт), чтобы видеть порядок ответов
    return b"\x55" + struct.pack("<Q", seq) + META

def drain(until):
    got = []
    while time.perf_counter() < until:
        try:
            d, a = s.recvfrom(2048)
            got.append((time.perf_counter(), d, a))
        except BlockingIOError:
            pass
    return got

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))]

def report(name, sent_t, got, n):
    rtt, seqs, other = [], [], 0
    for t, d, a in got:
        if len(d) == 22 and d[0] == 0x55 and d[9:11] == b"\xDE\xAD" and d[13] == 0x6A:
            q = struct.unpack("<Q", d[1:9])[0]
            if q in sent_t:
                seqs.append(q)
                rtt.append((t - sent_t[q]) * 1e3)
                continue
        other += 1
    uniq = set(seqs)
    reord = sum(1 for i in range(1, len(seqs)) if seqs[i] < seqs[i - 1])
    print(f"\n[{name}] отправлено {n}, ответов {len(seqs)} (уникальных {len(uniq)}, дублей {len(seqs)-len(uniq)}),"
          f" потеряно {n-len(uniq)}, перестановок {reord}, посторонних пакетов {other}")
    if rtt:
        print(f"   RTT мс: min {min(rtt):.3f} p50 {pct(rtt,50):.3f} p90 {pct(rtt,90):.3f}"
              f" p99 {pct(rtt,99):.3f} max {max(rtt):.3f} mean {st.mean(rtt):.3f}")
    return rtt

seq = 0
# 1) пинг-понг: по одному запросу, ждём ответ
sent, got = {}, []
for i in range(200):
    seq += 1
    sent[seq] = time.perf_counter()
    s.sendto(pkt(seq), T)
    g = []
    end = time.perf_counter() + 0.05
    while time.perf_counter() < end:
        try:
            d, a = s.recvfrom(2048)
            g.append((time.perf_counter(), d, a))
            break
        except BlockingIOError:
            pass
    got += g
got += drain(time.perf_counter() + 0.1)
r1 = report("пинг-понг x200", sent, got, 200)

# 2) пачка: 100 запросов подряд без пауз
sent, base = {}, seq
for i in range(100):
    seq += 1
    sent[seq] = time.perf_counter()
    s.sendto(pkt(seq), T)
got = drain(time.perf_counter() + 0.3)
report("пачка 100 подряд", sent, got, 100)

# 3) темп 1 мс (как горячий путь): 2000 запросов за 2 с, приём параллельно
sent, got = {}, []
t0 = time.perf_counter()
for i in range(2000):
    tgt = t0 + i * 0.001
    while True:
        now = time.perf_counter()
        try:
            d, a = s.recvfrom(2048)
            got.append((now, d, a))
            continue
        except BlockingIOError:
            pass
        if now >= tgt:
            break
    seq += 1
    sent[seq] = time.perf_counter()
    s.sendto(pkt(seq), T)
got += drain(time.perf_counter() + 0.2)
late = [ (sent[k] - (t0 + (k - min(sent)) * 0.001)) * 1e3 for k in sent ]
print(f"\n   опоздание отправки от сетки, мс: p50 {pct(late,50):.3f} p99 {pct(late,99):.3f} max {max(late):.3f}")
report("темп 1 мс x2000", sent, got, 2000)
s.close()

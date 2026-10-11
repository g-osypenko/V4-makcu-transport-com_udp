#!/usr/bin/env python3
# Read-only: network status via MAK_API 0x3D (layout from the Toolkit bundle) and the UDP server cost
# of host-chip-local requests (0x3D) vs requests that may go to the device chip (0x6A, 0x02).
# Cursor does not move. Nothing is written.
import socket, statistics, struct, sys, time

T = (sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 9_000_000


def frame(op, pl=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl


def raw(body):
    global seq
    seq += 1
    return seq, b"\x55" + struct.pack("<Q", seq) + body


def poll(got):
    while True:
        try:
            d, _ = s.recvfrom(4096)
        except (BlockingIOError, ConnectionResetError):
            return
        if len(d) >= 9 and d[0] == 0x55:
            got[struct.unpack("<Q", d[1:9])[0]] = (time.perf_counter(), d[9:])


def wait(until, got):
    while time.perf_counter() < until:
        poll(got)


def q(op, pl=b"", t=0.05):
    got = {}
    k, p = raw(frame(op, pl)); s.sendto(p, T)
    wait(time.perf_counter() + t, got)
    if k not in got:
        return None
    d = got[k][1]
    return d[5:5 + struct.unpack("<H", d[2:4])[0]] if d[:2] == b"\xDE\xAD" else d


u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]
ip = lambda b, o: ".".join(str(x) for x in struct.pack(">I", u32(b, o)))
mac = lambda b, o: ":".join(f"{x:02X}" for x in b[o:o + 6])

st = q(0x3D)
print("0x3D status len", None if st is None else len(st))
if st and len(st) == 62:
    f = st[0]
    print(f"  flags 0x{f:02X}: eth_present {f & 1} esp_hosted {(f >> 1) & 1} wifi_sup {(f >> 2) & 1} ble_sup {(f >> 3) & 1}"
          f" wifi_conn {(f >> 5) & 1} ap_ready {(f >> 6) & 1}")
    print(f"  ethernet source {st[1]} mode {st[2]} selected_mode {st[3]} (0 device, 1 host, 2 auto) speed code {st[4]}"
          f" dhcp_bound {st[5]} enabled {st[44]}")
    print(f"  eth ipv4 {ip(st, 49)} mac {mac(st, 53)} rx {u32(st, 30)} tx {u32(st, 34)} vlan {st[59]} tag {st[60] | st[61] << 8}")
    print(f"  wifi enabled {st[45]} state {st[6]} ble enabled {st[46]} state {st[8]}")
    print("  raw", st.hex(" "))
ex = q(0x3D, bytes([13]))
if ex and len(ex) == 61 and ex[0] == 13:
    t = ex[2]
    print(f"0x3D[13]: address mode {ex[1]} flags 0x{t:02X} internet {(t >> 4) & 1} webui_disabled {(t >> 5) & 1}"
          f" netmask {ip(ex, 21)} gw {ip(ex, 25)} dns {ip(ex, 29)}")
else:
    print("0x3D[13]:", None if ex is None else ex.hex(" "))
for netif in (0, 1):
    r = q(0x3D, bytes([22, netif]))
    if r and len(r) == 37:
        fl = r[2]
        print(f"0x3D[22,{netif}]: present {fl & 1} enabled {(fl >> 1) & 1} link {(fl >> 2) & 1} dhcp {(fl >> 3) & 1}"
              f" ip {ip(r, 3)} mask {ip(r, 7)} gw {ip(r, 11)} mac {mac(r, 19)} rx {u32(r, 25)} tx {u32(r, 29)}")
    else:
        print(f"0x3D[22,{netif}]:", None if r is None else r.hex(" "))
p = q(0x3D, bytes([21]))
print("0x3D[21] udp port:", None if p is None else (p.hex(" "), (p[1] | p[2] << 8) if len(p) == 3 else None))


def rx_counter():
    r = q(0x3D, bytes([22, 0]))
    return u32(r, 25) if r and len(r) == 37 else None


# Does every datagram reach the netif? Send 1000 queries at 1 ms, compare netif RX delta with replies.
time.sleep(0.05)
r0 = rx_counter()
got = {}
sent = []
t0 = time.perf_counter()
for i in range(1000):
    wait(t0 + i / 1000, got)
    k, pk = raw(frame(0x6A, b"\0" * 5)); s.sendto(pk, T); sent.append(k)
wait(time.perf_counter() + 0.1, got)
r1 = rx_counter()
print(f"1000 x 0x6A @1 ms: replied {sum(1 for k in sent if k in got)}, netif RX frames delta {None if r0 is None or r1 is None else r1 - r0}"
      f" (includes the 1 status query)")

# Server cost of 0x3D (host-chip local) vs 0x6A
base = []
for _ in range(30):
    got = {}
    k, pk = raw(frame(0x6A, b"\0" * 5)); t = time.perf_counter(); s.sendto(pk, T)
    wait(t + 0.006, got)
    if k in got: base.append((got[k][0] - t) * 1e3)
idle = statistics.median(base)
print(f"idle RTT 0x6A {idle:.3f} ms")
for name, body in (("0x3D status", frame(0x3D)), ("0x3D[21] port", frame(0x3D, bytes([21]))),
                   ("0x3D[22,0] netif", frame(0x3D, bytes([22, 0]))), ("0x6A meta", frame(0x6A, b"\0" * 5)),
                   ("0x3F[3] perf status", frame(0x3F, bytes([3])))):
    rx = []
    for g in (300, 600, 900, 1200):
        acc = 0; rt = []; xr = []
        for _ in range(25):
            got = {}
            kx, px = raw(body); t0 = time.perf_counter(); s.sendto(px, T)
            wait(t0 + g / 1e6, got)
            kb, pb = raw(frame(0x6A, b"\0" * 5)); tb = time.perf_counter(); s.sendto(pb, T)
            wait(time.perf_counter() + 0.008, got)
            if kx in got: xr.append((got[kx][0] - t0) * 1e3)
            if kb in got:
                acc += 1; rt.append((got[kb][0] - tb) * 1e3)
            wait(time.perf_counter() + 0.003, got)
        r = statistics.median(rt) if rt else float("nan")
        rx.append(f"g{g}: B {acc:2d}/25 busy~{(g / 1e3 + r - idle) if rt else float('nan'):4.2f}")
    print(f"{name:20s} RTT_X {statistics.median(xr) if xr else float('nan'):5.2f} | " + " | ".join(rx))
s.close()

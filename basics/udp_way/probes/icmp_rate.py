#!/usr/bin/env python3
# ICMP echo at fixed interval via async IcmpSendEcho2 (no admin). Isolates board network stack from UDP app.
import ctypes, ctypes.wintypes as wt, socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.175"
ip = struct.unpack("<I", socket.inet_aton(HOST))[0]
iph = ctypes.WinDLL("iphlpapi")
k32 = ctypes.WinDLL("kernel32")
iph.IcmpCreateFile.restype = wt.HANDLE
iph.IcmpSendEcho2.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                              ctypes.c_void_p, wt.WORD, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, wt.DWORD]
iph.IcmpParseReplies.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.CreateEventW.restype = wt.HANDLE
h = iph.IcmpCreateFile()
data = ctypes.create_string_buffer(b"x" * 32, 32)

def run(interval_ms, n):
    evs, bufs = [], []
    t0 = time.perf_counter()
    for i in range(n):
        tgt = t0 + i * interval_ms / 1000
        while time.perf_counter() < tgt: pass
        ev = k32.CreateEventW(None, True, False, None)
        buf = ctypes.create_string_buffer(256)
        iph.IcmpSendEcho2(h, ev, None, None, ip, data, 32, None, buf, 256, 1000)
        evs.append(ev); bufs.append(buf)
    dur = time.perf_counter() - t0
    time.sleep(1.2)
    ok, rtts = 0, []
    for ev, buf in zip(evs, bufs):
        if k32.WaitForSingleObject(ev, 0) == 0:
            cnt = iph.IcmpParseReplies(buf, 256)
            status = struct.unpack_from("<I", buf.raw, 4)[0]
            if cnt and status == 0:
                ok += 1; rtts.append(struct.unpack_from("<I", buf.raw, 8)[0])
        k32.CloseHandle(ev)
    print(f"ICMP interval {interval_ms:.2f} ms: sent {n} in {dur*1e3:.0f} ms (={n/dur:.0f}/s) replied {ok} lost {n-ok} ({100*(n-ok)/n:.1f}%)"
          f" rtt_ms max {max(rtts) if rtts else '-'}")

for iv in (5.0, 2.0, 1.5, 1.0, 0.5):
    run(iv, 500)

# Headline number: records/s the board accepts over UDP when each datagram carries a 62-record 0x3F batch,
# sent as fast as the datagram server allows. Read-only perf counters; no mouse movement.
import socket, struct, sys, time
sys.stdout.reconfigure(encoding="utf-8")
T = ("192.168.50.175", 8080)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", 0))
try: s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception: pass
s.setblocking(False)
seq = 600_000
def frame(op, pl): return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl
def xfer(op, pl, wait=0.05):
    global seq; seq += 1
    s.sendto(b"\x55"+struct.pack("<Q",seq)+frame(op,pl), T)
    end=time.perf_counter()+wait
    while time.perf_counter()<end:
        try: d,_=s.recvfrom(4096)
        except (BlockingIOError,ConnectionResetError): continue
        if len(d)>=9 and d[0]==0x55 and struct.unpack("<Q",d[1:9])[0]==seq:
            b=d[9:]
            if b[:2]==b"\xDE\xAD":
                ln=struct.unpack("<H",b[2:4])[0]; return b[5:5+ln]
    return None
u32=lambda b,o: struct.unpack_from("<I",b,o)[0]
def status():
    p=xfer(0x3F, bytes([3]))
    return dict(active=p[1]!=0, received=u32(p,19), peak=u32(p,31), avg=u32(p,35)) if p and len(p)==63 else None
def batch(cid,n): return bytes([6,n])+b"".join(struct.pack("<I",(cid+i)&0xffffffff) for i in range(n))
xfer(0x3F, bytes([4]))  # stop any prior
st=xfer(0x3F, bytes([1,0xE8,0x03]))  # start 1000 ms
if not st or st[1]==0: print("perf test did not start"); sys.exit()
cid=1; sent_dg=0
t0=time.perf_counter()
# blast 62-record batches ~every 1.7 ms (datagram-limit pace), no reply to wait on
for i in range(560):
    while time.perf_counter() < t0 + i*0.0017: pass
    seq+=1; s.sendto(b"\x55"+struct.pack("<Q",seq)+frame(0x3F, batch(cid,62)), T); cid+=62; sent_dg+=1
dur=time.perf_counter()-t0
time.sleep(0.08)
fin=status()
print(f"sent {sent_dg} datagrams x 62 = {sent_dg*62} records in {dur*1e3:.0f} ms ({sent_dg/dur:.0f} dgram/s)")
print(f"board received {fin['received']} records = {fin['received']/dur:.0f} records/s | peakRate {fin['peak']}/s avgRate {fin['avg']}/s")
print(f"vs single-frame UDP ceiling ~581 frames/s -> batching multiplier ~{fin['received']/dur/581:.0f}x")
xfer(0x3F, bytes([4]))

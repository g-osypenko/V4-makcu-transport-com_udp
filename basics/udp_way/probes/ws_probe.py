# Targeted check (not a scan): is the board's HTTP/WebSocket server reachable on the two ports the
# vendor's own software uses (80, 8080/TCP)? Proper HTTP/1.1 WebSocket upgrade to /ws. Read-only.
import base64, os, socket, sys, time
sys.stdout.reconfigure(encoding="utf-8")
H = "192.168.50.175"
def ws(port):
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /ws HTTP/1.1\r\n"
           f"Host: {H}:{port}\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM); s.settimeout(2.5)
    t0 = time.perf_counter()
    try:
        s.connect((H, port))
    except Exception as e:
        print(f"TCP {port}: no connect after {time.perf_counter()-t0:.2f}s ({type(e).__name__})"); s.close(); return
    print(f"TCP {port}: connected in {(time.perf_counter()-t0)*1e3:.0f} ms -> sending WS upgrade")
    try:
        s.sendall(req); data = s.recv(2048)
        print(f"  reply {len(data)} B:\n  " + data[:400].decode('latin1').replace('\r\n','\n  '))
    except Exception as e:
        print(f"  no reply ({type(e).__name__}: {e})")
    s.close()
for p in (80, 8080):
    ws(p)

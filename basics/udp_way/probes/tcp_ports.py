# Targeted TCP probe of ONE host (the board), user-approved. Find any HTTP/WebSocket listener.
import socket, sys, threading, time
sys.stdout.reconfigure(encoding="utf-8")
H = "192.168.50.175"
ports = [80,81,443,554,1880,2000,3000,3333,4000,5000,5001,6000,7000,7681,7682,8000,8001,8008,8080,8081,8088,8443,8888,9000,9001,9002,9090,9100,10000,11000,8266,3232]
open_ports = []; results = {}
def check(p):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM); s.settimeout(1.2)
    t0 = time.perf_counter()
    try:
        s.connect((H, p)); results[p] = "OPEN"; open_ports.append(p)
    except socket.timeout:
        results[p] = "filtered"
    except ConnectionRefusedError:
        results[p] = "refused"
    except Exception as e:
        results[p] = type(e).__name__
    finally:
        s.close()
ths = [threading.Thread(target=check, args=(p,)) for p in ports]
for t in ths: t.start()
for t in ths: t.join()
print("OPEN:", sorted(open_ports) or "none")
ref = [p for p in ports if results[p]=="refused"]
print("refused (host up, port closed):", sorted(ref) or "none")
print("filtered/timeout:", len([p for p in ports if results[p]=="filtered"]), "ports")

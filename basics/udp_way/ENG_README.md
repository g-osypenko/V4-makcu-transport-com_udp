# MAKCU mover (UDP): Raw 0x69/0x6A over the network

*English translation of [`README.md`](README.md) (Russian original). Links point to the English (`ENG_`) versions of the documents where one exists. Source-code comments and program output are still in Russian.*

> **Current as of 2026-10-12: 1000 Hz over UDP has been reached** on V4.097 (the official firmware since 2026-10-11) with the batch command `0x6C`.
> The reference transport: [`perfect-transport-udp/`](perfect-transport-udp/ENG_README.md). In the mover this is
> `--tx batch` (the default on firmware ≥ 4097). Measurements and answers from the MAKCU engineers:
> [`MAKCU_RESPONSE_UDP.md`](MAKCU_RESPONSE_UDP.md) (already in English). Below is the history on firmware 4094.

**Status as of 2026-10-10 (evening): the transport works, Raw over UDP ~283 Hz, the path is exact. 1000 Hz over
UDP on firmware 4094 was not found by any method.**
The Raw path over UDP delivers each report 1:1. The counters "sent = in the FIFO = handed to the
USB endpoint" agree, rejections 0, the path matches to the count, even with 25 % of datagrams
lost. With the reply-clocked pace (the default) the HID rate is **282–283 Hz** (with the bucket it was 250).
The limit is on the board's side. Its UDP handler spends **~1.72 ms on any valid frame** and
accepts exactly one frame per datagram, so no more than ~581 commands/s. Raw spends two
datagrams per report (the movement and the receipt), a ceiling of ≈ 290 Hz. The network loses nothing: the RX counter
of the board's network interface grows by every datagram sent. Below are the numbers, the intake model and
everything that was tried to get around it (sections 2, 7).

The COM variant with 1000 Hz: [`../comport_way/ENG_README.md`](../comport_way/ENG_README.md). This test bench
is built the same way and uses its shared headers.

---

## 1. The pipeline

```
 motion source (Trajectory / ABCurves / accumulator) - as in comport_way
        │  every 1 ms: a whole report (dx,dy), often (0,0)
        ▼
 runMotion (../comport_way/motion_engine.h): a 1 ms QPC grid, catch-up, carry-over
        │  move(dx,dy): false = the transport is busy, the delta accumulates at the caller
        ▼
 RawUdpTransport (makcu_udp_transport.h)
        │  move(): the delta goes into the transport's carry-over (reply-clocked pace)
        │  pace: one datagram in flight, the next - right on the reply (the receive thread)
        │  0x69 with the accumulated delta, the 0x6A receipt in a separate datagram
        │  datagram = 0x55 | nonce:8 | DE AD LEN CMD PAYLOAD (exactly one frame)
        ▼  Ethernet -> switch -> a USB hub with Ethernet in MAKCU's host port
 MAKCU fw 4094: UDP handler ~1.72 ms per frame (~581/s) -> Raw FIFO -> one HID report per 0x69
        ▼  USB, 1 ms polling
 Windows: ~283 reports/s, exact path
```

HID rate = the number of `0x69` sent, and they are no more than half of the datagrams: ~566 datagrams/s / 2
≈ **283/s** (with the bucket `--pace bucket`: 500 / 2 = 250). The tick deltas between sends are merged into the
next report, the path is not lost.

## 2. What was found out about UDP (step 0, everything measured on this board)

Network: PC `192.168.50.87` (cable, switch), board `192.168.50.175:8080`, ping < 1 ms.

| Question | Result |
| --- | --- |
| Wrapper | **RAW**: `0x55` + an 8-byte nonce + a MAK_API frame. The board returns the same nonce. A bare frame (HOST without encryption) gets no reply. Encryption is not enabled |
| DEVICE / FIRMWARE / `0x1F` | `0x03` (mouse + keyboard) / **4094** / interpolation 0 |
| `0x6A(id=0)` | schema 1, capacity 16. After a board reboot `last_accepted_id` = 0 |
| Step 4: `0x69(1,0)` + `0x6A` | state 1 after 1.11 ms, the receipt after 3 ms: **state 2** (handed to USB) |
| RTT | 1.05 ms p50, 1.15 ms p99 at a rate ≤ 500/s. Reorderings 0, duplicates 0 |
| Several frames in a datagram | 2–4 frames, an `MBAT` packet from the SDK (BLE), a record without `DE AD`: **no reply**, the datagram is dropped whole |
| KM text (`0x6B` = `'k'`) | works: `m.version()` → `km.MAKCU`. Two commands in one frame (CRLF or `;`) give `ERR`. The opcode byte in the reply is the first letter of the reply text |
| Reply-less SETs (`0x16`, `0x18`) | also occupy a receive slot: at 1000 datagrams/s the board still processes ~580 |
| Several source sockets | 1, 2 or 4 ports: the same ~580/s in total |
| Button/input streams (`0x10`, `0x52`) | off, no effect on intake |
| What is lost on overload | **requests** (before processing): polling `0x6A(id, consume=0)` says "no such id". There were no lost replies to accepted requests |

### Where the bottleneck is (numbers)

The board's intake (`makcu_udp_mover.exe --netprobe`, `0x6A(id=0)` requests, the cursor stands still):

| datagram interval | lost | replies/s | RTT p50 / p99, ms |
| --- | --- | --- | --- |
| 3000 µs | 0 % | 334 | 1.04 / 1.18 |
| 2000 µs | 0 % | 502 | 1.04 / 1.10 |
| 1800 µs | 0 % | 557 | 1.05 / 1.16 |
| 1500 µs | 16 % | 560 | 1.48 / 1.96 |
| 1000 µs | 42 % | 585 | 1.56 / 2.07 |
| 500 µs | 71 % | ~580 | 1.81 / 2.13 |

| Link | Check | Conclusion |
| --- | --- | --- |
| PC | send lateness from the grid p99 0.027 ms; bench tick lateness 99.3–99.5 % < 50 µs, max 132–182 µs (as with COM) | not a limit |
| Network + USB-Ethernet + the board's network stack | ICMP echo **2004 packets/s: 500 of 500**, RTT < 1 ms | not a limit |
| The board's UDP handler | any opcode, any size, any number of sockets: a ceiling of **~583 datagrams/s**, 1.72 ms between replies, the excess is dropped before processing | **the bottleneck** |
| Raw API | 2 datagrams per report (`0x69` + the receipt `0x6A consume=1`, otherwise the space runs out after 16 records) | 583 / 2 ≈ 290 reports/s in theory |

### The board's intake model (a re-check, evening of 2026-10-10)

All probes only read (`0x6A(id=0)` etc.), the cursor does not move. Run with: `python -I probes\<name>.py`.

| Fact | Numbers | Probe |
| --- | --- | --- |
| The network delivers **everything** | 1000 datagrams at a 1 ms step: the board's Ethernet interface RX counter (`0x3D [22,0]`) +1018, replies 581. The UDP handler drops, not Ethernet/USB/lwIP | `udp_netstat.py` |
| Cycle per valid frame | 1.60–1.85 ms, peak 1.72, not tied to the 1 ms grid. At saturation 576–585 replies/s | `udp_cycle_hist.py` |
| The drop window | B at 0.50–0.55 ms after A: 0 of 60; 0.60: 1; 0.65: 3; 0.70: 30; 0.75: 57; ≥ 0.80: all. From another socket the same | `udp_slot.py` |
| A slot for one datagram | an accepted B is served exactly 1.72 ms after A (reply B − reply A = 1.70–1.74 at any gap 0.8–1.7 ms). A third (C), while B waits or in B's drop window, is lost | `udp_slot.py` |
| The cost does not depend on the opcode | `0x6A`, `0x02`, `0x04`, `0x3F`, SET `0x16` without a reply, unknown `0xEE` (reply `FF`), KM text `0x6B` (`m.version()`, `km.move(0,0)`): 1.68–1.81 ms. `0x3D` (network status) is dearer: RTT 1.96 ms. A frame with 200 bytes of payload: ~2.1 ms (~1.7 µs/byte) | `udp_cycle.py`, `udp_netstat.py` |
| Broken datagrams are free | an extra byte, padding, a wrong signature, a bare frame (HOST), a header only: the next one is served at once (RTT 1.04). 1400 bytes of garbage delay it by ~1.4 ms (a transfer over USB FS) | `udp_cycle.py` |
| Exactly one frame per datagram | accepted only if the length = 9 + 5 + LEN. 2/4/10/100 complete `55 nonce frame` records, padding up to 4/32/64 bytes, a trailing `0x55`, IP fragmentation (1900–2000 bytes): not a single reply | `udp_multirec.py` |
| TCP | port 80: a 3 s timeout (neither SYN-ACK nor RST). No WebUI/WebSocket over the network | one attempt, with permission |

Conclusion: the handler is stop-and-wait with a fixed part of ~1 ms after the reply; the frame apparently goes
across a bridge to the device chip (the cost grows with the frame size, the network status `0x3D` is dearer than
the rest). This is a hypothesis about the internal design; the numbers above are facts.

### Important: the transport CAN batch ("one frame per datagram" is wrong as a general conclusion)

The limit of ~581/s is about single frames with a reply. The built-in performance test (`0x3F`)
accepts **several records in one datagram**: `0x3F 06 <count> <id u32 × count>`, count up to 62.

| Probe | Result |
| --- | --- |
| A batch of N records in ONE datagram increases the accepted counter by exactly N | `udp_perf.py`: delta 1 / 10 / 62 |
| Batches of 62 at the datagram limit | **34,643 records/s** (589 datagrams/s × 62, peak 35,109/s) = ~60× of 581 | `udp_perf_max.py` |
| `0x3F` is only a counter (it produces no HID) | movement cannot be done that way |
| `0x69` strictly 10 bytes of payload: 20/40/21 bytes → `ff 01` (reason 1) | a movement batch is not accepted | `udp_movebatch.py` |

So the bottleneck for 1000 Hz is **not the transport** (it carries 34,000 records/s), but the fact that the movement
opcode `0x69` takes one record per datagram and requires a receipt (another datagram).

### A route without firmware: WebSocket `/ws` (unavailable so far)

In the firmware 4076 image (public, `app.makcu.com/makcu-fw`) the host chip has an HTTP + WebSocket
server (`GET /ws`, `101 Switching Protocols`). The Toolkit runs the network test exactly through
`ws(s)://host/ws`, 4 sessions of 64 records per frame (~38k/s on the demo). If ordinary commands pass through `/ws`,
this is a path to 1000 Hz past the datagram limit without touching the firmware. But over Ethernet
the board does not serve it: ICMP and UDP 8080 work, while **all 32 typical TCP ports time out, not a single
`refused`** (`tcp_ports.py`, `ws_probe.py`). TCP on this interface is "dark". To open it — the
Geek button (hold ~5 min, by its description it opens the network WebUI) or over another interface.

Two routes to a clean 1000 Hz over UDP on 4094: (1) reach `/ws` (no firmware, but TCP has to be
opened); (2) the vendor enables for `0x69` the same batch that already works for `0x3F` (a tiny
change; 34,643 records/s prove the transport is ready). The request: `MAKCU_REQUEST_UDP.md`.

The board's network settings (`0x3D`, format from the Toolkit bundle app.makcu.com, read-only):
Ethernet mode Auto, Device selected (a DHCP client; Host is a direct connection, the board hands out an address),
DHCP, UDP port 8080, VLAN off, WebUI access not forbidden by a flag, internet off,
there is no Wi-Fi/BLE/ESP-Hosted on this board. There is no "UDP wrapper HOST/RAW" mode in the settings.
The opcode map from the Toolkit: `0x08` = API_REBOOT (do not send!), `0x0C` = the AES key and encryption flags
(the key is set once and forever), `0x3D` = Ethernet, `0x3E` = CONNECTION, `0x3F` = the performance
test (packs up to 62–64 records into one frame, a "caller" field: 1 Ethernet, 2 Wi-Fi, 3 COM, 4 BLE).

## 3. Files

| File | What is inside |
| --- | --- |
| `makcu_udp_transport.h` | `RawUdpTransport : mover::Transport` - a non-blocking UDP socket, its own receive thread (`select`), `SIO_UDP_CONNRESET` off, nonce = the datagram number (the reply is matched to the request by it, RTT and losses are counted), a reply-clocked pace (`Pace::Reply`) or a bucket (`Pace::Bucket`), formats `RawMove` (0x69/0x6A), `Mak` (0x18), `MoveNow` (0x67), `query`/`kmQuery`/`getInterp`/`setInterp`, `prepareRaw` (metadata, cleanup, window), `probeIntake` (intake measurement) |
| `probes/*.py` | probes of the board (Python, no dependencies): intake and its model (`udp_rate`, `udp_dead`, `udp_slot`, `udp_cycle`, `udp_cycle_hist`), network status and the RX counter (`udp_netstat`), datagram formats (`udp_batch`, `udp_multirec`, `udp_text`), Raw (`udp_raw69`, `udp_raw_cost` - they move the cursor by ±1), ICMP (`icmp_rate`) |
| `makcu_udp_mover.cpp` | the test bench: CLI, a connection and Raw check at start, an F-key menu, format comparison, sweep, `--check`/`--netprobe` |
| `build.bat` | MinGW g++ C++23, WinAPI + ws2_32 only (+ ABCurves C99). No SDK needed |

The shared headers are taken from `..\comport_way` via the include path, without copying: `motion_engine.h`,
`substepper.h`, `abc_renderer.h`, `hook_monitor.h`, `rawinput_monitor.h`, `rate_probe.h`,
`mover_common.h`. For this the `Transport` interface was split out of `makcu_transport.h` into
`..\comport_way\mover_transport.h` without the SDK. This is the only change to the COM bench: `motion_engine.h`
includes it instead of `makcu_transport.h`. `comport_way\build.bat` builds as before after the change.

## 4. The Raw logic over UDP (what differs from COM)

The basis is taken from `RawComTransport`: ids strictly increase, a window ≤ 12 records (≤ free − 2), a receipt
no earlier than 3 ms, carry-over of rejected deltas, cleanup of records of past sessions, `finish()`. The differences
for datagrams:

- **Reply-clocked pace (`--pace reply`, the default).** Exactly one datagram in flight.
  The next goes out right on the reply to the previous one: the receive thread sends it itself, between
  ticks. If there is no reply for `kReplyWaitUs` (3 ms), the datagram goes out anyway; the loss is handled by the
  recovery below. The reply comes after ~1.05 ms, that is, after the drop window (~0.65 ms) but
  before the end of the board's cycle (1.72 ms). The new datagram waits in the "slot", the board works with no idle time:
  ~566 datagrams/s with no losses. `move()` always accepts the delta into the transport's carry-over and
  sends it if the reply to the datagram in flight has already come. Otherwise the delta goes out with the
  next 0x69 send from the receive thread. If there is no reply, the receive thread wakes up once per
  1 ms and sends the receipts that have matured. Service requests (`query`) wait for the same permission.
- **Bucket (`--pace bucket`, and always for 0x18/0x67 - they have no reply).** On average one
  datagram per `--gap-us` (1800 µs), capacity `--burst` (1). On a 1 ms tick grid this is exactly
  2 ms (500/s). `move()` returns `false` while the pace does not allow, and never blocks.
- **Order.** First the new delta, the receipt - when the window is almost full
  (live ≥ window − 2) or a record matured with no new delta. In steady state the datagrams
  alternate: movement, receipt.
- **A lost `0x69`** (no reply for 8 ms): polling `0x6A(id, consume=0)`. The reply "no such id"
  (255/5) means the request did not arrive: the delta goes to the carry-over, there was no movement. If the record
  exists, only the reply was lost, work continues. Re-sending the same delta under a
  new id without polling is forbidden by the contract, and the transport does not do it.
- **A lost receipt:** a `consume=1` retry. If the first one arrived, the retry gets 255/5 for a
  known record: it is considered handed over, the "ambiguous" counter grows. Over all runs 0.
- **Reordering and duplicates:** a reply is matched to a request by nonce, a reply for an already closed
  record goes to "late replies". Not observed on a LAN.

## 5. Build and run

```
build.bat
makcu_udp_mover.exe --check          connection + Raw + the board's intake, the cursor stands still (~3 s)
makcu_udp_mover.exe --netprobe       a detailed measurement of the board's intake, the cursor stands still (~10 s)
makcu_udp_mover.exe                  F-key menu, rawmove format
```

Exit is END (do not kill the process: that is how "Enhance pointer precision" and the
interpolation are restored, if it was changed).

### Menu

| Key | Mode |
| --- | --- |
| F1 / F2 | 250 / 1000 ticks/s, 30 s, the current shape |
| F3 / F5 | ABCurves Renderer: flicks / circle, 30 s |
| F4 | accumulator, flicks, 30 s + a speed → rate table |
| F6 | sweep of the `--rates` rates (HID, path, intervals), do not touch the mouse |
| F7 | format: rawmove → move (0x18) → movenow (0x67) |
| F8 | 1000 ticks/s, line 60 s (a test with the hand) |
| F9 | shape: circle → eight → line → flicks |
| F10 | accumulator, speed ladder 0.05..8 counts/ms, 2 s each (24 s) + a table |
| F11 | `0x1F` interpolation cycling 0 / AUTO / 25 / 50 / 100 (live, restored on exit) |
| F12 | measure the hand for 15 s with no injection |
| ESC / the same F-key | stop the run |

### CLI

| Option | Meaning |
| --- | --- |
| `--host IP`, `--udp-port N` | the board's address (192.168.50.175:8080) |
| `--wire raw\|host` | the wrapper; on 4094 only `raw` answers |
| `--pace reply\|bucket` | the rawmove datagram pace: reply-clocked (the default, ~283 Hz) or the bucket (250 Hz) |
| `--gap-us N`, `--burst B` | bucket: the average interval (1800) and the capacity (1); always for 0x18/0x67 |
| `--tx rawmove\|move\|movenow` | format; `move`/`movenow` go through the interpolator |
| `--interp auto\|0..100` | `0x1F` interpolation live (for 0x18/0x67) |
| `--compare A,B --rates N,..` | format comparison (ramp, 4 s per cell) |
| `--sweep`, `--run`, `--substep`, `--abc`, `--pattern`, `--speed`, `--size`, `--duration` | as in the COM bench |

The output is the same as for COM, plus a `udp ...` line: datagrams per second, how many were waited on for a reply and
how many arrived, RTT, pace. For the reply-clocked pace it prints how many datagrams went out right on the reply,
how many on timeout, and how many times a delta waited in the carry-over; for the bucket - the pace waits. In the
`raw 0x69` line there are added "lost requests", "lost replies", "polls", "ambiguous",
"late replies".

## 6. Results (fw 4094, Logitech Lightspeed 046D:C547, 2026-10-10)

### Reply-clocked pace (`--pace reply`, the default)

| Run | HID | Raw | Path |
| --- | --- | --- | --- |
| `--run --hz 1000 --pattern circle --duration 10` | **282 Hz** (1 s windows: median 283, worst 280), 99.4 % of intervals 2.5–5 ms | 2832 = 2832 = 2832, rejections 0; datagrams 566/s, RTT p50 1.75 ms, right on the reply 5654, on timeout 6, no reply 3 (receipts repeated); carry-over (0,0) | (−290,54) = (−290,54) |

### Acceptance with the bucket (`--pace bucket`, was the default until the evening of 2026-10-10)

| Run | HID | Raw | Path |
| --- | --- | --- | --- |
| F2: 1000 ticks/s, circle 30 s | **250 Hz** (worst window 248), 99.5 % of intervals at 4 ms | 7503 = 7503 = 7503, rejections 0, datagrams 500/s, no reply 4 (repeated), carry-over (0,0) | (−214,136) = (−214,136) |
| F10: ladder 24 s | generator = HID up to the ceiling: 50→50, 100→100, 200→200, then 250, CV 0.01–0.04 | 5093 = 5093 = 5093, no reply 1 of 10187 | (276,0) = (276,0) |
| F4: flicks 30 s | 118 Hz on average (pauses between flicks), up to 250 on fast stretches | 3575 = 3575 = 3575, losses 0 | (−91,−192) = (−91,−192) |
| Losses: flicks 15 s at `--gap-us 1000` | 111 Hz | no reply 1342 of 5420 (25 %): 678 lost `0x69` found by polling and returned to the carry-over, 471 receipts repeated, ambiguous 0 | **(45,−169) = (45,−169)** |

For comparison COM (`../comport_way`): F2 ~1000 Hz, 99.7–100 % of intervals at 1 ms; F10 - generator = HID
in all bins, 1000 Hz from 1 count/ms.

### Datagram pace

| `--gap-us` / `--burst` | datagrams/s | HID, Hz | losses | path |
| --- | --- | --- | --- | --- |
| 1800 / 1 (the default) | 500 | 250 | ~0 | exact |
| 1800 / 2 | 556 | 276–277 | 2.4 % on F10 (pairs with a 1 ms interval), uneven intervals (CV up to 0.6) | exact |
| 1750 / 2 | 571 | 271 | 4 % | exact |
| 1720 / 2, 1700 / 2 | 579 / 588 | 253 / 259 | 12 % / 11 % | exact |

`--burst 2` gives +11 %, but the board more often drops the second datagram of a pair with a 1 ms interval, especially
after a pause. On slow stretches HID is then below the generator. The default is the clean variant.

### The interpolator over UDP (`--compare`, ramp 4 px/ms)

| format, interpolation | commands/s | HID, Hz | reports per command | path |
| --- | --- | --- | --- | --- |
| rawmove | 126 / 251 | 124 / 248 | 1.00 | **1.00** |
| move 0x18, 0 | 250 / 500 | 564 / 423 | 2.26 / 0.85 | **0.57 / 0.28** |
| movenow 0x67, 0 | 250 / 500 | 564 / 423 | 2.26 / 0.85 | 0.57 / 0.28 |
| move 0x18, AUTO | 250 / 500 | 626 / 527 | 2.51 / 1.05 | 0.26 / 0.13 |
| move 0x18, 100 | 250 / 500 | 616 / 476 | 2.43 / 0.94 | 0.27 / 0.13 |

The interpolator cuts a command into several reports and gives more events, but does not rise above ~626 Hz
(on COM the ceiling is the same, ~640–690 Hz). The main thing — over UDP it **loses 43–87 % of the path**.
It is not suitable for an exact trajectory.

The old "fast" method (0x18 with no pace limit, as in the early `basics/pure_1000hz_stream.py`, deleted on 2026-10-12, it is in git history), repeated
on 4094: `--compare move --rates 500,1000 --gap-us 0`, interpolation 0, ramp 4 px/ms.

| commands/s | HID avg, Hz | reports per command | path | intervals 1 frame / 2 frames / > 2.5 ms |
| --- | --- | --- | --- | --- |
| 500 | 554 | 1.11 | 0.28 | 34 % / 51 % / 15 % |
| 1000 | 415 | 0.42 | 0.14 | 31 % / 36 % / 33 % |

Below 600 Hz and with a loss of 72–86 % of the path. The earlier 600–700 Hz over UDP (fw 4076) is the interpolator
multiplying a command into several reports, not intake above ~583 frames/s. On COM the same 0x18
at 500 commands/s brings the path in exactly (1.00, `../comport_way/ENG_SOLUTION_1000HZ_RAW_MOVE.md`
section 7.2), meaning that over UDP something is wrong with 0x18 apart from intake. This was not investigated.

## 7. What was tried for 1000 Hz and why it did not work

| Idea | Outcome |
| --- | --- |
| Several frames in one datagram (one header + frames, `MBAT`, several KM lines) | the board drops it or answers `ERR` |
| Several complete `55 nonce frame` records in a row (2–100), padding, alignment, IP fragmentation | dropped: only the length 9 + 5 + LEN is accepted |
| A bare frame (HOST) | dropped for free; there is no HOST mode in the board's settings |
| Datagrams more often | everything above ~583/s the board loses before processing (before that everything reaches its network interface) |
| Several source sockets | a shared ceiling, a shared drop window |
| Reply-less commands (`0x18`, `0x67`, SET), KM text `0x6B`, an unknown opcode | cost the same (~1.7 ms) as requests with a reply |
| The interpolator (one command → several reports) | a ceiling of ~626 Hz and path loss; the old 0x18 method at 1000/s: 415 Hz, path 0.14 |
| Receipts less often | not possible: the board has 16 records, a receipt frees exactly one (the official contract `raw-v4074.md`) |
| A more precise pace (a bucket with capacity 2) | +11 % (277 Hz) at the price of losses |
| Reply-clocked pace (one datagram in flight) | **+13 % with no losses: 282–283 Hz**, now the default |
| WebUI/WebSocket over TCP (the Toolkit runs the test in 4 sessions) | TCP 80 does not answer |
| Wi-Fi | not on this board (the `0x3D` status flags; only through an external MAKNET dongle) |
| Encrypted UDP | not tested: needs an AES key, set once and forever; decided not to touch |

1000 Hz over UDP requires changes outside the PC. Options:
1. **COM** (`../comport_way`): 1000 Hz is already verified.
2. **Firmware**: batch sending over UDP (several frames in a datagram) or an id-range receipt
   in the Raw API. With packets of 2 movements + 2 receipts per datagram every 2 ms,
   even the current ~583 datagrams/s would be enough. A draft of the request: `MAKCU_REQUEST_UDP.md`.
3. **Encrypted UDP** (a different code path on the board): not tested, a key is needed.

## 8. Not verified

- Encrypted UDP (the AES key is set once; decided not to touch it for now).
- Why 0x18 over UDP loses the path at 500 commands/s although all commands are accepted (on COM the path is exact).
- The reply-clocked pace under hand movement (the board's cycle grows to ~2 ms) and long runs.
- F3/F5 (ABCurves) over UDP: in the menu, not run on hardware (expected: the renderer ~350 reports/s
  on flicks, the transport will merge them to ≤ 283).
- F12 and runs with the hand at the same time as injection.
- Long runs (10+ min), a board reboot in the middle of a stream, Wi-Fi.
- Packet reordering and duplicates: the logic exists (matching by nonce), but they did not occur on a LAN.
- Two programs with the Raw API at the same time (shared ids, as on COM).

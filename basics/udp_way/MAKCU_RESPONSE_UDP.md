# MAKCU UDP — engineers' answers + the 0x18 closed-loop follow-up

Companion to [`MAKCU_REQUEST_UDP.md`](MAKCU_REQUEST_UDP.md). Records what the firmware
engineers answered to that request (relayed 2026-10-11 through the makcu.com Discord
bot, which is an LLM assistant), and the follow-up measurement they asked for.

**Environment:** fw **V4.094**, interpolation (`0x1F` GET) = `00`, Logitech Lightspeed
C547 @ 1000 Hz, board `192.168.50.175:8080`, UDP RAW wrapper (`0x55` + 8-byte nonce +
`DE AD` + len + op + payload), no encryption.

---

## 1. What the engineers confirmed / corrected

- **Our measurements are correct and match the firmware.**
- **`~1.72 ms` per UDP frame is structural**, not a setting: strict stop-and-wait, one
  command in flight. The **encrypted path and `/ws` go through the same single-command
  path.** → **1 kHz Raw over Ethernet is not reachable on V4.094.** (Answers questions
  1, 2, 4; kills avenue E as a *speed* route.)
- **TCP:** the WebUI/WebSocket server is on **port 80 only**. No TCP service on 8080; a
  SYN to any other port is ignored without an RST (so our 8080 timeout is expected).
  Port 80 **not** answering on the USB-Ethernet interface is accepted as a **bug to
  diagnose** (our request F / measurement 8).
- **`0x18` path loss over UDP:** their explanation changed three times. Final position:
  they accept it as an **unexplained bug** and must re-check the `0x18` UDP path and
  explain the `556 / (4 × cmd/s)` fit below. Their earlier "set interpolation to 0"
  workaround is **wrong** on 4.094 (confirmed: `0x1F` GET returns `00` and it still
  plans ~4 reports/command).

### Two test builds promised (no date)

1. **Raw batching opcode** — one datagram carries **up to 16 Raw moves** (each still its
   own HID report, same ids/rules as `0x69`) plus an optional **cumulative receipt**,
   with **one reply**. This is our **request A** and the real route to 1 kHz over UDP.
2. **Diagnostic build** that counts where **port-80 SYNs** are dropped on the
   USB-Ethernet interface.

Their one open worry about build #1: *can the device actually emit 1000 HID reports/s
over the UDP path?* Our answer is already in `MAKCU_REQUEST_UDP.md`: on the **same
V4.094, Raw `0x69` over COM sustains 997–1001 HID reports/s** at 1000 cmd/s with exact
path — so the HID output stage does 1 kHz; what is capped over UDP is command
*admission*, which batching removes. (Don't re-send this; they have it.)

---

## 2. The 0x18 closed-loop follow-up (what they asked us to run)

They asked: *"send each `0x18` only after the previous reply arrives (closed loop); we
expect the path to be exact up to ~581 cmd/s."* Two things came out of running it.

**`0x18` has no UDP reply.** Phase 0 sent 20 × `0x18` and counted replies by nonce:
**0/20**. The same frames move the mouse in the sweep (valid, executing), so `0x18` is
**fire-and-forget** — a literal "wait for the previous reply" loop is impossible.
Instead we paced it **one datagram in flight on a fixed interval**: because the board
digests a frame in ~1.72 ms, any spacing ≥ 1.72 ms guarantees the previous datagram is
done before the next is sent — no two datagrams ever contend for the busy window.

Probe: [`udp_18_closed.cpp`](udp_18_closed.cpp). Ramp +X at a constant **2000 px/s
commanded at every rate**, 2.5 s per rate.
`path(live)` = displacement arrived during the run ÷ commanded.
`path(drained)` = total after sends stop and output goes quiet (no movement for 400 ms,
cap 15 s).

```
cmd/s  spacing  HID Hz  reports/cmd  path(live)  path(drained)  delivered px/s
 125   8.00 ms   498      4.00         0.994        1.000           2000 (100%)
 150   6.67 ms   582      3.96         0.731        0.740           1480 ( 74%)
 200   5.00 ms   416      2.09         0.693        0.697           1394 ( 70%)
 300   3.33 ms   417      1.39         0.462        0.465            930 ( 46%)
 450   2.22 ms   417      0.93         0.308        0.310            620 ( 31%)
 581   1.72 ms   417      0.72         0.240        0.241            482 ( 24%)
```
(every row spaced ≥ 1.72 ms → contention-free by construction)

### Findings

- **Not delayed — lost.** `path(drained) ≈ path(live)` at every rate, each drain
  finished under 0.8 s, and the path never recovers toward 1.0. If undelivered steps
  were being carried, a 2.5 s run at 300 cmd/s would leave ~3000 reports draining for
  ~7 s at the device's output rate; we measured 0.48 s and no recovery. Steps are
  discarded in near-real-time. **(Refutes "exact totals after the tail drains".)**
- **Not an admission / busy-window drop.** At 150 cmd/s the datagrams are 6.67 ms apart
  — ~4× the 1.72 ms cycle — so nothing can land in a busy window, yet 26 % of the
  displacement is already gone. **(Refutes "only ~139 cmd/s admitted, the rest dropped
  before admission".)**
- **Output-bound and worsens with rate.** From 200 cmd/s up, `path ≈ 556 / (4 × cmd/s)`
  almost exactly. Net effect: raising the `0x18` rate makes the mouse move **less** —
  commanded is a constant 2000 px/s, delivered falls from 2000 px/s at 125 cmd/s to
  ~480 px/s at 581.

### Engineers' reply to this

They accepted it: the paced test removes the busy-window explanation, the drained total
matches the live total so steps are lost rather than delayed, and they are passing the
full table to the firmware engineers to re-check the `0x18` UDP path and explain the
`556/(4×cmd/s)` fit. No conclusion until they do.

---

## 3. Current state / next action

- **Exact-path options over UDP today:** Raw `0x69` (reply-clocked, ~283 moves/s, path
  1.000). The interpolated `0x18` path is unusable above ~139 cmd/s and is a confirmed
  bug.
- **Waiting on:** the two test builds above. When the batching build arrives, rerun
  `udp_18_closed.exe` **and** the raw bench and report the same metrics (HID rate,
  interval histogram, path sent-vs-received, Raw counters). The metric format is ready.

Build line for the probe:
```
g++ -std=c++23 -O2 -I ..\comport_way udp_18_closed.cpp -lws2_32 -lwinmm -static -o udp_18_closed.exe
```

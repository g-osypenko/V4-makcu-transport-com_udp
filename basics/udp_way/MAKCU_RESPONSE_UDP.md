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

---

## 4. Root cause (engineers, 2026-10-11) and the V4.097 matrix (2026-10-12)

**Root cause, verified from source:** the dropped commands never reach the movement
engine. UDP lands on the networking chip, and each command needs a full hand-off with the
other chip (request → reply → completion → idle ack) before the next one is accepted; a
packet arriving during the hand-off is silently dropped. On V4.094 that hand-off averaged
~7.2 ms under load → ~139 commands/s (our drained totals: `path × cmd/s` = 139.4–140.0).
COM goes straight to the upstream chip and has no such hand-off (the engineers' COM lab run
on V4.094: zero loss at 125–1000 cmd/s, report rate ~690–700 Hz). The earlier "1.72 ms busy
window" was the cost of light query frames, not of a move.

**V4.097** (test build) adds batch `0x6C` (up to 16 Raw moves per packet, one hand-off,
one receipt) and sends an interpolation-0 move as one report. Engineers' advice: closed loop
(wait for the reply/receipt), ≤ 7 new records per packet. Probe:
[`udp_raw_batch.cpp`](udp_raw_batch.cpp); logs `v4097_raw_batch.txt`, `v4097_0x18_extra.txt`.

```
row                          moves/s  HID Hz  path(live)  path(drained)  intervals 1 ms
a) 0x18 @125/s (fixed rate)      125     125     0.997       1.000
a) 0x18 @200/s                   200     200     0.996       1.000
a) 0x18 @300/s                   300     300     0.999       1.000
a) 0x18 @400/s                   267     267     0.666       0.668
a) 0x18 @500/s                   259     259     0.516       0.518
a) 0x18 @581/s                   291     291     0.499       0.501
b) 0x69 @125/s (closed loop)     125     125     0.998       0.998
b) 0x69 @200/s                   200     200     0.998       0.998
b) 0x69 @300/s                   284     284     0.998       0.999
b) 0x69 @581/s                   284     284     0.998       0.999
c) 0x6C N=7  @ 50 pkt/s          350     347     0.988       0.992           86%
c) 0x6C N=7  @100 pkt/s          700     697     0.993       0.996           86%
c) 0x6C N=7  @139 pkt/s          974     970     0.995       0.998           97%
c) 0x6C N=16 @ 50 pkt/s          351     344     0.977       0.983           86%
c) 0x6C N=16 @100 pkt/s          703     694     0.989       0.996           86%
c) 0x6C N=16 @139 pkt/s          975     967     0.992       0.995           97%
```

`path(drained)` < 1 on Raw/batch rows = carry not yet sent at cut-off; delivered ÷ admitted
= 1.000 on every row. Board ends with 16/16 free records (nothing leaked).

- **~1 kHz over Ethernet works on V4.097:** `0x6C` N=7 @139 pkt/s = 974 moves/s, HID 970 Hz,
  97 % 1 ms intervals, exact path. Every receipt result 0, 7/7 admitted, free slots 2,
  freed ≈ 6.9/packet, K ≈ 7, `more` 0 %, no timeouts.
- N=16 gives the same throughput: ~7 of 16 admitted per packet (result 1, first-failure
  reason 3 = no space, free slots 0, `more` set), because a receipt frees at most 7 rows.
- `0x18` (no reply) is exact up to 300 cmd/s at a fixed rate on V4.097; above that only
  ~260–290/s are accepted (unanswered hand-off ≈ 3.5 ms).
- Raw `0x69` closed loop is unchanged at ~284 moves/s (2 datagrams per move, ~567/s).
- External USB-C power on the UGREEN hub changed nothing (checked on V4.094).

**Rig:** PC onboard Realtek 2.5GbE (1 Gbps link) → switch → **UGREEN 60554** (USB 3.0 to
3×USB 3.0 + gigabit RJ45) on MAKCU USB3, Logitech Lightspeed receiver 046D:C547 @1000 Hz in
the same hub. Read with the hub plugged into the PC: NIC = **ASIX AX88179, VID:PID 0B95:1790**,
behind a Genesys Logic hub chip 05E3:0610. (The request doc's "RTL8152" was wrong.)

---

## 5. Engineers' closing answer (2026-10-12)

- **0x18 over UDP tops out around 280 commands/s.** Each UDP command goes to the other chip
  over an internal link that carries one command at a time; a datagram arriving while the
  previous one is in flight is dropped. 0x18 has no reply, so the drop is silent. Accepted
  commands are applied exactly (matches drained path 1.000).
- **0x6C N=16 partial (reason 3) is expected:** at most 7 rows fit at once, the rest comes back
  as partial with `more` set.
- **Intended way above ~280/s:** 0x6C with up to 7 rows per packet, one packet in flight, next
  one sent on the receipt (what `makcu_udp_mover --tx batch` does), or 0x69 with receipts.
- A counter or reply for the dropped case is a known gap for later. **Firmware development is
  paused**, so nothing will change soon.

**Status: closed.** 1 kHz over Ethernet = V4.097 + `0x6C` closed loop. Update 2026-10-12: V4.097
became the **official** MAKCU firmware on 2026-10-11 (MCP `firmware_versions`: `current_official`),
so `0x6C` is no longer test-only. V4.095 and older lack it.

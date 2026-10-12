# MAKCU UDP path/HID measurement — reproducible bundle

Self-contained copy of the tests used to measure movement over the UDP (Ethernet) API:
the `0x18` path loss, Raw `0x69`, and the `0x6C` batch. Each builds with one `g++` command — the `.cpp` files and the three headers are
in this folder, no extra include path. Windows only (WinSock + Win32 input APIs).

## Files

| file | role |
| --- | --- |
| `udp_18_closed.cpp` | paced `0x18` ramp (fixed rate, `0x18` has no reply), path sent-vs-received, HID rate/intervals |
| `udp_raw_batch.cpp` | closed-loop Raw `0x69`+`0x6A` and `0x6C` batch (receipt → X), same ramp and metrics, receipt fields |
| `hook_monitor.h` | HID **rate** tool — low-level mouse hook (`WH_MOUSE_LL`) |
| `rawinput_monitor.h` | **path** tool — Raw Input per-device dx/dy sum |
| `mover_common.h` | QPC clock + precise pacing + process/thread timing tuning |

(Inline comments in the headers are in Russian; everything you need is in English here
and in `udp_18_closed.cpp`.)

## Build & run

```
g++ -std=c++23 -O2 udp_18_closed.cpp -lws2_32 -lwinmm -static -o udp_18_closed.exe
g++ -std=c++23 -O2 udp_raw_batch.cpp -lws2_32 -lwinmm -static -o udp_raw_batch.exe
udp_18_closed.exe --host <board-ip> --targets 125,200,300,581
udp_raw_batch.exe --host <board-ip> --mode all      # b) 0x69 at 125/200/300/581, c) 0x6C N=7,16 at 50/100/139 pkt/s
```
MinGW-w64 g++ 13+ (tested on 15.3.0). Options: `--host`, `--port` (8080),
`--targets 125,150,200,...`, `--speed` (px/ms), `--cell` (s per rate),
`--quiet-ms`, `--drain-cap-ms`; `udp_raw_batch` also `--mode raw|batch|all`, `--raw-rates`,
`--pkt-rates`, `--recs`, `--dt`. The cursor moves during the run — don't touch the mouse (each
cell parks the cursor at the left edge of the desktop first; it is restored at the end).

## Test environment (what we ran)

- Firmware **V4.094**, interpolation `0x1F` GET = `00`.
- Mouse: **Logitech Lightspeed receiver, VID 046D / PID C547, 1000 Hz** polling, on USB3.
- Board reachable over UDP at `<board-ip>:8080`, **no encryption**.
- Topology: PC (onboard Realtek 2.5GbE, gigabit link) → switch → a **UGREEN 60554**
  hub (USB 3.0 to 3×USB 3.0 + Gigabit 1000M RJ45): Genesys Logic hub 05E3:0610 +
  ASIX AX88179 gigabit NIC 0B95:1790.
  The mouse receiver is in the same UGREEN hub, and that hub is the only device on
  MAKCU's USB3 port.

## Packet format (RAW UDP wrapper)

Every datagram is one framed command, little-endian:

```
0x55 | nonce[8] | 0xDE 0xAD | len[2] | op | payload[len]
  |       |          |          |       |      |
  |       |          |          |       |      +- op-specific
  |       |          |          |       +- opcode
  |       |          |          +- payload length (u16), i.e. bytes after op
  |       |          +- frame magic
  |       +- transaction id (u64), echoed back in the reply
  +- RAW wrapper marker
```

The reply (for ops that answer) has the same shape and echoes the nonce; the payload
starts at byte offset 14 of the datagram. Opcodes used here:

- `0x18` MOVE — payload `dx` (i16), `dy` (i16). **No reply** (fire-and-forget; verified
  0/20 answered). It still executes (it moves the mouse).
- `0x6A` RAW_STATUS — payload `id` (u32), `consume` (u8). `id=0, consume=0` returns an
  8-byte metadata reply: `last_accepted_id` at bytes 2..5, `free_records` at byte 6,
  `capacity` at byte 7. Used here only as a liveness/meta query.
- `0x1F` interpolation — GET with empty payload; reply byte is the current value.

## How the two measurement tools work (and why)

- **HID rate — `hook_monitor.h` (`WH_MOUSE_LL`).** The hook fires on every mouse input
  event the OS sees, regardless of window focus, and skips `LLMHF_INJECTED` events, so a
  hardware device (MAKCU) is counted but `SendInput` is not. Gives the HID report count
  and an interval histogram. (Raw Input is **not** used for rate: Windows coalesces Raw
  Input for a background window down to ~125 Hz, which under-reads the true report rate.)
- **Path — `rawinput_monitor.h` (Raw Input, `RIDEV_INPUTSINK`).** Sums per-device
  `lLastX/lLastY`. Used for displacement only: even when background coalescing drops the
  message *count*, the delta *sum* is preserved, so the path total is accurate. `primary`
  = the device with the most reports since the last reset.
- **Pacing — `mover_common.h`.** QPC clock, busy-wait to an absolute deadline,
  `timeBeginPeriod(1)`, `HIGH_PRIORITY_CLASS`, power-throttling off. Needed so the send
  interval is actually the interval.

## What the test does

One `0x18` datagram in flight at a time, on a fixed interval (`spacing = 1e6 / target`
µs). Since `0x18` has no reply to clock on, pacing is by timer. Motion is a ramp to the
right at `--speed` px/ms, so the **commanded displacement is a constant `speed × 1000`
px/s at every command rate** (each command carries the delta accumulated since the last
send). For each target rate it reports:

- `path(live)` = device dx sum during the send window ÷ commanded sum.
- `path(drained)` = device dx sum after sends stop and output goes quiet (no new movement
  for `--quiet-ms`, giving up at `--drain-cap-ms`) ÷ commanded sum.

`path(drained) ≈ path(live)` ⇒ steps were lost. `path(drained) → 1.0` after a long drain
⇒ steps were only delayed. Phase 0 additionally reads interpolation (`0x1F`), reads the
Raw metadata (liveness), and checks whether `0x18` replies.

Console table columns: `target c/s`, `spacing ms`, `sent 0x18`, achieved `cmd/s`,
`admit%` (replies ÷ sent; `n/a` when the op doesn't reply), `HID Hz` (live window),
`rep/cmd` (reports per command after drain), `path(live)`, `drain ms`, `path(drained)`.
Derived: `delivered px/s = path × speed × 1000`.

## Result we measured (V4.094, speed 2 px/ms = 2000 px/s commanded, 2.5 s per rate)

```
cmd/s  spacing  HID Hz  rep/cmd  path(live)  path(drained)  delivered px/s
 125   8.00 ms   498     4.00      0.994        1.000           2000 (100%)
 150   6.67 ms   582     3.96      0.731        0.740           1480 ( 74%)
 200   5.00 ms   416     2.09      0.693        0.697           1394 ( 70%)
 300   3.33 ms   417     1.39      0.462        0.465            930 ( 46%)
 450   2.22 ms   417     0.93      0.308        0.310            620 ( 31%)
 581   1.72 ms   417     0.72      0.240        0.241            482 ( 24%)
```

`path(drained) ≈ path(live)` and each drain settled in < 0.8 s (no backlog), so steps are
lost, not delayed. `path × cmd/s = 139.4–140.0` across 200–581 cmd/s, i.e. a flat
~139 commands/s accepted regardless of send rate — consistent with a fixed per-command
acceptance limit upstream of the motion stage. At ≤ 125 cmd/s (spacing ≥ 8 ms) the path
is exact.

For comparison on the same firmware: Raw `0x69` reply-clocked over UDP ≈ 283 moves/s with
exact path; Raw `0x69` over COM sustains 997–1001 HID reports/s at 1000 cmd/s, exact.

## Results on V4.097 (same rig, same ramp: 2000 px/s commanded, 2.5 s per row)

`delivered/admitted` = device dx after drain ÷ displacement in records the board accepted.
`path(drained)` < 1 on the Raw/batch rows is only the carry not yet sent when the window
closed (4–85 px of ~5000); delivered/admitted is 1.000 on every row.

```
row                          moves/s  HID Hz  path(live)  path(drained)  intervals 1 ms
a) 0x18 @125/s (fixed rate)      125     125     0.997       1.000          (8 ms spacing)
a) 0x18 @200/s                   200     200     0.996       1.000
a) 0x18 @300/s                   300     300     0.999       1.000
a) 0x18 @400/s                   267     267     0.666       0.668
a) 0x18 @500/s                   259     259     0.516       0.518
a) 0x18 @581/s                   291     291     0.499       0.501
b) 0x69 @125/s (closed loop)     125     125     0.998       0.998
b) 0x69 @200/s                   200     200     0.998       0.998
b) 0x69 @300/s                   284     284     0.998       0.999          (567 datagrams/s)
b) 0x69 @581/s                   284     284     0.998       0.999
c) 0x6C N=7  @ 50 pkt/s          350     347     0.988       0.992           86%
c) 0x6C N=7  @100 pkt/s          700     697     0.993       0.996           86%
c) 0x6C N=7  @139 pkt/s          974     970     0.995       0.998           97%
c) 0x6C N=16 @ 50 pkt/s          351     344     0.977       0.983           86%
c) 0x6C N=16 @100 pkt/s          703     694     0.989       0.996           86%
c) 0x6C N=16 @139 pkt/s          975     967     0.992       0.995           97%
```

- `0x18` on V4.097 is one report per command, exact up to 300 cmd/s at a fixed rate;
  above that only ~260–290 commands/s are accepted (an unanswered `0x18` hand-off ≈ 3.5 ms).
- Raw `0x69` closed loop: 2 datagrams per move, ~567 datagrams/s → 284 moves/s, exact.
- `0x6C` N=7: every receipt result 0, 7/7 admitted, free slots 2, freed ≈ 6.9/packet,
  K ≈ 7, `more` 0 %, no timeouts → **974 moves/s, HID 970 Hz, 97 % 1 ms intervals**.
- `0x6C` N=16: receipts result 1 (partial), ~7 of 16 admitted, first-failure reason 3 (no
  space), free slots 0, `more` set → throughput equals N=7 (receipts free at most 7 rows).

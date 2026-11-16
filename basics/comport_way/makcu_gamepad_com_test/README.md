# MAKCU V4.126: controller on USB3 disappears under COM traffic, and the USB3 port stays dead

Test material for the MAKCU engineers, 2026-11-16. Written so that it can be read by an LLM as well as by a person:
every number below comes from a log in `logs/`, and every program is in source form in this archive.

## 1. Short version

- **Polling `GET 0x54 CONTROLLER_PHYSICAL` over the COM port at ~2000 requests/s, with no injection at all, makes the
  controller disappear from MAKCU's USB3 port** after 4-108 s (4 runs out of 4, one of them with the single-file
  reproduction in `repro/`). Read-only at 1000/s: lost at 38.8 s on the cable (2026-11-15), 30 s clean on the
  receiver (one run). Read-only at 100-500/s never failed.
- **After that the USB3 port is dead.** MAKCU keeps answering on COM (~6500 requests/s), but `0x02 DEVICE` = `0x00`,
  `km.device()` = `R:-;M:0uf;K:0uf;C:0uf`, `0x54` = `FF`. Unplugging and re-plugging the controller's receiver does
  not help (2 out of 2: the receiver is powered, its LED is on, but it never enumerates). Only unplugging all MAKCU
  cables (a full restart of the board) brings it back. In a healthy state the same receiver re-enumerates by itself
  after unplugging (3 out of 3, 5-24 s).
- The same happens with the controller on a cable (2026-11-15) and on its 2.4 GHz receiver (2026-11-16), so it is
  unlikely to be about the controller drawing too much current through MAKCU (the receiver draws little).
- `SET 0x40` once per millisecond halves the controller report rate seen by MAKCU (`sequence` advances ~500/s
  instead of ~1000/s), both on the cable and on the receiver.

## 2. Setup

| Item | Value |
| --- | --- |
| MAKCU firmware | **V4.126** (official; `0x04` answers 4126). Was updated from V4.122 on 2026-11-15 |
| PC link | USB2, CH343 `1A86:55D3`, COM5, 4 000 000 baud, 8N1, no flow control, DTR/RTS off. Driver WCH CH343SER 2.2.2026.5 (Windows 11 23H2) |
| USB1 | to the PC (power + cloned devices) |
| USB3 | **only the controller**: GameSir controller in XInput mode. 2026-11-15: USB cable. 2026-11-16: its 2.4 GHz receiver (a small charging base with an antenna, `VID_3537`). Nothing else on USB3 |
| What MAKCU reports | `0x02 DEVICE` = `0x82` (keyboard + Xbox 360/XInput); `km.device()` = `R:KC;M:0uf;K:64uf;C:8uf`; the PC sees the clone as `VID_3537&PID_1098` "XBOX 360 For Windows" |
| Controller settings on MAKCU | smoothing **Off** (interpolation 0) unless stated; buffer 8 ms; one "Linear" profile; no deadzones. Some runs on **Auto**, marked |
| Controller report rate | ~1000 reports/s on the cable and on the receiver. Note: while the GameSir Connect app is running, the receiver gives only ~230 reports/s; it was closed for all runs below (and the rate was checked) |

## 3. Programs in this archive

| File | What it is |
| --- | --- |
| `repro/repro_poll54.cpp` (+ `.exe`) | **The minimal reproduction.** One file, Win32 only. Opens the port, sends `DE AD 00 00 54` at `--rate` per second, one request in flight, prints one line per second and detects the failure itself. Build line inside |
| `soak/pad_soak.cpp` + `soak/makcu_gamepad_raw_transport.h` (+ `.exe`) | The "load ladder" we used for all soak runs: steps of increasing COM load, stops at the first failure. Uses our gamepad transport (one worker thread, 500 us tick, reads without waiting, `GET 0x54` every tick = 2000/s or less, `SET 0x40` every N ticks, `GET 0x40` 4/s as a check). Comments are in Russian |
| `probes/pad_probe_info.cpp` + `probes/pad_probe_common.h` (+ `.exe`) | Read-only snapshot of the board: `0x02`, `0x04`, `km.*` text queries, `GET 0x40/0x41/0x52/0x54`, settings image (`0x3E` INFO/READ only), then 1 s of `0x54` polling. Used for the snapshots right after each failure |

Build (MinGW-w64): see the first lines of each `.cpp`. `pad_soak`: `g++ -std=c++20 -O2 -static pad_soak.cpp -lsetupapi -lwinmm -o pad_soak.exe`.

### pad_soak steps used here

| Step | `GET 0x54`/s | `SET 0x40`/s | What the injection does |
| --- | --- | --- | --- |
| `poll500`, `poll1k`, `poll2k` | 500 / 1000 / 2000 | 0 | read only |
| `stick2-p100`, `stick15-p100`, `stick1-p100` | 100 | 500 / 667 / 1000 | right stick in a circle (radius 12000, 1 turn/s) |
| `stick1-p2k` | 2000 | 1000 | circle (this is what our real transport does with smoothing Off) |
| `taps2-p100` | 100 | 500 | circle + right trigger press 80 ms every 250 ms |
| `hold2-p100` | 100 | buttons only | hold RT + A 3.2 s, release 0.5 s, no stick |
| `all2-p100` | 100 | 500 | circle 2 s + trigger tap, stick release (all-zero `SET 0x40`), hold RT + A 1.5 s |

### How to read the logs (they are in Russian)

`pad_soak` prints one line per 10 s:
`time | step | s | Windows packets/s | board: requests/s, new reports/s, FF, no reply, skipped | stick commands/s, button commands, 0x40 refusals, repairs`.
- "new reports/s" = how many `0x54` replies carried a new `sequence`; "skipped" (пропущено) = sum of `sequence` jumps
  minus one, i.e. reports MAKCU had that we did not poll. **Controller reports/s = (new + skipped) / 10.**
- `ОБРЫВ` = failure, `итог` = step summary, `ИТОГ` = run summary, `kinds` = `0x02 DEVICE` byte (`kinds 80` printed by
  pad_soak = `0x82 & 0xFC`, i.e. the controller is still enumerated).
- `soak_*_info_t0/t10/t30.txt` = `pad_probe_info` snapshots 0, 10 and 30 s after the failure (nobody touched anything).
- `view_*.log` = a separate read-only window that watches Windows XInput (no COM): "Windows подключён/НЕТ" = the PC
  sees / does not see the controller, "пакетов/с" = XInput packets/s.
- `drop_*.log` = a script that logs the controller's USB presence every 0.2 s (`[+] в USB` = appeared, `[!] ... НЕТ` = gone).

## 4. Results

### 4.1 2026-11-16, controller on its 2.4 GHz receiver (V4.126)

| Time | COM traffic | Smoothing | Controller reports/s seen by MAKCU | Result | Log |
| --- | --- | --- | --- | --- | --- |
| 22:07-22:18 | none (two 1 s snapshots) | Off | 1000 (after GameSir Connect was closed) | **600 s clean** (480 s at 1000/s) | `drop_dongle4.log`, `view_dongle4.log` |
| 22:21-22:30 | `stick2-p100` 180 s, `taps2-p100` 180 s, `hold2-p100` 180 s | Off | 1000 | **540 s clean** (90 012 stick commands per step, 1438 and 1651 button commands, 0 refusals) | `soak_dongle1.log` |
| 22:32 | `stick1-p2k` | Off | **500** from the first 10 s | **disappeared at 63.8 s** | `soak_dongle2.log` (+ `_info_t*`) |
| 22:39 | `poll500` 30 s | Off | 1000 | clean | `soak_rateA.log` |
| 22:39 | `stick1-p100` 30 s | Off | **500** | clean (30 s only) | `soak_rateA.log` |
| 22:40 | `poll1k` 30 s | Off | 1000 | clean | `soak_rateB.log` |
| 22:41 | `poll2k` | Off | 1000 | **disappeared at 21.0 s** | `soak_rateB.log` (+ `_info_t*`) |
| 22:44 | `poll2k` | Off | 1000 | **disappeared at 4.3 s** | `soak_poll2k_off2.log` (+ `_info_t*`) |
| 22:50 | `poll2k` | **Auto** | 1000 | **disappeared at 108.4 s** | `soak_poll2k_auto1.log` (+ `_info_t*`) |
| 23:01 | `repro_poll54 --rate 2000` (this archive) | **Auto** | 1000, then 899 at 72 s | **lost at 75.1 s** (150 247 requests) | `repro_poll54_auto1.log` |

After every failure (snapshots at 0, 10 and 30 s, nothing touched): COM answers ~6500-6900 requests/s,
`0x02` = `0x00`, `km.device()` = `R:-;M:0uf;K:0uf;C:0uf`, `0x54` = `FF` (1 byte), the clone is gone from Windows.

Re-plugging only the receiver after a failure (`drop_replug_after_fail1.log`, and once more by hand at 22:42): the
receiver's LED is on (it is powered), the controller blinks looking for it, Windows sees nothing, MAKCU stays at
`0x02` = `0x00` for 100 s. 2 out of 2. Re-plugging all MAKCU cables fixes it.

Re-plugging only the receiver in a healthy state (`drop_hotplug1.log`, 21:52, no COM traffic): 3 out of 3 came back
by themselves after 23.5, 9 and 5.5 s (the receiver first shows up as `PID_2106`, then as `PID_1098`).

One more failure with no COM traffic at all: 21:37, controller on the receiver, the PC only read XInput, the
controller disappeared after 38 s and did not come back (`view_dongle1_4126.log`). We did not take a snapshot of
MAKCU before re-plugging, and around that time the GameSir Connect app was being closed, so we do not count it.

### 4.2 2026-11-15, controller on a USB cable (V4.126, smoothing Off)

| COM traffic | Frames/s | Result | Log |
| --- | --- | --- | --- |
| none (controller used through MAKCU) | 0 | ~5 h 50 min, no dropout | — |
| `GET 0x54` only (separate probe) | 100 | 300 s clean | — |
| `stick8/4/2-p100` (125 / 250 / 500 commands/s) | 230 / 355 / 605 | 3 x 180 s clean | `pad_soak3_20261115.log` |
| `enter2-p100` (258 enter / release cycles) | ~330 | 180 s clean | `pad_soak5_20261115.log` |
| `poll1k` | 1000 | **disappeared at 38.8 s** (an earlier run: 61 s clean) | `pad_soak1_20261115.log` |
| `stick1-p100` | 1100 | **disappeared at 9.8 s**; reports seen by MAKCU ~495/s | `pad_soak2_20261115.log` |
| reference transport, `GET 0x54` 2000/s + `SET 0x40` 1000/s | 3000 | **disappeared after ~7 s** (an earlier run ~20 s clean) | `pad_bench_20261115_off3_4126.log` |
| `all2-p100` | ~360 | **went silent at 51 s**: `0x02` still `0x82`, `0x54` frozen `sequence`, `timing` = `0x8000` | `pad_soak4_20261115.log` |
| `taps2-p100` | ~605 | **went silent at 144 s** (576 trigger presses) | `pad_soak6_20261115.log` |

"Went silent" is the second failure mode we saw only on the cable: the controller stays enumerated, MAKCU keeps
returning the same `0x54` snapshot with `timing` = `0x8000` (invalid), Windows still has the device but gets no
packets. It also needed a full restart of MAKCU.

### 4.3 For comparison: V4.122, 2026-11-14, cable, smoothing Auto

Reading `0x54` without any pause (~3500/s) for 90 s, 313 977 requests: the controller stayed. (Same controller,
different firmware and smoothing mode. One run.)

### 4.4 `repro_poll54.exe` on V4.126

`repro_poll54.exe --port COM5 --rate 2000 --dur 300`, 2026-11-16 23:01, V4.126, smoothing Auto, controller on its
receiver, held in the hand (the left stick moved now and then so that it does not fall asleep).
Log: `logs/2026-11-16_receiver/repro_poll54_auto1.log` (the XInput window of the same run: `view_repro1.log`).

- seconds 1-71: 2000 requests/s, 2000 replies of 32 bytes, 0 FF, 0 timeouts, `sequence` +1000/s, Windows ~1000 XInput packets/s;
- second 72: `sequence` +899, Windows 902 packets/s; second 73: 1990 requests;
- second 75: 1256 replies of 32 bytes and 744 FF, Windows: controller gone. Detected at 75.1 s (150 247 requests);
- 0, 5, ..., 30 s later: `0x02` = `0x00`, `km.device()` = `R:-;M:0uf;K:0uf;C:0uf`, Windows has no controller. Did not come back.

So the shortest reproduction is: open the port, send `DE AD 00 00 54` 2000 times per second, wait 5-110 s.

## 5. What we cannot rule out

- **The controller.** Everything was measured with one controller model (GameSir, XInput). We have no second
  controller. What points at MAKCU: the controller survives 8-10 minutes at 1000 reports/s through MAKCU with no COM
  traffic and 9 minutes of light traffic; it fails within seconds to a couple of minutes once COM traffic is about
  1000-2000 frames/s, even read-only; and after the failure MAKCU's USB3 port no longer enumerates anything plugged into it.
- **Smoothing mode.** Off: 21.0 and 4.3 s; Auto: 108.4 and 75.1 s. Auto may delay the failure, but it does not prevent it;
  two runs each, so even the delay may be chance.
- **Firmware.** V4.122 with Auto held ~3500/s for 90 s; we cannot go back to V4.122 to separate firmware and mode.

## 6. Questions

1. Is there a maximum supported request rate for `0x54` / `0x40` over COM? Our use case needs `SET 0x40` once per
   millisecond with smoothing Off, and reading the physical controller at a similar rate.
2. Why does the USB3 port stop enumerating devices after the failure, and is there a command that restarts the USB3
   side (or the whole board) without unplugging cables?
3. Does every `SET 0x40` cost the USB3 side one controller poll (500 instead of 1000 reports/s at 1000 commands/s)?
4. Would the `0x73` / `0x74` streams (event `0x75`) load the USB3 side less than polling `0x54`?
5. Do you have a debug build or a log we can capture at the moment of the failure?

We can run any test build or diagnostic you send and return the logs.

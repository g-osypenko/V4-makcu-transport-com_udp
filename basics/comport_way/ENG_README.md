# MAKCU mover (COM): reference 1000 Hz injection + ABCurves Renderer

*English translation of [`README.md`](README.md) (Russian original). Links point to the English (`ENG_`) versions of the documents where one exists. Source-code comments and program output (for example `HID ... Гц`) are still in Russian; the table in "How to read the output" gives the English meaning.*

**Status as of 2026-10-10: working.** On MAKCU firmware 4094 injection runs at
~1000 Hz (99.7–100% of intervals are 1 ms), the "sent = arrived in Windows" path
matches to the count, and touching the real mouse does not break the trajectory (checked
visually by the user). The key to this is the **Raw movement `0x69`/`0x6A`** command
instead of `MOVE 0x18`. The full write-up — how the solution was found, the protocol, all measurements,
risks — is in [ENG_SOLUTION_1000HZ_RAW_MOVE.md](ENG_SOLUTION_1000HZ_RAW_MOVE.md).

This README is the main document of the folder and the entry point for the next session.
The reference transport to embed in your own programs (movement + mouse buttons):
[`perfect-transport-comport/`](perfect-transport-comport/ENG_README.md).

---

## 1. How it works (the pipeline)

```
 motion source                    now: Trajectory::at(t) - hardcoded shapes
 (position as a function of time) next: external trajectories (plan for the next chat)
        │  every 1 ms: a smooth intent Δp = p(t_k) - p(t_k-1), double, mouse axes (Y down)
        ▼
 ABCurves Renderer                preprocessing of the trajectory BEFORE it goes to MAKCU:
 (abc_renderer.h)                 Δp -> an integer report (dx,dy) with the "texture" of a real
                                  sensor: in which ms there is a report, how counts are
                                  quantized, micro-jitter. Often (0,0) - that is normal. The fractional
                                  remainder accumulates inside, the total path is preserved.
        │  non-zero report (without the renderer: dx = round(p) - already_sent)
        ▼
 RawComTransport, RawMove         0x69 RAW_MOVE for each report + receipts 0x6A,
 (makcu_transport.h)              a window of 12 records, carry-over of rejected deltas, its own
                                  overlapped COM handle, no blocking of the timing thread
        │  COM5, CH343, 4,000,000 baud
        ▼
 MAKCU fw 4094                    FIFO -> EXACTLY ONE HID report per request, added to the
                                  physical mouse movement in the same report
        │  USB; the PC polls MAKCU at the interval of the cloned mouse (1 ms)
        ▼
 Windows / application            ~1000 reports/s, exact path
```

Consequences:
- **HID rate = rate of non-zero commands** (not above the USB polling rate). For 1000 Hz a
  non-zero delta is needed every 1 ms. The renderer itself decides in which ms there is a report: its
  zero reports = milliseconds without a report, like a real mouse.
- The renderer is preprocessing: it knows nothing about MAKCU, and MAKCU knows nothing about the
  renderer. The Raw path delivers each renderer report 1:1. The old `0x18` path
  mixed them with its own interpolator, which is why the renderer used to not work as intended.
- The mouse in MAKCU's host port must be polled at 1000 Hz: the board clones its
  polling interval (with a 125 Hz mouse, injection is no faster than 125 Hz).

## 2. Reference files (approved 2026-10-10)

### Reference module (portable to other programs)

**The transport on its own, frozen:** [`perfect-transport-comport/`](perfect-transport-comport/ENG_README.md) - one
header `makcu_raw_transport.h` (Raw 0x69/0x6A, no SDK), an example with a self-check. Take it when you need
only the delivery of reports to MAKCU; `makcu_transport.h` below remains the working copy of the test bench.

| File | What is inside | What to take |
| --- | --- | --- |
| `makcu_transport.h` | `RawComTransport`: its own overlapped COM handle, a ring of 32 entries, opening with a baud probe 4M/1M/115200 and `DEVICE 0x02`, a read thread, `kmQuery`/`transact`, **`Format::RawMove`** (0x69/0x6A: `prepareRaw`, `moveRaw`, receipts, window, carry-over, `finish`) | `RawComTransport` in `Format::RawMove` mode. The `Mak`/`KmMove`/`KmMoveNow*` formats and `SdkTransport` are for comparison only |
| `mover_transport.h` | the `Transport` interface + `TransportStats` without the SDK (shared with `../udp_way`, split out of `makcu_transport.h` on 2026-10-10) | all of it |
| `motion_engine.h` | `runMotion()`: an absolute QPC grid, a 1 ms tick, catching up on missed ticks, a pause on an OS freeze > 25 ms, a renderer step for every ms, carry-over when the transport is busy, `tx.finish()` at the end. `Trajectory`/`Pattern`/`makeFlicks` are test shapes | `runMotion` as a model of the loop. The shapes will be replaced by an external source |
| `abc_renderer.h` | `AbcRenderer`: model loading (44,484 bytes, CRC), a profile of exactly 256 real 1 ms reports, `beginEvent(seed)` per run, `step()` once per 1 ms, Y-axis flip (the renderer is Y up, HID is Y down) | all of it |
| `substepper.h` | `Accumulator`: a noiseless accumulating substepper - a smooth 1 ms intent -> an integer report, the remainder accumulates (<= 0.5 count), exact path. The rate is set by the speed alone | all of it (an alternative to the renderer) |
| `mover_common.h` | `Clock` (QPC), `PreciseWaiter` (spin/hybrid), `tuneProcessForTiming` (HIGH, timeBeginPeriod(1), EcoQoS off), `tuneTimingThread` (TIME_CRITICAL), `PointerAccelGuard` (temporarily turns off "Enhance pointer precision") | all of it |
| `profiles/human_start_256.csv` | the renderer profile: 256 real `dx,dy` reports (Y down), exported from `ABCurves/examples/data/human_start.npz` | the file |
| `../../ABCurves/runtime/c/` | the native renderer runtime (C99): `src/abc_online.c`, `abc_fixed.c`, `abc_w5_stream.c`, `include/*.h` | sources (the ABCurves submodule, commit `e197441`) |
| `../../ABCurves/models/renderer_global_h80.bin` | the renderer model, 44,484 bytes | the file |

From the mak-suite SDK (the `mak-suite` submodule, commit `2616b1c`) the reference needs only
`makxd::ApiOpcode` (opcode numbers) and, optionally, `Device::findDevices()` for finding the
port. **The SDK is not used in the hot path**: `Device::mouseMove()` blocks
(p99 ~19 ms) and can only do `MOVE 0x18`; the SDK has no Raw API and no `0x1F` interpolation.

### Test bench and diagnostics (not part of the module)

| File | Role |
| --- | --- |
| `makcu_mover.cpp` | CLI, F-key menu, `MakcuLink` (port search, SDK/raw, spread, interpolation, settings snapshot), measurements, summaries |
| `hook_monitor.h` | HID rate via the `WH_MOUSE_LL` LL hook (every event, no background thinning) |
| `rawinput_monitor.h` | path check via Raw Input (sums of deltas; it does not measure rate - in the background it is thinned to ~125 Hz) |
| `build.bat` | build: MinGW g++ C++23 + the static `libmakxd-cpp.a` + ABCurves C99 |

The early experiments from the root of `basics\` (Python scripts, `makcu_*1000hz*.cpp`, `makcu_driver_cpp.cpp`,
UDP clients, the root `*.md`) and the handoff notes `HANDOFF_PROMPT.md` were deleted on 2026-10-12.
They remain in git history (up to and including commit c579fd6).

### Reference parameters

| Parameter | Value |
| --- | --- |
| Transport | `RawComTransport`, `Format::RawMove`, `Mode::Async` |
| Port | COM5 (CH343, VID 1A86 PID 55D3), 4,000,000 baud, DTR/RTS off |
| Tick | 1 ms (1000 Hz), absolute QPC grid, spin wait |
| Raw | `timeout_uf = 0` (1 s), window min(12, free on the board − 2) of 16 records, ack after ≥ 2 ms, ack retry ≥ 1 ms |
| Renderer | `renderer_global_h80.bin` + `human_start_256.csv`, a new seed per run (or `--abc-seed`) |
| Priorities | process HIGH, timing thread TIME_CRITICAL, EcoQoS off, `timeBeginPeriod(1)` |
| Windows | "Enhance pointer precision" off while running (restored on exit) |
| Mouse in MAKCU | 1000 Hz (verified: Logitech Lightspeed 046D:C547, wired G502 X 046D:C098) |

## 3. Embedding in your own program (a minimal skeleton)

```cpp
#include "makcu_transport.h"
#include "abc_renderer.h"
using namespace mover;

// --- once ---
tuneProcessForTiming();                         // mover_common.h
RawComTransport tx;
std::string err, info;
if (!tx.open("COM5", err)) { /* port busy / the board did not answer */ }
tx.setFormat(RawComTransport::Format::RawMove);
if (!tx.prepareRaw(info)) {                      // metadata 0x6A(id=0)
    tx.setFormat(RawComTransport::Format::Mak);  // firmware without the Raw API: 0x18 (~640 Hz)
}

AbcRenderer abc;
std::vector<Report16> profile;
abc.loadModel("ABCurves/models/renderer_global_h80.bin", err);
AbcRenderer::readProfileCsv("profiles/human_start_256.csv", profile, err);
abc.prepareProfile(profile, err);                // ~3 ms, outside the hot path

// --- for each motion stream (run/gesture) ---
abc.beginEvent(seed, err);
// Timing thread (tuneTimingThread + PreciseWaiter), tick k every 1 ms by QPC:
//   Point d = source.intent(k);                 // smooth displacement for this ms (double)
//   Report16 r{};
//   abc.step(d.x, d.y, r);                      // often r = (0,0)
//   pendX += r.dx; pendY += r.dy;
//   if ((pendX || pendY) && tx.move(clamp16(pendX), clamp16(pendY))) pendX = pendY = 0;
//   // move() == false: the window/ring is busy - the delta stays in pend and goes out on the next tick
// If the thread is late by several ms - step the renderer for EACH missed ms
// (its clock is the 1 ms counts), and merge the reports into one send (this is what runMotion does).
tx.finish();                                     // wait for the receipts, send the carry-over (up to 300 ms)

// --- at the end ---
tx.close();
```

A ready model of this loop is `runMotion()` in `motion_engine.h` (the `renderer` branch).
Raw statistics come from `tx.extraStats()`: normally "sent = accepted into the FIFO = handed
to the USB endpoint", rejections 0, carry-over (0,0).

## 4. Build and run

```
build.bat                         # MinGW-w64 g++ 15 (C:\mingw64\bin), CMake for the SDK
makcu_mover.exe                   # F-key menu, rawmove transport by default
makcu_mover.exe --help
```

**Before running**, close any other `makcu_mover.exe`: COM5 is exclusive, and `build.bat`
will not overwrite a running exe. **Exit is END** (do not kill the process: that is how
"Enhance pointer precision" gets restored).

### Menu (global keys, the polling tester may be in focus)

| Key | Mode |
| --- | --- |
| F1 | 250 Hz, 30 s, current shape |
| F2 | **1000 Hz, 30 s - the main mode** |
| F3 | **ABCurves Renderer**, flicks to targets (minimum flick, Fitts, pauses), 30 s |
| F4 | the same flicks through the **accumulator** (`substepper.h`), 30 s + a speed -> rate table |
| F5 | **ABCurves Renderer**, circle, 30 s |
| F6 | sweep 125..2000 commands/s: HID + path, do not touch the mouse |
| F7 | sweep after a touch (touch the mouse during the first 6 s) |
| F8 | 1000 Hz, line 60 s - a test with the hand |
| F10 | **accumulator, speed ladder** 0.05..8 counts/ms, 2 s each (24 s) + a speed -> rate table |
| F9 | change the shape: circle → eight → line → flicks |
| F11 | mouse spread / interpolation cycling 0/5/10/25/50/100% (live; irrelevant for raw) |
| F12 | measure the hand for 15 s with no injection (hook calibration) + a hand speed -> rate table |
| ESC / the same F-key | stop the run |
| END | exit |

The menu's transport comes from `--tx` (`rawmove` by default; if the firmware has no Raw API -
automatically `0x18`).

### CLI

| Option | Meaning |
| --- | --- |
| `--port COM5` | port (by default - search via the SDK) |
| `--tx rawmove\|async\|sync\|km\|kmnow\|kmnowf\|sdk` | transport; `rawmove` is the reference, the others go through the firmware interpolator |
| `--hz N` | command rate (1000) |
| `--pattern circle\|eight\|line\|ramp\|wobble\|flicks`, `--speed V`, `--size S`, `--duration S` | test trajectory |
| `--abc`, `--abc-model`, `--abc-profile`, `--abc-seed` | run through the ABCurves Renderer (rate forced to 1000) |
| `--substep` | run through the accumulating substepper (1000 Hz); `--pattern steps` - the speed ladder |
| `--run` | one run without the menu |
| `--touch-test` | 30 s with audio cues: baseline / touch / after / sharp swing / after; summary by phase (ramp by default) |
| `--compare A,B,..` + `--rates N,..` | compare ways of sending at several rates |
| `--sweep`, `--sweep-speed` | sweep the command rate / the speed |
| `--check` | connection + timing on MOVE(0,0), the cursor stays still |
| `--probe` | raw board replies to the main queries |
| `--dump-settings` | a snapshot of all readable settings (run with `--no-sdk`), files `makcu_settings_fw<N>.txt` and `.makxd-settings` |
| `--no-sdk` | do not connect the SDK (port search only) |
| `--buttons-off` | turn off the button stream that the SDK enables |
| `--interp auto\|0..100` | `0x1F` interpolation live (for `0x18`/km.move; does not affect raw), restored on exit |
| `--spread N` | the same through the SDK settings (live) |
| `--poll-device` | `km.device()` in the live line |
| `--wait hybrid`, `--core N`, `--delay S`, `--keep-accel`, `--burst N`, `--pause-every/--pause-ms` | timing and diagnostics |

### How to read the output

The program prints these labels in Russian; the English meaning:

- `HID ... Гц` (HID ... Hz) - the LL hook, every mouse report that Windows saw (the hand = ~990 Hz).
- `отчётов на команду` (reports per command) - 1.00 for raw; > 1 means the firmware cuts the command up (the old path).
- `путь xN.NN` / `Путь: отправлено (...) пришло (...)` (path xN.NN / Path: sent (...) arrived (...)) - the sum of Raw Input counts; without the hand it should match.
- `raw 0x69: ...` - the board's receipts: sent / accepted into the FIFO / handed to the USB endpoint / rejected / immediate errors (no space) / ack and retries / window max / waited for the window / carry-over / last rejection reason.
- `отложено` (deferred) - ticks when the transport was busy (the delta went out on the next tick).

## 5. Results (short)

| commands/s | `MOVE 0x18` (interpolator) | **raw `0x69`** |
| --- | --- | --- |
| 125 | 500 Hz, 4 reports per command | 125 Hz, 1 to 1 |
| 250 | 640–686 Hz | 249–250 Hz, 1 to 1 |
| 1000 | 628–676 Hz, intervals 1-2 ms | **997–1001 Hz, 99.7–100% at 1 ms, exact path** |
| 2000 | 670 Hz, path 0.68 (losses) | 997 Hz, path preserved (back-pressure) |

ABCurves (F5, circle, 1000 Hz, Lightspeed): HID 997 Hz, external tester 999–1001 Hz,
path (−188, −145) = (−188, −145), 14,510 commands - 14,510 handed to USB, 0 rejections.
In detail - sections 4 and 7 in [ENG_SOLUTION_1000HZ_RAW_MOVE.md](ENG_SOLUTION_1000HZ_RAW_MOVE.md).

## 6. Current MAKCU settings

A snapshot of 2026-10-10 19:23, with the command `makcu_mover.exe --no-sdk --dump-settings` (read-only).
Files alongside:
- [ENG_makcu_settings_fw4094.txt](ENG_makcu_settings_fw4094.txt) - the full snapshot (translated; the original is [makcu_settings_fw4094.txt](makcu_settings_fw4094.txt));
- `makcu_settings_fw4094.makxd-settings` - the official encrypted export (789 bytes).
  Restore: app.makcu.com → Import or the SDK `importDeviceSettings()` (applies live;
  to survive power-off - Save separately).

Hardware: MAKCU on **COM5** (USB-Enhanced-SERIAL CH343, VID 1A86 PID 55D3), **4,000,000 baud**,
firmware **4094**. In the host port at the last checks - Logitech Lightspeed (046D:C547);
the wired G502 X (046D:C098) was checked as well.

| What | Value | What it means for us |
| --- | --- | --- |
| Firmware (`0x04`) | **4094** | = V4.085 + controller sticks; without the V4.086 fix (not needed, we use raw) |
| Routes (`0x02`, `km.device()`) | `0x03` = mouse + keyboard; `R:MK;M:8uf;K:8uf;C:0uf` | mouse period 8 microframes = 1 ms → a ceiling of 1000 Hz |
| Interpolation (`0x1F`) = mouse spread | **0** (factory AUTO = 255) | affects only `0x18`/`km.move`; does not affect raw. Whether 0 is saved in NOR is not known |
| COM speed (`km.baud()`) | 4000000 | reference |
| KM echo (`km.echo()`) | 0 | mutations are silent (needed for raw/km without extra traffic) |
| Button stream (`0x10`, `km.buttons()`, `INPUT_STREAM mouse`) | 1 | enabled by the SDK on connect; button events go to COM, our read thread drops them |
| Keyboard/controller streams | 0 / 0 | |
| Physical input locks (`km.lock_*`, masks) | all 0 | the physical mouse is not blocked by anything |
| Virtual screen (`km.screen()`) | 1920×1080 | only for `km.moveto`/`km.getpos`, we do not need it |
| Raw API (`0x6A` id=0) | schema 1, `last_accepted_id` 92782, **free 16 of 16** | ids are reset on a board reboot |
| Settings (SDK) | sections `0x04` (mouse only), kinds `0x03`, save_state 0, revision 2486104977 | the controller is not connected - controller sections at defaults |
| Controller processing | interpolation auto, buffer 8 ms, behavior 0 "Linear", the rest off | not related to the mouse |
| Translation (mouse/keyboard → controller) | all 4 channels off | |

The full snapshot:

```text
# MAKCU settings snapshot, 2026-10-10 19:23, port COM5, 4000000 baud

## MAK_API (GET)
- 0x02 DEVICE (kinds): 03
- 0x04 FIRMWARE_VERSION: FE 0F 00 00 = 4094
- 0x1F interpolation (255=AUTO): 00 = 0
- 0x10 BUTTONS stream: 01
- 0x2B KEY_KEYS stream: 00
- 0x41 CONTROLLER_STREAM: 00
- 0x52 INPUT_STREAM mouse: 01
- 0x52 INPUT_STREAM keyboard: 00
- 0x52 INPUT_STREAM controller: 00
- 0x6A RAW_STATUS id=0 (metadata): 00 01 6E 6A 01 00 10 10 = schema 1, last_accepted_id 92782, free 16/16

## KM_API (queries)
- km.version() -> km.MAKCU
- km.device() -> R:MK;M:8uf;K:8uf;C:0uf
- km.baud() -> 4000000
- km.echo() -> 0
- km.buttons() -> 1
- km.keys() -> 0
- km.controller() -> 0
- km.stream(mouse) -> 1
- km.stream(keyboard) -> 0
- km.stream(controller) -> 0
- km.screen() -> km.screen(1920,1080)
- km.getpos() -> km.getpos(0,897)
- km.moving() -> 0
- km.left() / right() / middle() / side1() / side2() -> 0 / 0 / 0 / 0 / 0
- km.lock_mx() / my() / mw() -> 0 / 0 / 0
- km.lock_mx+() / mx-() / my+() / my-() / mw+() / mw-() -> 0 / 0 / 0 / 0 / 0 / 0
- km.lock_ml() / mr() / mm() / ms1() / ms2() -> 0 / 0 / 0 / 0 / 0

## Device settings (SDK readDeviceSettings)
- info: sections 0x04 (1=controller 2=translation 4=mouse), kinds 0x03, save_state 0, revision 2486104977
- mouse_spread_percent (= interpolation 0x1F): 0
- controller: interpolation 2 (0 off, 1 fixed, 2 auto), buffer_ms 8, timing_variance 0%, curve_enabled 0, legacy_strengths 0/0/0, profile 0 of 1
  - behavior 0 (right stick) 'Linear': enabled 1, strength 0% (present 0), curve 0, advanced 0, inertia 0, micro 0, limit 0, magnitude_var 0, angle_var 0
    curve[0]: deadzone 0, anti_deadzone 0, deadband 0, points (0,0) (25,25) (50,50) (75,75) (100,100)
  - behavior 1 (left stick) '': enabled 0, strength 0% (present 0), curve 0, advanced 0, inertia 0, micro 0, limit 0, magnitude_var 0, angle_var 0
    curve[1]: deadzone 0, anti_deadzone 0, deadband 0, points (0,0) (0,0) (0,0) (0,0) (0,0)
  - behavior 2 (left trigger) '': enabled 0, ... (all 0), curve[2] points (0,0) x5
  - behavior 3 (right trigger) '': enabled 0, ... (all 0), curve[3] points (0,0) x5
- translation 0 (left stick/WASD): enabled 0, scale 1, timeout_ms 50
- translation 1 (right stick/mouse): enabled 0, scale 1, timeout_ms 50
- translation 2 (left trigger): enabled 0, scale 100, timeout_ms 50
- translation 3 (right trigger): enabled 0, scale 100, timeout_ms 50
- settings image (400 bytes: 0..371 controller, 372..395 translation, 396 mouse spread, 397..399):
    000: 00 00 00 00 02 00 00 00 08 00 00 00 00 00 00 00 01 00 00 00 01 00 00 00 06 0F 00 00 4C 69 6E 65
    032: 61 72 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 05 00 00 19 19 32 32 4B 4B
    064: 64 64 00 00 00 05 00 00 19 19 32 32 4B 4B 64 64 00 00 00 05 00 00 19 19 32 32 4B 4B 64 64 00 00
    096: 00 05 00 00 19 19 32 32 4B 4B 64 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    128..351: all 00
    352: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 32 00 00 00 01 00 32 00
    384: 00 00 64 00 32 00 00 00 64 00 32 00 00 00 00 00
```

Not readable through the API and not part of the snapshot: USB identity override, descriptor interval,
LED, network, activation, saved controller presets (these are separate Toolkit operations).

## 7. Rules and pitfalls

- **Do not mix `0x18`/`km.move` and raw** in one stream: raw waits until the AUTO queue is empty.
- **Raw receipts are mandatory**: the board has at most 16 live/unread records; without
  `0x6A consume=1` after 16 requests the board starts answering "no space".
- **Raw ids strictly increase within one board boot** and are shared by all programs on this
  COM/UDP. Before working - `prepareRaw()` (it takes `last_accepted_id`). After the board
  reboots the count starts over - `prepareRaw()` again.
- **Unconfirmed receipts live on the board until it reboots** and occupy its 16 places.
  Found at the snapshot of 2026-10-10: 3 records (id 64886–64888, state 2 "handed to USB"), the tail of an
  interrupted run - most likely the console was closed with the X in the middle of a run and the process
  was killed before `finish()`. Confirmed manually. Safeguards in the code: the console-close handler
  waits up to 2 s for the run to stop and call `finish()`; `prepareRaw()` confirms
  final records among the last 4096 ids and sets the window = min(12, free − 2);
  a write does not overwrite slot `id % 64` while an old record is stuck in it. Only a board reboot
  (USB reconnect) or confirming the exact id fully clears it.
  Check: `--probe` or `--dump-settings` → the line `0x6A ... free N/16`.
- **A delta that does not fit into the descriptor's X/Y field is rejected** (reason 17), the board does not
  cut it up. The current code does not handle this (see section 8).
- **HID rate = rate of non-zero commands.** A standing trajectory = no reports (like a
  real mouse).
- The mouse in the host port is 1000 Hz, otherwise the ceiling = its polling rate.
- Measure the rate with the LL hook or an external tester in focus; the Raw Input of a background window on
  Windows 11 is thinned (~125 messages/s), while the sums of deltas stay correct.
- Do not use the SDK's `Device::mouseMove()` in the hot path (blocking for up to tens of ms).
- Interpolation `0x1F` (= "Mouse injection" in app.makcu.com) does not affect raw. The factory
  value is AUTO (255); on this board it is 0 now (see section 6).
- Exit only through END/ESC/Ctrl+C - otherwise "Enhance pointer precision" will not be restored.

## 8. What is not verified and the plan for the next chat

The next step (per the user): **remove the hardcoded trajectories** - motion
will arrive from outside, and the logic will get more complex. What to keep when reworking:
1. The motion source must supply a **smooth intent for every 1 ms** (or a position
   as a function of time) - this is the renderer's input. The renderer steps strictly once per 1 ms, including
   ticks missed by the OS.
2. The timing loop is an absolute QPC grid, no `Sleep` drift, no I/O blocking
   (like `runMotion`). Carry the delta over when `move() == false`.
3. Transport - `RawComTransport` + `Format::RawMove`, `finish()` when the
   motion stream ends, `prepareRaw()` at start and after the board reconnects.
4. For every new motion stream - `beginEvent(seed)` on the renderer.

Not verified / open:
- a formal `--touch-test --hz 1000` on raw with a summary table (there is only a visual check);
- the HID rate when the hand and injection move at the same time;
- a long run of 10+ min at 1 kHz (the maker of the Raw API has not checked this either);
- slicing a delta at the boundary of the descriptor's X/Y field (reason 17 would now go into endless
  carry-over if a single delta is larger than the field; at 1 kHz deltas are small, the risk is the catch-up after an OS freeze);
- behaviour when the board reboots in the middle of a stream (unconfirmed deltas may be repeated);
- closing the console in the middle of a run after the handler fix (checked only by building);
- two programs with the Raw API at the same time (shared ids).

## 9. Sources

- [ENG_SOLUTION_1000HZ_RAW_MOVE.md](ENG_SOLUTION_1000HZ_RAW_MOVE.md) - the full write-up of the solution.
- `https://makcu.com/api/raw-v4074.md` - the Raw API contract; `https://makcu.com/api/versions` -
  opcodes by build; `https://makcu.com/en/api/` - commands and changelog;
  `https://makcu.com/mcp` - the official MCP server (build history `firmware_versions`).
- `mak-suite\protocol\*.md` - MAK_API/KM_API (without the Raw API and `0x1F`).
- `../../ABCurves/runtime/c/README.md`, `../../ABCurves/models/README.md` - the renderer.

### The "speed -> rate" measurement (F3/F4/F5/F10/F12, `--substep`, `--abc`)

Runs with a 1 ms step (renderer or accumulator) and the hand measurement print a table by speed
bins at the end (the centres are the F10 ladder steps) and write a per-millisecond series to `rate_substep.csv` /
`rate_abc.csv` / `rate_hand.csv` (`ms,speed,report,hid`).

- `генератор, Гц` (generator, Hz) - non-zero reports of the substepper/renderer; `HID, Гц` - LL hook events (with raw 0x69 they match).
- `интервал: медиана` (interval: median) and `разброс CV` (spread CV, SD/mean) - regularity. Accumulator: rate = speed x 1000
  up to 1 count/ms, then 1000 Hz, the intervals are even (CV ~ 0). The hand and the renderer are "ragged".
- The hand's speed - from the difference of the hook's `pt`: correct at pointer speed 10/20, with "Enhance pointer
  precision" off and the cursor not at the screen edge. Do not touch the mouse during injection runs.

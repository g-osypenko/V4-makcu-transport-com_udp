# MAKCU: 1000 Hz injection and a stable trajectory. How it was solved

*English translation of [`SOLUTION_1000HZ_RAW_MOVE.md`](SOLUTION_1000HZ_RAW_MOVE.md) (Russian original). Source-code comments and program output are still in Russian; the labels of the raw statistics line are translated in section 6.*

Date: 2026-10-10. Board firmware: **4094** (a test build; by the build history it is
"the official V4.085 + controller stick setting"). COM5, CH343, 4,000,000 baud.
Program: `makcu_mover.exe` from this folder.

## 1. In short

- **The cause was in our way of sending, not in a "firmware ceiling".** We sent
  movement with the MAK_API command `MOVE 0x18` (the same one the mak-suite SDK uses). In the firmware
  this command goes through the **interpolator** (AUTO / "Mouse injection"): it cuts each
  command into several USB reports and emits them at its own pace, around 640–690 Hz.
  `km.move` takes the same path and, on firmware of the V4.085 line (including 4094), so does `km.move_now`.
- **The firmware has a second path: Raw movement, commands `0x69` and `0x6A`.** It is a
  FIFO queue in which every command goes out as **exactly one HID report**, bypassing the
  interpolator, together with the physical mouse movement in that same report. This path is
  described only on makcu.com: it is not in the mak-suite SDK or its documentation.
- After switching to `0x69` we got **997–1001 Hz at 1000 commands/s**: 99.7–100%
  of intervals at 1 ms, and the "sent = arrived in Windows" path matches to the count.
  This holds on the Lightspeed receiver and the wired G502 X, on a circle, a line and with the ABCurves
  Renderer. The user checked it with an external tester (999–1001 Hz) and visually:
  the symptom with the touch is gone to the eye.
- The conclusion from `README.md` ("the injection rate is limited by the stock firmware, nothing
  can be done from the PC") is **wrong**. See section 11.

## 2. The original task and the symptom

Requirements (from the original handoff note; deleted on 2026-10-12, it is in git history):
1. A cursor-movement module in C++ through MAKCU over COM, to import into other programs.
2. The speed and shape of the trajectory must not change when you touch the real mouse.
3. The HID report rate during injection is above 700 Hz, ideally around 1000, and it does not
   sag after touching the mouse.
4. Optionally: the ABCurves Renderer for slicing the trajectory into reports.

Symptoms before the solution:
- injection through `0x18` gave ~615–690 Hz, even when the PC sent exactly 1000 commands/s;
- after touching the real mouse the rate dropped to ~250–460 Hz and stayed there until
  the end of the run, and sometimes between runs;
- at 3000 commands/s the cursor after the touch went "faster and wider". The cause: before the touch
  41% of the path reached Windows, after the touch 87%.

## 3. What is actually going on

### 3.1 The chain

```
program on the PC --COM5 (CH343, 4 Mbaud)--> MAKCU (2 x ESP32-S3) --USB--> PC
                                                  ^
real mouse (MAKCU host port) ---------------------+
```

MAKCU **clones the USB descriptor** of the attached mouse, down to the endpoint's polling
interval. The PC polls MAKCU at the same interval as the mouse itself, and
every HID report is the physical mouse movement plus the injection.

### 3.2 Two injection paths in the firmware

| | AUTO / interpolator path | Raw path (`0x69`/`0x6A`) |
| --- | --- | --- |
| Commands | `MOVE 0x18`, `km.move`, `km.move_now` (on the V4.085 line) | `0x69 RAW_MOVE`, receipts `0x6A RAW_STATUS` |
| What it does with a command | spreads it over several reports: "Interpolates injected mouse movement over time", and even "0% still interpolates" | puts it in the FIFO, then **one whole report**, added to the physical movement of that report |
| Emission pace | its own: ~640 Hz with the Lightspeed, ~680 Hz with the G502 X | one report per command: the pace = the pace of our commands, but not above the USB polling rate |
| Setting | MAK_API `0x1F` = the "Mouse injection / Interpolate" slider in app.makcu.com: 0..100 or 255 = AUTO | not needed |
| Feedback | none | for every command: queued / handed to the USB endpoint / rejected with a reason |

### 3.3 Why 0x18 / km.move gave ~640 Hz

The measurement (section 7.1): at 125 commands/s there were **exactly 4.00 reports per command**
(500 Hz). At 250–1000 commands/s the output sat at ~640 Hz with intervals of about
40–45% at 1 ms and 55–60% at 2 ms, with any command format. The output rate does not
follow the command rate: at 250 commands/s there are more reports than commands, so the
board makes the extra ones itself. This is the behaviour of the interpolator, and it is officially described:
- app.makcu.com, the slider tooltip: "Interpolates injected mouse movement over
  time while preserving the total X/Y distance... **0% still interpolates**";
- the build history, V4.086 (test): "Fixes km.move_now / MOVE_NOW and interpolation 0
  still being smoothed over several USB reports (aim tools could overshoot or circle):
  the move now goes out in the next free USB report... AUTO interpolation behaves the
  same way when commands arrive faster than the mouse reports. This build is based on
  V4.085". **4094 does not have this fix**: 4094 is based on 4085.

Why the interpolator's pace is exactly ~1.56 ms and not 1 ms is unknown: the firmware is closed.
The pace depends slightly on the attached mouse (640 Hz with the Lightspeed, 676–686 Hz with the G502 X).

### 3.4 Why the touch "broke" the trajectory

- The touch changed the interpolator's mode (in past sessions ~615 → 250–460 Hz). The mode
  held until the mouse moved again. The official wording "...when commands
  arrive faster than **the mouse reports**" says the firmware takes the real mouse's report rate into account.
  The exact mechanism is **not confirmed**.
- At 3000 commands/s the interpolator lost part of the path, and the share of loss depended on the
  mode: 41% before the touch against 87% after. Hence "faster and wider".
- At ≤1000 commands/s the path through the interpolator arrived exactly, but the HID rate
  sagged.
- The Raw path does not use the interpolator. Each command is one report, and the physical
  movement is simply added to it in that report. We set the pace, the path is exact.

### 3.5 Why a 125 Hz mouse limits injection

The user plugged in a wired mouse with 125 Hz polling, and injection did not rise
above 125 Hz. This is expected: MAKCU clones the polling interval, and the PC polls the board
once per 8 ms. **For 1000 Hz the host port needs a 1000 Hz mouse or receiver.**
The wired G502 X and the Lightspeed (046D:C547) are suitable.

### 3.6 Why earlier sessions did not find this

- They worked with the mak-suite SDK. Its `MAK_API.md`, `KM_API.md` and `makxd_protocol.h`
  have neither `0x69`/`0x6A` nor `0x1F`. Movement in the SDK is only `MOVE 0x18`.
- The Raw API is described only on makcu.com (`/api/versions`, `/api/raw-v4074.md`) with
  the note "test, opt-in", and the maker writes that it has not tested 1 kHz itself.
- The smoothing at interpolation 0 was taken for a "firmware ceiling", although the official
  build history directly calls it a bug and names the build with the fix (4086).

## 4. The course of the investigation (chronology)

All runs: a ramp trajectory (to the right only, 4 px/ms, the path in Raw Input is counted
exactly), the HID rate from the `WH_MOUSE_LL` LL hook (calibration: the hand ≈ 990 Hz), the mouse
not touched unless stated otherwise.

1. **`--probe`, a snapshot of the board.** Firmware `0x0FFE` = 4094. `0x1F` → `DE AD 01 00 1F 00`:
   interpolation really is 0, not AUTO. `BUTTONS` = 1 (the SDK enables the button stream
   on connect). `km.version()` = `km.MAKCU`, `km.device()` =
   `R:MK;M:8uf;K:8uf;C:0uf`, `km.echo()` = 0. `km.move_now(0,0)` and its variant in a
   MAK_API frame were accepted silently, with no ERR.
2. **`--compare` of four ways of sending** (`0x18`, `km.move`, `km.move_now`,
   `km.move_now` in a `'k'` frame): all four identical. 125 commands/s → 500 Hz
   (4.00 reports per command), 250–1000 commands/s → 628–642 Hz. Conclusion: the format is not the cause.
3. **The button stream off (`--buttons-off`).** 639–644 Hz, path 1.00. No effect.
4. **Without the SDK at all (`--no-sdk`).** 500 / 640 / 636 Hz. The SDK has no effect.
5. **`--check`, the PC side.** 2000 records out of 2000 in 2 s, 0 errors, a call averaging
   2.1 µs (max 79), no more than 1 in flight, tick lateness averaging 0.4 µs
   (max 85). Sending is clean.
6. **The user.** A 125 Hz mouse limited injection to 125 Hz. The wired G502 X
   gave ≤700 Hz, the symptom stayed. So the Lightspeed is not to blame.
7. **A review of the official sources:**
   - app.makcu.com (the Toolkit JS bundle): `0x1F` with one byte is a SET (0..100 or 255),
     the board replies with the value; AUTO has been the default since V4.041; "0% still interpolates";
     `0x3F` is a command-transport test, not a HID-rate test;
   - the MAKCU MCP server (`https://makcu.com/mcp`, the `firmware_versions` tool): the history
     and lineage of all builds. From there: 4094 = 4085 + sticks, the smoothing fix
     only in 4086;
   - `https://makcu.com/api/versions` and `https://makcu.com/api/raw-v4074.md`: the full
     Raw API contract. By the opcode matrix `0x69`/`0x6A` exist in all builds from 4074
     to 4094.
8. **The `rawmove` transport was implemented** (sections 5–6). First run, 1000 Hz, 3 s:
   HID 998–1001 Hz, 100% of intervals at 1 ms, path (12000, 0) = (12000, 0);
   3000 commands sent, 3000 accepted, 3000 handed to USB, 0 rejections.
9. **`--compare async,rawmove` at 125–2000 commands/s** (section 7.2) and a 5 s circle:
   one report per command at all rates, 1000 Hz at 1000 commands/s.
10. **Raw made the default transport**, the F1–F8 presets go through it too.
    The user checked F1 (250 Hz) and F5 (ABCurves, circle, 1000 Hz) with the LL hook and an
    external tester: 250 and 999–1001 Hz, the path matches (section 7.3).

## 5. Raw movement: the protocol

Source: `https://makcu.com/api/raw-v4074.md` (in Chinese) and `https://makcu.com/api/versions`.
Everything is little-endian, a frame as in MAK_API: `DE AD | LEN:u16 | CMD:u8 | PAYLOAD`
(LEN without the command byte).

### 0x69 RAW_MOVE: send a movement

The payload is exactly 10 bytes:

| field | type | meaning |
| --- | --- | --- |
| request_id | u32 | not 0, **strictly increasing** within one board boot (shared by all programs) |
| x | i16 | full X displacement |
| y | i16 | full Y displacement |
| timeout_uf | u16 | 0 = the default 8000 (1 s); 1..16383 units of 125 µs |

The reply to each send is 6 bytes `state:u8 reason:u8 request_id:u32`, that is, a
frame `DE AD 06 00 69 ...`:
- `state=1, reason=0`: accepted into the FIFO, **but not yet sent**;
- `state=255`: an immediate error, the request was not accepted (reasons below).

Behaviour:
- the board checks the coordinates against the mouse descriptor. What does not fit into one
  HID report is **rejected** (reason 17), with no cutting or clipping;
- Raw first waits until the AUTO queue is empty (if you mixed it with `0x18`),
  then takes **exactly one report** and is added to the physical movement of that
  report. If the sum does not fit, the physical report goes out as is, and Raw waits for the
  next one;
- the timeout counts from the moment the request reached the head of the FIFO;
- 125 µs is only a unit of time. The maker separately writes that it does not
  mean 8 kHz and does not promise 1 kHz.

The example is built from the specification, it is not a captured dump. Here id = 1, x = +4, y = 0,
the default timeout:
```
-> DE AD 0A 00 69  01 00 00 00  04 00  00 00  00 00
<- DE AD 06 00 69  01 00  01 00 00 00            (state 1: in the FIFO)
```

### 0x6A RAW_STATUS: find out and confirm the result

The payload is 5 bytes: `request_id:u32 consume:u8` (0 or 1).
- For an ordinary id the reply is the same, 6 bytes. `state 1` = still in the queue,
  `2` = **handed to the USB endpoint** (success, reason 0), `3` = a final rejection.
  Rejection reasons: 17 — range/descriptor, 18 — timeout, 20 — the target disappeared before
  sending, 21 — endpoint reset.
- `consume=0` only reads. `consume=1` reads and frees the record, but only
  if it is already in a final state. For a record in the queue it returns error 6,
  and the record stays.
- `request_id=0, consume=0` — metadata, 8 bytes:
  `0:u8 schema=1:u8 last_accepted_id:u32 free_records:u8 capacity=16:u8`.

```
-> DE AD 05 00 6A  01 00 00 00  01
<- DE AD 06 00 6A  02 00  01 00 00 00            (state 2: handed to USB)
-> DE AD 05 00 6A  00 00 00 00  00               (metadata)
<- DE AD 08 00 6A  00 01  B1 38 00 00  10 10     (last_accepted_id 14513, free 16 of 16 - values from a real run)
```

Immediate errors (`state=255`):

| reason | meaning |
| --- | --- |
| 1 | wrong length, id, consume or timeout |
| 2 | the id is not greater than one already accepted in this boot (replay protection) |
| 3 | no space: the 16 records or the command queue are full; can be retried with the same id |
| 4 | no mouse or no needed field in the descriptor |
| 5 | there is no such id or it has already been confirmed |
| 6 | `consume=1` for a record that is still queued (not deleted) |
| 7 | a firmware update or reboot is in progress |

Important:
- the board has **at most 16 live or unread records**, and they must be
  confirmed (`0x6A consume=1`);
- an id cannot be reused. After a real board reboot the history and the
  id counter are reset. Reconnecting the port and an endpoint reset do not reset them.

## 6. How the `rawmove` transport is built in our code

File `makcu_transport.h`, class `RawComTransport`, format `Format::RawMove`.

1. **Preparation** (`prepareRaw`). A metadata query `0x6A(id=0)`; `last_accepted_id` is taken from the reply,
   and the next id = it + 1. If there is no reply (firmware without the Raw API),
   `MakcuLink::acquire` prints a warning and switches to `MOVE 0x18`.
   If records on the board are occupied (`free_records < 16`), the final
   records among the last 4096 ids are confirmed. Window = min(12, free − 2).
   Added after a finding: 3 hanging receipts from an interrupted run, see section 10.
2. **Sending** (`moveRaw`), on every engine tick:
   - the tick's delta gets the "carry-over" added — deltas that the board rejected or did not
     accept, so the path is not lost;
   - the record `id % 64` is set up (phase 1: waiting for the reply to `0x69`), and a `0x69` frame is assembled;
   - into the **same** `WriteFile` up to 3 `0x6A consume=1` frames are appended for records
     whose time has come: accepted more than 2 ms ago, a retry no more than once per 1 ms; and
     a repeat if the ack's reply was lost (20 ms) or the reply to `0x69` was lost (50 ms);
   - **its own window is 12 live records** (the board has 16). If the window is full, `move()`
     returns false, the engine keeps the delta and hands it over on the next tick
     (back-pressure, no losses). This is the "waited for the window" counter.
3. **Receiving** (`parseRaw`/`onRawReply`, the COM read thread):
   - `0x69 state 1` → phase 2 (in the FIFO);
   - `0x6A state 2` → "handed to USB", the record is freed;
   - `0x6A state 3` → "rejected", the delta goes back to the carry-over;
   - `255/6` → the record is still in the queue, repeat the ack later;
   - other `255` (3 — no space, 5 — no id, etc.) → the delta goes back to the
     carry-over, the record is freed. For `255/2` the next id shifts by +1000:
     meaning someone has taken the numbers.
4. **End of a run** (`finish()`, called at the end of `runMotion`). For up to 300 ms it
   confirms the remaining records and sends the carry-over.
5. **Statistics** — the line "raw 0x69: ..." in the summary (the program prints it in Russian; the English meaning):
   - "отправлено" (sent), "принято в FIFO" (accepted into the FIFO), "отдано в USB-endpoint" (handed to the USB endpoint), "отказ" (rejected);
   - "немедл. ошибок" (immediate errors) and in brackets "нет места" (no space);
   - "ack, повторов" (ack, retries);
   - "окно max" (window max), "ждали окно" (waited for the window);
   - "перенос сейчас" (carry-over now), "посл. причина" (last reason).

   Normally all the first counters are equal, rejections and errors are 0, carry-over (0,0).

The hot path did not change: its own overlapped COM5 handle, a ring of 32 entries,
a QPC schedule, the `motion_engine.h` engine (position is a function of time,
tick delta = `round(p) − already_sent`). The mak-suite SDK remains only for
port search and, without `--no-sdk`, for the handshake and reading settings.

**Consequence for use:** in Raw mode the HID rate = the rate of our
non-zero commands, not above the USB polling rate. If you need 1000 Hz, send 1000 commands/s, and
on every tick there must be a non-zero delta. An empty tick = no report, like a
real mouse that stands still.

## 7. Results

### 7.1 The AUTO path (`0x18` etc.), Lightspeed, firmware 4094

| commands/s | `0x18` | `km.move` | `km.move_now` | `move_now` in a frame | reports per command |
| --- | --- | --- | --- | --- | --- |
| 125 | 500 Hz | 500 | 500 | 500 | 4.00 |
| 250 | 638–643 | 637 | 631–644 | 642–644 | ~2.56 |
| 500 | 639–643 | 639 | 638–644 | 552–643 | ~1.28 |
| 1000 | 628–639 | 628 | 628–639 | 627–639 | ~0.64 |

Intervals ~40–46% at 1 ms and ~53–59% at 2 ms; `km.device()` was `M:8uf` all the time.
Twice the `km.move_now` path was 1.14 and 1.11. On a repeat with the button stream off,
it was 1.00 everywhere, so a mouse touch is more likely, but this is not proven.

### 7.2 AUTO versus Raw, wired G502 X (046D:C098), firmware 4094

| commands/s | `0x18`: HID | reports/command | path | **raw 0x69: HID** | reports/command | path | raw intervals |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 125 | 500 | 4.00 | 1.00 | **125** | 1.00 | 1.00 | 100% > 2.5 ms (8 ms) |
| 250 | 686 | 2.75 | 1.00 | **249** | 1.00 | 1.00 | 100% > 2.5 ms (4 ms) |
| 500 | 682 | 1.37 | 1.00 | **499** | 1.00 | 1.00 | 100% at 2 ms |
| 1000 | 676 | 0.68 | 1.00 | **997** | 1.00 | 1.00 | 99.7% at 1 ms |
| 2000 | 670 | 0.34 | **0.68** | **997** | 0.50 | 1.01 | 100% at 1 ms |

Raw at 125–1000 commands/s: 0 rejections, 0 errors, 0 ack retries, window no more than 4.
At 2000 commands/s the board runs into 1 report per USB frame. The window filled up
("waited for the window" 3987), the engine merged deltas, the path was preserved. There are many ack retries
(19786), because records sit in the queue. **The working point is 1000 commands/s.**

Separate raw runs, G502 X:
- ramp 3 s, 1000 Hz: average 998 Hz, median 1001, 100% of intervals at 1 ms, path (12000, 0) = (12000, 0);
- circle 5 s, 1000 Hz: average 997 Hz, median 1000, 100% at 1 ms, path (−123, 147) = (−123, 147), 5000/5000 handed to USB.

### 7.3 The user's check: Lightspeed (046D:C547), default transport raw

| preset | HID (LL hook) | external tester | intervals | path sent = arrived | raw |
| --- | --- | --- | --- | --- | --- |
| F1, 250 Hz, circle, 23.5 s | 250 Hz | 250 Hz, σ 0.059 ms | 99.9% in 2.5–5 ms (4 ms) | (−34, −96) = (−34, −96) | 5884 / 5884 / 5884, 0 rejections |
| F5, ABCurves, circle, 1000 Hz, 14.5 s | 997 Hz (median 997, worst 996) | 999 Hz average, 1001 instantaneous, σ 0.028 ms | 99.7% at 1 ms, 0.2% at 2 ms | (−188, −145) = (−188, −145) | 14510 / 14510 / 14510, 0 rejections |

ABCurves in F5 produced 998 non-zero reports per second. Before, the interpolator
smeared those reports; now every 1 ms renderer report arrives as is.
Visually, per the user, there is no touch symptom.

## 8. What was changed in the code

### `makcu_transport.h`
- `Transport`: added the virtual `finish()`, `extraStats()`, `resetStats()`.
- `RawComTransport::Format`:
  - `Mak` (0x18);
  - `KmMove` (`km.move(x,y)\r\n`);
  - `KmMoveNow` (`km.move_now(x,y)\r\n`);
  - `KmMoveNowFramed` (`DE AD LEN 'k' "m.move_now(x,y)"`);
  - **`RawMove`** (0x69/0x6A).
- The shared write path was split out into `sendFrame()`, frames are assembled into a buffer of up to 64 bytes.
- `transact()`: send bytes and return everything that arrived within N ms (for `--probe`,
  `0x1F`, Raw metadata).
- Raw: `prepareRaw()`, `moveRaw()`, `appendAcksLocked()`, `parseRaw()`,
  `onRawReply()`, `finish()`, `extraStats()`, plus the records, window, carry-over and
  counters. The description is in section 6.

### `motion_engine.h`
- At the end of `runMotion()`, `tx.finish()` is called: confirm the receipts and send the carry-over.

### `makcu_mover.cpp`
- `--tx rawmove|async|sync|km|kmnow|kmnowf|sdk`. **The default is `rawmove`**, and if
  there is no Raw API the program switches to `0x18` by itself.
- The F1–F8 presets no longer set their own transport and do not touch spread; the transport
  comes from `--tx`. F2 — 1000 Hz, 30 s (the main mode). F8 — 1000 Hz, line
  60 s, a test with the hand.
- Diagnostics:
  - `--probe`: raw replies of `0x04`/`0x1F`/`0x10`, `km.version/device/echo/buttons/moving`,
    acceptance of `km.move_now`;
  - `--compare A,B,..` together with `--rates N,N,..`: the ways of sending one after another at
    each rate; a table of HID, reports per command, path, intervals, `M:`, the raw line;
  - `--touch-test`: 30 s with audio cues, phases "baseline → touch → after →
    sharp swing → after", a summary by phase; the path in phases without the hand is pure injection;
  - `--no-sdk`: do not connect the SDK at all;
  - `--buttons-off`: turn off the `BUTTONS=0` button stream that the SDK enables;
  - `--interp auto|0..100`: `0x1F` interpolation live, the original one is restored on exit;
    **not run on hardware**;
  - `--dump-settings` (with `--no-sdk`): a snapshot of all readable board settings into
    `makcu_settings_fw<N>.txt` and the official `.makxd-settings` export; the result
    is pasted into `README.md`, section 6;
  - `--poll-device`: `M:..uf` from `km.device()` in the live line; **not run on hardware
    separately**, the same query works in `--touch-test`.
- The summaries have "reports per command" and the raw statistics line.

## 9. How to use

```
build.bat
makcu_mover.exe                         # menu F1..F8, raw transport by default
makcu_mover.exe --run --hz 1000 --pattern circle --duration 20
makcu_mover.exe --run --abc --pattern flicks --duration 30
makcu_mover.exe --touch-test --hz 1000                 # touch check (the hand is the user)
makcu_mover.exe --compare async,rawmove --rates 125,250,500,1000
makcu_mover.exe --probe                                # the cursor does not move
makcu_mover.exe --tx async ...                         # the old path through the interpolator, for comparison
```

Before running:
- **close any other `makcu_mover.exe`**: COM5 is opened exclusively, and `build.bat`
  cannot overwrite the exe;
- exit through END, do not kill the process: that is how "Enhance pointer precision" gets restored.

For embedding in other programs you need `RawComTransport` in
`Format::RawMove` mode and `runMotion()` (or your own loop). On every 1 ms tick —
`move(dx, dy)`, at the end — `finish()`.

## 10. What is not verified, and the risks

1. **A formal touch test on Raw is not recorded.** There is only the user's visual check
   and runs without the hand. Need to run `--touch-test --hz 1000` and
   save the summary by phase: HID and path in the phases without the hand, HID during the touch.
2. **The HID rate when the hand and injection move at the same time** was not measured on Raw.
3. **A long run.** The longest runs are 14.5 s at 1000 Hz and 23.5 s at 250 Hz.
   The maker writes that it has not tested 1 kHz and long-term stability of Raw itself. A
   run of 10+ minutes without the hand is needed: watch rejections, "no space", carry-over.
4. **A too-large delta.** The board rejects it with reason 17 rather than cutting it. Our code
   returns the rejected delta to the carry-over and sends it again in full. If a single delta is
   larger than the X/Y field in the descriptor, it will be rejected over and over. At 1000 Hz deltas
   are small, but after an OS freeze the engine catches up with a single delta of up to 25 ms of path.
   The width of the X/Y fields of our mice was not checked. The right fix is to cut the delta at the
   descriptor boundary.
5. **A board reboot in the middle of a run.** The records in RAM disappear. Our code will treat
   unconfirmed records as not accepted (reason 5) and return their deltas to the carry-over.
   If the board managed to emit them before the reboot, the movement will double. A rare case.
6. **Two programs with the Raw API running at once.** The id numbers are shared, reason 2 errors are possible:
   handled by shifting the id by +1000, but not specifically tested.
7. **Mixing with `0x18`.** Raw waits until the AUTO queue is empty. Do not send both
   kinds of commands interleaved.
8. **`--interp auto` was not run.** On the board interpolation is currently `0x1F = 0`
   (the factory value is AUTO = 255). It does not matter for Raw. Who changed it to 0, and when
   (and whether it is saved in the board's memory), is unknown.
9. **Firmware V4.086** (the official fix of smoothing for `km.move_now` and
   interpolation 0) was not installed or tested. It is not needed with Raw.
10. **The limit set by the mouse in the host port** (section 3.5): a 125 Hz mouse = a maximum of 125 Hz of injection.
11. **Hanging receipts.** The 19:19 snapshot showed free 13 of 16. A full scan of
    92,782 ids with the `0x6A consume=0` query found ids 64886–64888 in state 2:
    handed to USB, but not confirmed. This is the tail of an interrupted run. Most likely it was
    closing the console with the X: Windows kills the process right after the handler returns,
    and `finish()` has no time. The records were confirmed manually, free became 16 of 16.
    Fixes:
    - the close handler waits up to 2 s for the run to stop;
    - `prepareRaw` confirms the tail of the last 4096 ids and reduces the window;
    - `moveRaw` does not overwrite an occupied slot `id % 64`.

    Closing the console in the middle of a run after the fix was not checked on hardware.

## 11. What is outdated in other files

- **`README.md`**, the section "Measurement results" and the conclusion "What cannot be done from the PC side":
  wrong. The rate was set by the interpolator for `0x18`, Raw gives 1000 Hz.
  The table of modes after the touch applies only to the `0x18` path. The README needs to be
  rewritten from this file.
- **`motion_engine.h`**, the comment at the beginning: "the injection output rate is set by the
  firmware". This is true only for `0x18` / `km.move`.
- `legacy-python-exp/POLLING_RATE_AND_QUEUE_DEEP_DIVE.md` and
  `legacy-python-exp/STATE_AND_FREQUENCY_ANALYSIS.md` (until 2026-10-12 they lived in the root):
  hypotheses of past sessions, the Raw API is not mentioned there. The root `CONTEXT_AND_SETUP.md`
  was rewritten on 2026-10-12 and is current.

## 12. Sources

- `https://makcu.com/en/api/` — the command reference and changelog.
- `https://makcu.com/api/versions` — the opcode matrix by build and binary payloads.
- `https://makcu.com/api/raw-v4074.md` — the full Raw API contract (0x69/0x6A).
- `https://makcu.com/mcp` — the official MCP server (streamable HTTP, no authorization),
  tools `firmware_versions`, `get_command`, `search_docs`, `list_commands`.
  From `firmware_versions`: "V4.094 ... Otherwise the same as the official V4.085";
  "V4.086 ... This build is based on V4.085".
- app.makcu.com: the Toolkit JS bundle (`assets/ConfiguratorRuntime-*.js`, `assets/index-*.js`)
  — the semantics of `0x1F` (SET/GET, AUTO = 255), the tooltip "0% still interpolates",
  the `0x3F` transport test; the firmware catalog `/makcu-fw/firmware.json`.
- The mak-suite SDK (`mak-suite\protocol\*.md`, `cpp\makxd-cpp\include\makxd_protocol.h`):
  the Raw API and `0x1F` are not there.

> **Superseded 2026-11-16 by `MAKCU_DEV_REPORT_GAMEPAD_20261116.md` + `basics/comport_way/makcu_gamepad_com_test.zip`. Not sent.**

# Controller on USB3 stops reporting / disappears while the PC talks to MAKCU over COM (V4.126)

## Setup

- MAKCU, firmware **V4.126** (official). The same kind of dropouts were seen on V4.122 one day earlier.
- Controller on USB3: GameSir in XInput mode, wired, 1000 reports/s (`0x02 DEVICE` = `0x82`; the PC sees the clone as
  `VID_3537&PID_1098` / `PID_100F`, "XBOX 360 For Windows").
- PC link: USB2 COM port (CH343, 4 000 000 baud), binary MAK_API frames `DE AD | LEN | CMD | PAYLOAD`.
- Controller settings: smoothing Off (interpolation 0), buffer 8 ms, 1 ms controller polling on, one "Linear" profile,
  no deadzones. Nothing else is attached to USB3.
- Commands used: `GET 0x54 CONTROLLER_PHYSICAL`, `SET 0x40 CONTROLLER_STATE` (20 bytes), `GET 0x40` four times a
  second, `0x02 DEVICE` rarely. No text commands, no streams.

## What happens

Two failure modes, both only while the PC is sending commands:

1. **The controller disappears.** `0x02` goes from `0x82` to `0x00`, `0x54` answers `FF`, `km.device()` answers
   `R:-;M:0uf;K:0uf;C:0uf`, Windows loses the cloned XInput device. MAKCU keeps answering on COM.
2. **The controller stays enumerated but goes silent.** `0x02` is still `0x82`; `0x54` returns 32 bytes with a frozen
   `sequence` and `timing = 0x8000` (invalid); Windows still has the XInput device but receives no packets.

In both cases the controller does not come back by itself (we waited up to 85 s). Replugging only the controller cable
does not bring it back; replugging all MAKCU cables (a full restart of the board) does.

## Measurements on V4.126 (2026-11-15, one evening, board restarted after every failure)

| COM traffic | Frames/s | Result |
| --- | --- | --- |
| none (controller used through MAKCU for a game) | 0 | ~5 h 50 min, no dropout |
| `GET 0x54` only | 100 | 300 s, no dropout |
| `SET 0x40` (right stick moving in a circle) 125 / 250 / 500 per s + `GET 0x54` 100/s | 230 / 355 / 605 | 3 x 180 s in a row, no dropout |
| `SET 0x40` 500/s, 258 cycles "stick to (12000, -1) for 300 ms, hand-off, 400 ms pause" + `GET 0x54` 100/s | ~330 | 180 s, no dropout |
| `GET 0x54` only | 1000 | first run 61 s clean; second run: controller disappeared after 39 s |
| `SET 0x40` 1000/s + `GET 0x54` 100/s | 1100 | controller disappeared after 10 s |
| `GET 0x54` 2000/s + `SET 0x40` 1000/s | 3000 | first run ~20 s clean; second run: disappeared after ~7 s |
| `SET 0x40` 500/s (same circle, stick never released) with a right-trigger press every 250 ms (held 80 ms) + `GET 0x54` 100/s | ~605 | controller went silent after 144 s (576 presses) |
| `SET 0x40` 500/s with trigger/button presses and stick releases (all-zero `SET 0x40`) + `GET 0x54` 100/s | ~360 | controller went silent after 51 s |
| several short runs: write smoothing (`0x3E` record `0x1D` BEGIN / WRITE / APPLY, no SAVE), stick 500/s, release | ~600 | controller went silent within ~15 s of traffic |

Two more observations:

- With `SET 0x40` at 1000/s the `sequence` field of `0x54` advances only ~495 times per second instead of ~1000; at
  125-500 commands/s it advances ~935-975 times per second. It looks as if every command costs the USB3 side one
  controller poll.
- Without the controller attached, V4.126 answers `FF` to every `0x54`; V4.122 often did not answer at all.

## What we cannot rule out: the controller itself

All of this was measured with **one controller** (GameSir); we have no second one to compare. This model has known
quirks of its own: on its 2.4G dongle it falls asleep and reconnects by itself, and other owners report the same. So
the fault may be in the controller (for example, it stops answering when the host polls it irregularly) and not in
MAKCU. What points at the MAKCU side or at the pair, not at the controller alone: plugged through MAKCU with no COM
traffic it ran ~5 h 50 min without a single dropout, the dropouts start within seconds to a minute once COM traffic
reaches about 1000 frames/s, and replugging only the controller cable does not recover it. If you have a GameSir or
can tell us which controllers you test with at 1000 commands/s, that would settle it.

## Shortest reproduction

Open the COM port at 4 000 000 baud and send `DE AD 01 00 54` once per millisecond, reading the replies. No injection is
needed. With our controller it disappeared after 39 s in one run out of two.

## Questions

1. Is there a supported maximum command rate for controller commands (`0x40`, `0x54`) over COM? Our texture work needs
   `SET 0x40` once per millisecond with smoothing Off.
2. Does V4.126 include the USB3 auto-reconnect from the V4.123-V4.125 test builds? We never saw the controller come
   back by itself.
3. Is there a command that restarts the USB3 side (or the whole board) without replugging cables?
4. Would the `0x73` / `0x74` streams load the USB3 side less than polling `0x54`?
5. At about 600 frames/s the stick alone was clean for 360 s (a 180 s circle and 180 s of 258 enter/release cycles),
   but both runs that added trigger/button presses to the same `SET 0x40` frames went silent (after 51 s and 144 s),
   and so did the short runs with settings writes (`0x1D` APPLY) and no presses. The sample is small, so this is a
   suspicion, not a result. Can a change of buttons/triggers in `SET 0x40`, or APPLY, disturb the USB3 side?

We can run any test build or diagnostic you suggest and send the support report.

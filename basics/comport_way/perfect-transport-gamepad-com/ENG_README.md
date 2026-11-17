# perfect-transport-gamepad-com (short English copy of README.md)

State 2026-11-17. Transport revision 3, renderer revision 4. MAKCU firmware 4133 (custom dev build; before it official 4126).
One test controller: GameSir in XInput mode (`kinds 0x82`), cable and later its 2.4 GHz receiver in the board's controller port.
The Russian README.md is authoritative; full history in `HANDOFF_GAMEPAD_COM.md` (repo root).

## Key facts

- **COM load drops the controller (USB3 failure).** Seen at 2000 `GET 0x54`/s (4126: 5/5 within 4-108 s, dead until the
  board is replugged; 4133: 1 of 3 x 300 s, recovers by itself in 1.4-2 s), at 500 reads + ~325 commands/s (4133: at
  987 s) and at 100 reads + 1000 commands/s (4126: 9.8 s; 4133: 130 s). MAKCU (2026-11-17): cause found, fix in testing.
- Best texture: board smoothing Off + one `SET 0x40` per 1 ms + `presetOff()`: W1 sum 3.49 vs live hand (smooth curve
  15.70). But 1000 commands/s is exactly what drops the controller now; working mode until the fix: Auto, 500 commands/s.
- Read the COM port only without waiting (no read timeouts): the WCH CH343 driver bluescreens; COM5 now runs on the inbox
  `usbser.sys` (8 h clean).

## Transport (`makcu::GamepadRawTransport`)

- Right stick: `setRightStick(rx, ry)` (board Y points DOWN; Windows `sThumbRY = -ry - 1`), `releaseRightStick(waitMs)`
  (zeros = handoff to the physical stick; `(0, 0)` is "release", centre is `(0, -1)`), `waitCommand()`.
- Buttons / triggers: `setButton`, `setTrigger` (0..1023, Windows shows /4), `tap(id, holdMs)`, `releaseButtons`. The program
  can press but cannot un-press a button held by the hand.
- Periods (rev 3): command period = 2000 us (Auto/Fixed) or 1000 us (Off), never shorter than the controller's declared
  interval (`padCadenceUs()`, `cadence` of `0x54`); `GET 0x54` poll = max(2000 us, cadence) by default (was 500 us).
  `padReportRate()` = measured reports/s. The board's clone declares the real controller's interval to Windows, so it
  cannot emit more than the controller rate - except when a controller sends less than it declares (GameSir with
  GameSir Connect open: ~225/s at 1 ms cadence -> 225 + 500 commands = ~722 packets/s in Windows).
- Blend in/out at Off: `setBlendMs(16, 24)` by default (smoothstep from the physical stick).
- Board smoothing by code: `setSmoothing(0|1|2)` via documented `0x3E/0x1D` BEGIN/WRITE/APPLY, no SAVE (no NOR write);
  `close()` restores the previous value.
- Physical stream instead of polling (NOT yet run on the board): `setPhysicalSource(PhysicalSource::Stream, periodMs = 1)`
  queries `0x73`/`0x74` (empty body -> `mode u8 + period u16 LE`), sets `[2, period]`, verifies; `FF` = not in this firmware,
  stays on polling. `0x75` events (kind 1 axes id 10-15, kind 2 buttons; id 255 = snapshot unavailable) update `physical()`;
  `GET 0x54` drops to every 250 ms; state re-checked every 1 s; `close()` turns the streams off. `example_pad --stream [MS]`.
- One worker thread, 500 us tick, synchronous port, non-blocking reads; `GET 0x40` verify every 250 ms; reconnect on
  I/O error or 750 ms silence.

## Renderer (`makcu::PadTextureRenderer`)

- Use `presetForTransport(kind, boardOff, tx.commandPeriodUs(), tx.padCadenceUs())`: picks `presetOff()`/`presetFor()` and
  fits the tick and the ADC model to the command period (bit-identical to `presetOff()` at 1 ms / 1000 Hz / Off).
- `presetOff()`: GameSir ADC model (~101 units per raw LSB, radial response curve, sample every 2 ms with midpoint report,
  "no-midpoint" bursts, hysteresis 0.75 LSB). Adds 2 ms lag; target lead is done by the caller.
- Board measurements (V4.122, Off): renderer W1 3.49, lag 7.5 ms, tracking error 80 / 226; replaying the recorded hand 1.23.

## Board behaviour (measured)

- Auto: takes the target every 2 ms, `y = 0.5u + 0.5y_prev`, 6-7 ms lag; Off: pure 4-5 ms delay, every 1 ms command passes.
- Silence watchdog 1 s (target dropped instantly, `GET 0x40` still shows it); if the target does not change for 2 s the
  board yields to a moving hand. Both measured at Auto only.
- While commands flow the board receives fewer controller reports (~500/s at 2 ms commands; 495-660/s at 1 ms).

## Not done / not verified

`0x73`/`0x74` physical stream on the board (code exists, live test after the next firmware), transport behaviour during the 4133 1.4-2 s drop, watchdog and
hand-yield at Off, real 125/250/500 Hz controllers, other controller models, `pad_controls_test`.

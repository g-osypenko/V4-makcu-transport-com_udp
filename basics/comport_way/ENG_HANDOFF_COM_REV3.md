# Handoff: port revision 3 to the COM reference (2026-10-12)

*English translation of [`HANDOFF_COM_REV3.md`](HANDOFF_COM_REV3.md) (Russian original).*

Task for the next chat: bring `perfect-transport-comport/makcu_raw_transport.h` (COM, Raw `0x69/0x6A`)
up to the level of the UDP reference revision 3. The model is `basics/udp_way/perfect-transport-udp/` (code, README,
`controls_test.cpp`). For a general project overview see the root [`ENG_CONTEXT_AND_SETUP.md`](../../ENG_CONTEXT_AND_SETUP.md) (read it first).

## What already exists

- **UDP reference, revision 3** (`makcu_udp_batch_transport.h`): 1 kHz movement (`0x6C`), button reading
  (`0x53` events + `0x55` safety poll), **button injection** `press/release` (`SET 0x11…0x15`),
  **locks** `setLock/unlockAll` (`SET 0x60`, 14 targets), verification by the `GET 0x56` snapshot,
  **auto-reconnect**, state reset in `open()`/`close()`. Verified on hardware: clicks 25/25,
  ~1.5 ms to Windows; locks (the transport sees, Windows does not); reconnect after the cable was unplugged.
- **COM reference, revision 2**: 1 kHz movement (`0x69/0x6A`) + button reading (`0x53` events over COM,
  `0x55` on open and after an overflow). **No** injection, locks or reconnect.
- UDP test program `controls_test.exe`: `--clicks`, `--draw S`, `--watch S`, `--locks S`,
  `--reconnect S`, `--soak M`; its own window with `ClipCursor`, draws the trail and points, status on top, Esc exits.

## Owner's decisions (do not ask again)

- Clicks **as fast as possible**, with no ordering against movement. No timing and no "humanization": the hold time is set
  by the caller.
- **The wheel is not needed** at all (on 4097 the physical wheel cannot be read anyway).
- **Auto-reconnect + on `close()` release all buttons and clear all locks.**
- Long run — 30 minutes.
- Follow the UDP model: the same API (`press/release/setButtonState/releaseAllButtons`, `injectedButtons*`,
  `setLock/unlockAll/locked/locks/locksOnBoard`, `connected()`), a shared `LockTarget` block guarded by
  `MAKCU_LOCK_TARGET_DEFINED` (both headers must build in one program).

## What to port and how COM differs from UDP (verify by measurement, do not port blindly)

1. **Injection and locks.** `SET 0x11…0x15 state`, `SET 0x60 target state` — no reply. Over UDP a pause of ≥ 1–1.5 ms
   is needed after such a command (otherwise the next one is lost). **Over COM this limit most likely does not
   exist** (COM holds > 2000 frames/s, a ring of 32 WriteFile entries) — measure it: SET + an immediate
   GET, no pause. COM frames leave from the `move()` thread through the `sendFrame()` ring → `press()` must not send directly from any
   thread: keep the "requested" state in atomics and send it from `move()/service()`
   (as `resyncButtonsIfNeeded()` does), or add a lock on the ring.
2. **Verification with the `GET 0x56` snapshot** (40 bytes: `[0]` injected buttons, `[1..2]` locks u16) — parse the reply
   in `parseRaw()` (opcode `0x56`, length 40). Normal form of the locks as in UDP: two bits per axis
   (+/−), the whole-axis bit is only computed (`normalizeLocks`, `applyLock`).
3. **Reconnect over COM** is a different kind of drop: USB2 unplugged / board reboot → `WriteFile`
   / `ReadFile` fail and the COM port disappears. Needed: notice it (an I/O error or silence during the
   heartbeat), close the handle, wait for the port to reappear (`findMakcuPorts()`), reopen it (baud probe
   4M/1M/115200), redo `prepareRaw()` and the event stream, restore locks/buttons from the snapshot.
   Right now the COM transport sets `failed_` forever on an error.
4. **`open()`/`close()`**: clear leftovers of a crashed program on open; on close release and
   unlock (confirmed with a snapshot), and restore the `0x52` stream.
5. Test: make a COM variant of `controls_test` (or a `--com` mode in the shared one) with the same modes.

## Facts, pitfalls

- COM: CH343 `1A86:55D3`, **COM5, 4,000,000 baud** for us; the port is exclusive. USB2 may currently be
  unplugged — ask the owner to plug it in before testing. `MAKCU Virtual COM Port (COM8)` is the Toolkit
  channel, not ours.
- `GET 0x11…0x15` return **only the injected state**, not the physical one. Physical state is `0x55`/`0x53`.
- Lock: the game does not see it, the transport does (events describe the physical state before the lock).
- Injected buttons and locks are **not** released by the firmware itself.
- Firmware **V4.097 has been official** since 2026-10-11. Test builds V4.098 (the USB3 side restarts itself
  after a hang) and V4.099 (+ the hold-back of injection with wireless receivers after 9–18 s of idle).
  In the 30-minute UDP run the board's USB3 "went silent" in the 3rd minute (mouse and network died, only
  reconnecting helped) — the symptom described for 4098. The transport is not to blame; if it recurs, ask for 4098/4099.
- The tests move the cursor and click — only inside their own window with `ClipCursor`; the owner does not
  touch the mouse during a run; give them Esc to exit. We do not reverse-engineer the firmware.
- Build: MinGW g++ 15.3 (`C:\mingw64\bin`), the references are C++17, `-Wall -Wextra` with no warnings.

## How to start

1. Read `ENG_CONTEXT_AND_SETUP.md`, this file, `perfect-transport-udp/ENG_README.md` (the sections "Button injection
   and locks" and "Reconnect") and the code of `perfect-transport-comport/makcu_raw_transport.h`.
2. Ask for USB2 (COM5) to be plugged in, and check `perfect-transport-comport/example_line.exe --check`.
3. Measure with probes: whether a pause is needed after `SET` over COM; the format of the `0x56` reply over COM.
4. Implement COM revision 3, then the tests (`--clicks`, `--locks`, reconnect — unplug USB2,
   long run), update the README and `CONTEXT_AND_SETUP.md`, commit.

# Instructions for an LLM: the MAKCU project

*Short English summary of [`CLAUDE.md`](CLAUDE.md), which is the full version (commands, test table, how to read results). As of 2026-11-12.*

Two single-header C++ transports for the **MAKCU** board: COM (`makcu_raw_transport.h`) and UDP (`makcu_udp_batch_transport.h`), same API.
Bench: `basics/track_bench.h` with `track_test_com.cpp` and `track_test.cpp`.
The UDP address differs per user (DHCP): `example_line`, `controls_test`, `track_test` default to `--host auto` (`makcu_udp_discover.h`, `find_makcu.exe`).
Firewall rule for button events: `setup_firewall.bat`, not yet run with administrator rights on this PC.

**Owner's rules:** report in Russian; warn before every live test what the hand must do; the test window text must match the chat, because the
owner follows the window; cursor-moving tests only in their own clipped window; do not reverse the firmware and do not change it without the owner's decision
(the board answered V4.122 on 2026-11-12; it was V4.097 until 2026-10-31 and every measurement in the docs was taken on 4097); Raw Input and hooks only in tests; document in detail for an LLM; never run COM and UDP tests at the same time.

**Done:** revision 5 on both channels, all on `master` and GitHub.

**Open:**
1. UDP does not verify `km.screen` (`SET 0x64` has no reply).
2. Report holes on COM: 31.8–32.4 or 97.6–112.8 ms vanish while the board reports everything as sent; same rate on revisions 3 and 5; cause unknown.
3. Tail with a hand moving the mouse: COM 4.5–5.4 ms, UDP 6–9 ms; likely the owner's "3–4 frames" on UDP.
4. USB3 hang on 4097: during long UDP runs the mouse and network die until the board is replugged. Warn before long UDP runs. With tracking on in the running program it takes minutes over UDP and about 10 minutes over COM: `HANDOFF_UDP_TRACKING_HANG.md`, the next task.
   **Cause found 2026-11-13 (V4.122, UDP): command rate near the board limit (~530/s).** A bare probe (`udp_soak.exe`) with any opcode hangs it
   at 510/s in 11-190 s; 350/s hung once at 217 s; 250/s ran 30 min clean. The UDP reference sends packets and polls back-to-back, i.e. at the
   limit. Fix done and verified 2026-11-13 (three 30-min stress runs without a hang, tail 5.88 ms vs 3.95): UDP revision 6, minimum command period 4 ms
   (`setCommandPeriodUs`, `effectiveMaxLag` = max(setMaxLag, period + 3) = 7). COM: bare polling at ~5800/s ran 10 min clean; the CH343
   blue screen was reproduced by the old driver 2.1.2025.7 and did not recur in 30 min on driver 2.2.2026.5.
5. Not run: COM `--soak 30`, UDP revision 5 `--reconnect` with tracking and `--soak`; G HUB as a cause of the holes.
6. Blue screen `0xD1` in the CH343 driver on 2026-10-10 and 2026-10-30, same instruction, not a power loss; dumps not analysed: `HANDOFF_UDP_TRACKING_HANG.md`.

**Done 2026-11-12: `setSensitivity(percent)`** in both references (COM windows from 2 ms, UDP from 6 ms): share of physical mouse movement, 0–100 %, like a
lower DPI; any thread, changeable on the fly; injection is not scaled. The watchdog toggles the `0x16` mask in whole-ms windows of at least
2 ms. Measured: 75 / 50 / 33 / 25 / 10 % requested gave 75.3 / 49.7 / 33.2 / 24.9 / 9.9 %; injection under 50 % is unchanged (records =
messages, tail 1.15 ms). UDP: 75.6 / 49.8 / 33.6 / 24.4 / 10.0 %, injection under 50 % gets coarser (25 % merged against 6–7 %). The board hung twice over UDP
on 2026-11-12 on V4.122 after ~4–6 s of 1 kHz injection, with and without the mask: warn before any UDP injection run. Not checked: long runs, `controls_test`
on the new build, how the bursts look in a game. Details: COM reference README.

**How it was reached: partial pass-through of the physical mouse** (part of the hand movement reaches Windows, like aim slowdown on
gamepads). The API up to V4.122 has no scale for physical movement and no mouse motion stream; `MOVE_MASK 0x16` is pass or drop per direction.
"Lock and re-inject a share" is impossible: the board does not count physical movement under a mask. `0x34 MOUSE_HISTORY` and
`0x35 RAW_PASSTHROUGH` are listed but their format is undocumented; not probed. Approach A: toggle the mask every few ms (bench mode `--pwm`):
works over UDP on 4122, both axes, 75 / 50 / 25 % requested gave 75.0 / 49.9 / 25.0 % at an 8 ms period; feel in a game, toggling together with
injection and COM not checked. Details: `HANDOFF_MOUSE_TRACKING.md`. Approach B: counter-injection from `0x63` polling (not tried; the injection tail with a hand and the polling hang
argue against it). Approach C: ask MAKCU for a 0–100 % mask or a physical motion stream (not sent).

**`km.screen` 32767×32767** is not a monitor setting: it is the bounds of the board's position counter, which Windows and games never see. At
1920×1080 the counter clamps 960/540 counts from the centre and the count is lost; 32767 is the field maximum and gives ±16383 counts.
`setTracking()` sets and restores it by itself.

**Refuted, do not retest:** holes caused by separate `0x6A` writes (revision 3 shows the same holes); COM tail reaching 12 ms quickly; `0x6A` showing the queue.
A cause cannot be claimed from a handful of rare events: run the old build in the same scenario.

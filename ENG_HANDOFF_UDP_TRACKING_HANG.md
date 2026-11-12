# Handoff: the board dies with tracking on, within minutes over UDP, over COM too (2026-10-30)

**Update 21:05: it also died over COM,** about 9 min 40 s after connecting (session 20:52:58, 10 min 34 s, last button event 21:02:46). COM is not a workaround.
That weakens the network theory: COM polls about twice as often as UDP (~970/s) yet lasted longer. New candidate: commands at stream and shot boundaries mixed with polling.

**Update 21:30: blue screen in the CH343 driver,** 18 min 38 s into a COM session: `0xD1`, read at `0x38`, IRQL 2. The same crash, same page offset `0x191`, happened on 2026-10-10,
before tracking existed, so the bug is old; tracking may only make it likelier. Not a power loss. Dumps need admin, the module is unconfirmed: details in the Russian file.

*Short English summary of [`HANDOFF_UDP_TRACKING_HANG.md`](HANDOFF_UDP_TRACKING_HANG.md), which has the tables, test plan and acceptance criteria.*

**Symptom.** Inside the running program (`verse3_oh`, branch `hand-tracking`, `setTracking(true)` right after `open()`) the board loses USB3:
mouse and network die together, only replugging helps. Seen 2026-10-30, board V4.097, revision 5, UDP: after ~4 min 40 s, again ≤ 24 s after a
replug, and after ~2–3 min in a run with almost no aiming. COM: see the update above; what exactly was lost over COM is not recorded.

**Prime suspect, unproven.** Tracking polls `0x63` all the time: ~530 commands/s against ~24/s when idle before. Earlier hangs came within minutes of
continuous UDP traffic. All UDP traffic crosses the USB NIC on the board's USB3 port, the hub the mouse receiver is on. Tracking over UDP was never
run longer than 20 s on the bench. Other candidates: `0x63` together with physical reports, `km.screen` 32767 or recentring, G HUB.

**Not the cause.** Spacedesk (service stopped during the last hang); program logic (no new commands but the poll); `Qt5Core.dll` faults at exit.

**Plan.** Add a `--tracksoak` bench mode that flushes a line every 10 s, then run the matrix in the Russian file before changing the code. Warn the owner
before each run: the mouse may die. Main fix candidate: two poll rates, 20–50 Hz idle and ~500 Hz while the activation button is held. The program's simulator
shows 250–500 Hz is enough and 100 Hz is no better than no tracking in the worst case, but only around a stream's start.

**Until fixed:** set `mouse_tracking` false on either channel. Run the matrix on both channels, COM first, 30+ minutes each.

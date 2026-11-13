# Handoff: the board dies with tracking on, within minutes over UDP, over COM too (2026-10-30)

**Update 21:05: it also died over COM,** about 9 min 40 s after connecting (session 20:52:58, 10 min 34 s, last button event 21:02:46). COM is not a workaround.
That weakens the network theory: COM polls about twice as often as UDP (~970/s) yet lasted longer. New candidate: commands at stream and shot boundaries mixed with polling.

**Update 21:30: blue screen in the CH343 driver,** 18 min 38 s into a COM session: `0xD1`, read at `0x38`, IRQL 2. The same crash, same page offset `0x191`, happened on 2026-10-10,
before tracking existed, so the bug is old; tracking may only make it likelier. Not a power loss. Dumps need admin, the module is unconfirmed: details in the Russian file.

**Update 2026-11-13, V4.122, UDP: the cause is the command rate.** The reference sends the next command right after the reply: `0x6C`
packets while injecting (~500/s, 2 records each) and `0x63` polls while tracking (~510/s). That is the board's limit (~530/s). Pure injection
(no mask, no tracking) hung the board at 33 s, tracking alone (zero injected records) at 99 s. A bare probe without the reference,
`basics/udp_way/probes/udp_soak.exe`: 510/s and back-to-back hung it at 11-190 s with any opcode (`0x02` too); 350/s once clean for 10 min,
once hung at 217 s; **250/s ran 30 min with zero lost replies** (100/s 10 min clean). 1-3 s of sporadic lost replies precede every hang.
The mask `0x16` hypothesis is dropped. V4.122 never recovered by itself; the owner replugged every time. Fix (not done): a global minimum
command period in the UDP reference, candidate 4 ms (250/s). Injection stays 1 kHz with ~4 records per packet; cost about +2-3 ms of tail
(estimate). Not checked: `0x6C` at 250/s, longer than 30 min, COM. New tools: `track_test --stress S --load inject,sens,track,busy`.

**COM, 2026-11-13.** Bare synchronous polling (`basics/comport_way/com_soak.exe --rate 0`, 10 min, 3.48 M requests, ~5800/s) ran clean:
the command path itself copes, so the UDP hang lies in the network path (USB NIC on USB3, inter-chip hand-off). Then the game-like
reference stress (`track_test_com --stress 1800 --load inject,track,busy --burst 200,300`) ran clean for 610 s and the PC blue-screened:
**`0xD1 (0x38, 2, 0, …6191)`, the same instruction as 10.10 and 30.10, no power blink**, dump `111326-9218-01.dmp`, driver CH343S64.SYS 2.1.2025.7.
Hypothesis (unproven): a race in the CH343 driver under the reference's overlapped I/O (up to 32 writes in flight plus a pending read with
a 50 ms timeout); the bare probe does one synchronous op at a time. No debugger on the PC, the dump needs admin.
The owner then installed **CH343 driver 2.2.2026.5**: the same stress ran **30 min with no blue screen**, 0 link losses, 719 098 records all
delivered. Then a 60 min run (16:59-17:59): no blue screen, 0 link losses, 1 438 041 records all delivered. **Conclusion: the `0xD1` blue
screen came from CH343 driver 2.1.2025.7; it does not recur on 2.2.2026.5** (90 min of the load that crashed the old driver at 610 s).

**Fix verified 2026-11-13 evening (V4.122, UDP): reference revision 6** - minimum command period 4 ms, effective depth cap 7, and while
injecting the 0x55 poll every 250 ms and the 0x56 heartbeat every 1 s. `--stress 1800` with injection (old: hang at 33 s), tracking (old: 99 s)
and game-like load: **30 min each, 0 link losses, 0 lost receipts**. Injection tail without a hand 5.88 ms (was 3.95), ~980 reports/s, ~2 %
merged. Not yet: hand on the mouse, `--track 20`, `--trackinject 8`, longer than 30 min, the game.

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

Freeze report as you asked. Report taken during the freeze: **R-GE56ZQ** (note: "freeze state"). Device MK-R0F2-WZ5N.

Setup: custom 4133, smoothing Auto / 8 ms, GameSir dongle directly in MAKCU USB3 (no hub), Toolkit open. Our test went through Toolkit's virtual COM7, so the command profile has it. Load: right-stick circle 200 commands/s, evenly every 5 ms, plus GET 0x54 250/s. Nothing else, no buttons.

Timeline (local time):
- 19:19:08 start, clean.
- 19:23:13 controller silent ~2 s, one more report, then from 19:23:15 sequence stopped (timing stayed 0x0008). LEDs solid white, same as before. About 4 min in, sooner than the 8-9 min last time.
- 19:23:23 our program closed the port and exited without sending anything (no release packet). From 19:23:24 Windows got no packets from the controller.
- R-GE56ZQ taken in that state. Controller and board not touched.
- Shortly after that report, before the second one, the controller came back on its own. No dongle replug, no controller power cycle, nothing touched. This is the first freeze in 8 that recovered without a power cycle. So steps 2 and 3 were not done.

Question: does creating a report touch USB3 or the controller (re-enumerate, reset, request descriptors)? If yes, the report itself may have woken it.

Ignore R-Q44ZFW (19:09): our test was holding COM7, Toolkit skipped the device snapshot ("lease busy"). It was taken during a burst of short self-recovering drops (silent ~2 s, back in ~1 s), not during a freeze.

<!-- owner: mouse -->

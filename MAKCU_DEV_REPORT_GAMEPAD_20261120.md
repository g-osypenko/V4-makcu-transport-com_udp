# 4133: injection (SET 0x40) is the trigger - report to the MAKCU developers (2026-11-20, DRAFT)

Draft for the owner. Reply in the Discord thread of R-DDEJ38 / the assigned build. Two messages, each under 2000 characters.
Attach `basics/comport_way/makcu_4133_inject_logs_20261120.zip` (logs of every run below, README.txt inside).
On 2026-11-18 MAKCU asked not to send more until the new build is out - sending is the owner's call.

---

## Message 1

Still on 4133 (no new build yet). We isolated what triggers the failures on our side: it is controller injection, not `0x54` polling.

Setup as R-DDEJ38: GameSir on its 2.4 GHz receiver in USB3, PC on USB2 COM (4 Mbaud), Auto smoothing, controller not charging. Injection = right stick circle via `SET 0x40`, nothing else.

```
0x54 poll   SET 0x40          ran     result
250/s       none              600 s   clean
250/s       250/s             110 s   board drop at 23 s, then 5 more (storm)
250/s       125/s (fresh)     393 s   controller FREEZE at 271 s
250/s       250/s in bursts   600 s   controller FREEZE at 140 s;
            (0.6 s on/1.4 off)        after power-cycling it, 390 s clean
500/s       250/s             276 s   controller FREEZE at 154 s
1000/s      250/s (fresh)     285 s   controller FREEZE at 163 s
1000/s      250/s             373 s   controller FREEZE at 251 s
```

Two different failures, both only while injecting:

1. Board drop (as before): `0x02` -> `0x00`, `0x54` -> `FF`, Windows loses the controller, back by itself in 1.6-2.4 s. After the first one they come every 5-30 s.

2. Controller freeze (new today, 5 of 5 injection runs, 0 without injection): `0x02` = `0x82`, `km.device()` normal, Windows still has `VID_3537&PID_1098`, `0x54` answers 32 bytes but `sequence` stops and `timing` = `0x8000` (INVALID). Controller and receiver LEDs solid white. Moving the sticks does nothing. Waiting 2+ min does not help. Only power-cycling the controller fixes it; board and receiver untouched. During the freeze Windows keeps getting the injected packets, so only the physical input is gone.

---

## Message 2

Also: while `SET 0x40` is active the board gets only ~500 reports/s from the controller (1000/s without injection), even at 125-250 commands/s.

Questions:

1. Does the board send anything to the controller while injection is active (output reports, rumble, LED, re-poll)? Something on that path seems to freeze the GameSir and to halve its report rate.
2. Does the fix you mentioned on 17.11 ("cause found, in testing") cover drops triggered by injection?
3. Until then, is there an injection pattern you consider safe over COM (rate, keep-alive, release between bursts)?

Our code now pauses commands while the controller is gone and detects the freeze, but continuous use is not possible on 4133. We keep 4133 installed and can run any build or capture.

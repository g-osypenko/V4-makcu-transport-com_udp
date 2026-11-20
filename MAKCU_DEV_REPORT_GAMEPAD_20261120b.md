# Reply to MAKCU answer of 2026-11-20 14:57: freeze still happens at 200/s and 100/s (DRAFT)

One message, under 2000 characters. Reply in the same Discord thread. Optional attachment:
`basics/comport_way/makcu_4133_inject_logs_20261120b.zip` (the two runs below).

---

Thanks, that helps. We changed our code to your pattern and ran it, as you asked.

Changes: state sent evenly every 5 ms (200/s), an unchanged state is resent only every 250 ms, held buttons every 250 ms, release = one all-zero state (sent again only if a `GET 0x40` check shows it did not apply). Same setup as before (4133, GameSir on its receiver in USB3, COM, Auto, `0x54` polled 250/s).

```
SET 0x40, every value new (stick circle)   result
200/s (5 ms)                                controller FREEZE at 488 s
100/s (10 ms)                               controller FREEZE at 545 s
earlier today, 250/s (4 ms)                 FREEZE at 140-271 s (5 runs)
0x54 250/s only, no injection               600 s clean
```

No board drops (`0x02` stayed `0x82`) in the 200/s and 100/s runs. The freeze looks the same every time: `sequence` stops, `timing` = `0x8000`, LEDs solid white, only a controller power cycle helps. So at your safe pattern it is rarer (8-9 min instead of 2-4 min) but not gone: 7 of 7 injection runs today ended with a freeze, 0 of 1 without injection.

If it helps your side: we can run any test build, or keep a run going until the freeze and then take a Toolkit report without touching anything. We have only this one controller, so we cannot rule out the GameSir itself.

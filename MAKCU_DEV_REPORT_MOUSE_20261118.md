# Custom builds 4133/4134: report to the MAKCU developers (2026-11-18)

**NOT SENT.** Before this draft was written, the owner had already sent their own report (mouse R-codes R-HG49T6 and R-8E0JNX,
R-Q50J27, a G HUB screenshot), and MAKCU answered: 4134 is not a fix, the firmware watchdog tears down and rebuilds the link
between the board's two chips (mouse and controller alike), so keep the mouse in the PC and send nothing more. See
`HANDOFF_GAMEPAD_COM.md`, «Персональная сборка 4134». Kept only as a record of our own measurements.

Reply in the Discord thread where they sent the 4134 build (bot message 2026-11-18 05:16). Two messages, each under 2000
characters. Attach `basics/comport_way/makcu_4134_logs_20261118.zip` (README.txt inside).
Main news: since the custom build, the **mouse** also drops on USB3, with almost no COM traffic. The owner noticed it on
2026-11-17, the day 4133 was installed; before that, the same mouse on the same board was fine. Receiver directly in the PC: no drops.
If a Toolkit report is taken during a mouse drop (`mouse_drop_watch.exe --port COM7` with Toolkit open), put its code in place of
`R-XXXXXX`; otherwise delete that line.

---

## Message 1

Installed 4134. Results with the same GameSir 2.4 GHz receiver in USB3 (Auto smoothing, cables not touched):

```
repro_poll54, GET 0x54                     ran       result
2000/s on COM5                             4.5 s     drop, back in ~1.4 s
via Toolkit virtual COM7 (~1300/s)         18.8 s    drop, back in ~1.3 s
```

On 4133 the first two 300 s runs at 2000/s were clean. So the drop is still there on 4134, and in these two runs it came sooner. Same precursor: `sequence` +800-900/s instead of 1000 just before the drop.
Report taken while Toolkit was connected during the second drop: R-Q50J27 (the test went through Toolkit's COM7, so Toolkit was connected the whole time).
pad_soak not run yet. We stopped because of the problem in message 2.

---

## Message 2

New problem since the custom build: the **mouse** drops too. Logitech G502 X LIGHTSPEED, receiver `046D:C547` in MAKCU USB3. It started yesterday (the day 4133 went in): stutters, then the mouse disconnects and reconnects again and again. Re-plugging the receiver into MAKCU doesn't help. With the receiver plugged straight into the PC, it stops.

We measured it on 4134 with no injection and only `GET 0x02` twice a second on COM5. Normal desktop use, 10 min:
- 4 drops in 30 s (13:32:43, :52, 13:33:02, :10). Each time the board itself loses the mouse: `0x02` = `0x00`, `km.device()` = `R:-;M:0uf;K:0uf;C:0uf`, at the same moment that Windows removes it. Back after ~1.2-1.7 s with `0x03`, `R:MK;M:8uf;K:8uf`.
- Precursor: gaps of exactly 100-102 ms in the mouse reports while the mouse is moving, 20 of them in the 40 s before the drops.

It looks like the same USB3 loss and self-restart as with the controller, but it now hits a plain mouse with no COM load. That makes the board unusable for normal mouse use.
R-XXXXXX
Questions:
1. Could the USB3 recovery in 4133/4134 fire on a healthy device (for example, on a 100 ms report gap)?
2. Should we roll back to official until the next build, or keep 4134 and collect more data?

Logs: `makcu_4134_logs_20261118.zip`.

# V4.126 controller report to the MAKCU engineers (2026-11-16)

Two Discord messages (each under 2000 characters). Attach `basics/comport_way/makcu_gamepad_com_test.zip`
(README.md inside has every table, the logs and the sources). Replaces the unsent draft `MAKCU_DEV_REPORT_GAMEPAD_20261115.md`.

---

## Message 1

V4.126: the controller on USB3 disappears when we poll `GET 0x54` over COM fast, and after that USB3 never enumerates anything until we unplug all MAKCU cables. Archive attached (`makcu_gamepad_com_test.zip`): single-file repro, our soak tool, all logs, README.

Setup: GameSir controller (XInput, `0x02` = `0x82`, 1000 reports/s) alone on USB3, PC on USB2 COM (CH343, 4 Mbaud). Tested both on the controller's USB cable and on its 2.4 GHz receiver.

Shortest repro (`repro/repro_poll54.cpp`, one file, Win32): send `DE AD 00 00 54` 2000 times/s, one request in flight, no injection.

```
GET 0x54 rate   smoothing   result
2000/s          Off         lost at 21.0 s, 4.3 s
2000/s          Auto        lost at 108.4 s, 75.1 s (repro)
1000/s          Off         lost at 38.8 s (cable), 30 s clean (receiver)
100-500/s       Off         never failed
no COM traffic  Off         8-10 min clean (receiver), ~5 h 50 min (cable)
```

Right before the loss, `0x54` starts returning FF, and the clone disappears from Windows. After the loss:
- COM still answers (~6500 req/s), `0x02` = `0x00`, `km.device()` = `R:-;M:0uf;K:0uf;C:0uf`, `0x54` = FF;
- unplugging and re-plugging the receiver does not help (2/2): its LED is on, it never enumerates;
- only a full MAKCU restart (all cables) brings it back.
In a healthy state the same receiver re-enumerates after unplugging (3/3, 5-24 s).

---

## Message 2

More observations (all in the README with logs):

- `SET 0x40` once per ms halves the controller rate MAKCU sees: `0x54` `sequence` advances ~500/s instead of ~1000/s (cable and receiver). At 500 commands/s it stays ~1000/s.
- Light load is fine: `GET 0x54` 100/s + `SET 0x40` 500/s, stick circle + trigger taps + RT/A holds, 9 min clean on the receiver.
- On the cable we also saw a second mode: controller stays enumerated (`0x02` = `0x82`), `0x54` returns a frozen `sequence` with `timing` = `0x8000`, Windows gets no packets. Also needed a full restart.
- V4.122 + Auto (2026-11-14, cable): `0x54` at ~3500/s for 90 s was clean. We cannot go back to compare.
- Only one controller model tested, we have no second one.

Questions:
1. What request rate for `0x54` / `0x40` over COM is supported? We need `SET 0x40` every 1 ms with smoothing Off and similar reading.
2. Why does USB3 stop enumerating after the failure, and is there a command to restart USB3 (or the board) without unplugging?
3. Does each `SET 0x40` cost one controller poll on USB3?
4. Would the `0x73` / `0x74` streams load USB3 less than polling `0x54`?
5. Is there a debug build or log we can capture at the moment of failure?

We can run any build or diagnostic you send.

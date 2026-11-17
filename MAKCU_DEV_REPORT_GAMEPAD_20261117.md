# Custom build 4133: report to the MAKCU developers (2026-11-17)

Reply in the Discord thread where they sent the assigned build. Two messages, each under 2000 characters.
Attach `basics/comport_way/makcu_4133_logs_20261117.zip` (logs of every run below, README.txt inside).
Replace `R-XXXXXX` with the Toolkit report code taken after the working-load drop, or delete that line if none was taken.

---

## Message 1

Thanks for the build. Short answer: much better, but not gone. The board now recovers by itself; the drop itself still happens.

Board reports 4133. Same setup as R-DDEJ38: GameSir on its 2.4 GHz receiver in USB3, PC on USB2 COM (4 Mbaud), Auto smoothing, cables not touched. Logs attached (`makcu_4133_logs_20261117.zip`).

```
load                                        ran      result
GET 0x54 2000/s, no injection (repro)       300 s    clean
same                                        300 s    clean
same                                        73 s     drop, back in ~1.5 s
0x54 500/s + SET 0x40 every 2 ms            987 s    drop, back in ~2 s
 (~250 stick + ~75 button cmds/s)
```

On 4126 the same repro dropped 5/5 within 4-108 s and USB3 stayed dead until a full replug. On 4133, 5 s after the drop: `0x02` = `0x82`, `km.device()` = `R:KC;M:0uf;K:64uf;C:8uf`, 1000 reports/s, Windows re-enumerated the controller.

Before the 2000/s drop we saw the same precursor as on 4126: seconds where `sequence` advances ~900/s instead of 1000 (15 of 73 s). Before the working-load drop there was no visible precursor.

Toolkit reports (power never removed):
- R-HKX3SS: 2000/s drop, taken ~2 min after it recovered;
- R-XXXXXX: working-load drop.

---

## Message 2

Questions:

1. Is 4133 an automatic USB3 restart, or does it also change what causes the drop? If it is a restart: is there a flag or counter we can read (in `0x02`, `0x54` or elsewhere) to know it happened, so our code can resume cleanly?
2. Our normal load (~800 COM frames/s) still drops about once per 15-16 min. In a game the controller vanishing for 2 s and Windows re-plugging it is noticeable. What `0x54` + `0x40` rate over COM do you consider safe?
3. Would the `0x73` / `0x74` physical stream (instead of polling `0x54`) load USB3 less?

We keep 4133 installed and can run any other build or capture you send.

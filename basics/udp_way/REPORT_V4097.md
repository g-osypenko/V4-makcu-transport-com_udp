# V4.097 report to the MAKCU engineers (2026-10-12)

Two Discord messages (each under 2000 characters). Attach `makcu_udp_test.zip`.

---

## Message 1

V4.097 results: same scripts, mouse (Logitech Lightspeed 046D:C547, 1000 Hz) and NIC. Ramp 2000 px/s, 2.5 s per row, closed loop wherever the op replies. Updated scripts attached (`makcu_udp_test.zip`).

```
row                    moves/s  HID Hz  path live / drained
a) 0x18 @125/s             125     125   0.997 / 1.000
a) 0x18 @200/s             200     200   0.996 / 1.000
a) 0x18 @300/s             300     300   0.999 / 1.000
a) 0x18 @581/s             291     291   0.499 / 0.501
b) 0x69 @125/s             125     125   0.998 / 0.998
b) 0x69 @200/s             200     200   0.998 / 0.998
b) 0x69 @300/s             284     284   0.998 / 0.999
b) 0x69 @581/s             284     284   0.998 / 0.999
c) 0x6C N=7  @ 50 pkt/s    350     347   0.988 / 0.992
c) 0x6C N=7  @100 pkt/s    700     697   0.993 / 0.996
c) 0x6C N=7  @139 pkt/s    974     970   0.995 / 0.998
c) 0x6C N=16 @ 50 pkt/s    351     344   0.977 / 0.983
c) 0x6C N=16 @100 pkt/s    703     694   0.989 / 0.996
c) 0x6C N=16 @139 pkt/s    975     967   0.992 / 0.995
```

`path drained` < 1 on the b) and c) rows is only the carry still unsent when the window closed (4–85 px of ~5000). Delivered ÷ admitted = 1.000 on every row, and the board ends with 16/16 free records.

---

## Message 2

**0x6C receipts**
- N=7: every receipt result 0, admitted 7/7, free slots 2, freed ≈ 6.9 per packet, K ≈ 7, `more` 0, queued 7. No lost receipts, no X rejects. At 139 pkt/s, 97 % of HID intervals are 1 ms.
- N=16: result 1 (partial) on every packet after the first, ≈ 7 of 16 admitted, first-failure reason 3 (no space), free slots 0, `more` set. Same throughput as N=7, as you predicted.
- Sample partial receipt: `01 01 10 00 00 03 00 00 07 01 00 00 92 1c 00 00 | 83 1c 00 00 02 00 ...`

**0x18:** interpolation 0 now gives one report per command (1.00 reports/cmd). Exact up to 300 cmd/s at a fixed rate; above that only ~260–290/s are accepted (400 → 267, 500 → 259, 581 → 291).

**0x69** closed loop is unchanged at ~284 moves/s (2 datagrams per move).

**Real app:** we added 0x6C to our 1 kHz mover: one record per 1 ms tick, one packet in flight, next packet sent on the receipt with X from it, ≤ 7 records per packet. Receipt RTT p50 1.85 ms, ~1.9 records per packet, HID 983–1001 Hz, 98.5–99.7 % 1 ms intervals, 4000/4000 admitted, path exact.

**NIC:** the UGREEN 60554 hub is an **ASIX AX88179 gigabit NIC, VID:PID 0B95:1790**, behind a Genesys Logic hub chip 05E3:0610 (correction: not RTL8152). The mouse receiver is in the same hub, which is the only device on MAKCU USB3. PC → switch → the hub's RJ45.

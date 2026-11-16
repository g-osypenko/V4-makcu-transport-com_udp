# CH343 driver BSOD 0xD1 - short English copy (full text: HANDOFF_CH343_BSOD.md), 2026-11-16

- **Culprit confirmed by WinDbg on all 5 surviving minidumps (2026-10-30 … 2026-11-16, driver 2.1.2025.7 and 2.2.2026.5):**
  `CH343S64.SYS` (WCH), instruction `cmp qword ptr [rdx+38h],0` with `rdx = 0` at IRQL 2, offset `+0x6191` (2.1) / `+0x61f1` (2.2).
  `+0x38` in an x64 `IRP` is `IoStatus.Information`: most likely the driver checks the byte count of a current read IRP that was
  already completed elsewhere (race). Not provable without symbols.
- Two paths: the calling program's thread (`python.exe`, `pad_bench_com.exe`) and **the driver's own system thread**
  (`CH343S64+0x8660`, process `System`). The two newest crashes (2026-11-15 23:20, 2026-11-16 17:00) happened with COM5 merely held
  open by the owner's program, no traffic.
- **The same instruction and parameters appear in `CH341S64.SYS` dumps (CH340 driver) on the SparkFun forum** (System process, ports
  open, crash every few hours) - a long-standing bug in WCH's shared serial code; driver updates do not fix it.
- Arduino/go-serial: WCH drivers treat any read-timeout constant with the high bit set as 0, so go-serial spun thousands of empty
  reads per second and hit the BSOD; clamping to `0x7FFFFFFE` (real blocking read) made it go away (go-serial PR #200).
  Risk grows with the number of read IRPs the driver completes.
- **Best workaround (untested on MAKCU):** the CH343 enumerates as standard CDC-ACM (`USB\Class_02&SubClass_02&Prot_01`), so the
  Windows inbox `usbser.sys` (KMDF) can replace the WCH driver: Device Manager → COM5 → Update driver → pick "USB Serial Device".
  Must verify 4 Mbaud, latency, holes, and multi-hour stability.
- Meanwhile with the WCH driver: keep COM5 closed when not needed, immediate-return reads only, no empty-read spinning, never a
  timeout constant ≥ 0x80000000, no read timeouts.
- Open: which library/timeouts the owner's program uses; whether MAKCU sends unsolicited data over COM.

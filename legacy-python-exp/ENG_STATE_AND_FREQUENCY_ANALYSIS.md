# Current state of the MAKCU + ABCurves project and the frequency analysis (125 Hz vs 1000 Hz)

*English translation of [`STATE_AND_FREQUENCY_ANALYSIS.md`](STATE_AND_FREQUENCY_ANALYSIS.md) (Russian original). **Outdated:** the conclusions about a "125/500 Hz ceiling" were later disproved — see [`ENG_README.md`](ENG_README.md).*

This document records the current configuration, the work done, the physical measurements, and the technical contradictions between the injection rate and the Windows polling rate.

---

## 1. Hardware topology and network setup (single PC)

* **Computer:** Windows 10/11 x64, 4K Ultra HD monitor ($3840 \times 2160$), 240 Hz, Windows mouse acceleration off (`MouseSpeed=0`, `MouseThreshold1=0`, `MouseThreshold2=0`).
* **Board:** MAKCU (firmware **MAKCU V4.076 based on MAKOS**, build `1043460`).
  * Built on two **ESP32-S3** chips (the Device chip on USB 1 and the Host chip on USB 3), connected by a UART at **4,000,000 baud** (4 Mbaud, confirmed by the `0xA4` query).
* **Connection:**
  * **USB 1 (Device):** plugged straight into the PC (emulates the mouse in Windows, a clone of the Logitech G PRO Lightspeed `VID: 0x046D, PID: 0xC547`).
  * **USB 2 (COM):** fully disconnected (there are no COM ports in the system).
  * **USB 3 (Host):** connected to the UGREEN hub (the mouse receiver and an Ethernet patch cord are plugged into the hub).
* **Network protocol:**
  * Board IP: `192.168.50.175:8080`, **UDP** protocol, `UdpWireMode.RAW` mode.
  * Packet format: `0x55` (1B) + `nonce` (8B) + `0xDE 0xAD` (2B) + `0x04 0x00` (2B) + `0x18` (1B) + `dx: int16` (2B) + `dy: int16` (2B) = 18 bytes payload (60 bytes on the wire).

---

## 2. What has already been done and set up in the repository

1. **The built-in 17 ms smoothing filter in NOR flash was turned off:**
   * Firmware V4.076 had `mouse_spread_percent = 50` by default (it spread every injection over 17 ms).
   * Through a `DeviceSettings` patch in `makxd/settings.py` the parameter was forced to `mouse_spread_percent = 0` and permanently written into the board's NOR flash.
2. **A true scale was calibrated for 4K:**
   * With the filter off, the experimental scale is: **$1\text{ screen pixel} = 2.17\text{ mouse counts (mickeys)}$** (4.35x had been used by mistake before).
3. **A Q16.16 substepper was created and deployed (`abcurves_substepper.py`):**
   * A deterministic error-diffusion algorithm with a 32-bit register (precision $1/65536$ mickey) is working.
   * It removes the "sluggish start" at low speeds ($<1$ mickey/ms) and gives 100% preservation of deltas with no delay (0 ms lag).
   * A Flash & Hogan Minimum Jerk profile with a soft ramp ($10\tau^3 - 15\tau^4 + 6\tau^5$) was added, which removed the jumps at the start.
4. **The `RatePacer` clock generator was updated (`udp_mouse_controller.py`, `mouse_pattern_tester.py`):**
   * Protection against an avalanche flush (Anti-Burst Protection).
   * A protective hardware gap (`min_gap = 0.75 * interval`) was added, which guarantees that bursts of packets with zero delay cannot be sent.
5. **A fault-tolerant trajectory visualizer was created (`mouse_trajectory_canvas.py`):**
   * A 60 FPS canvas in a window or a 4K overlay, a ring buffer of 3000 points, hotkeys that work in Russian and English layouts, a background thread capturing the Windows cursor with `time.sleep(0)` with no memory leaks and no Tkinter C crashes.

---

## 3. Recorded facts and the frequency anomaly (the 125 Hz vs 1000 Hz puzzle)

### Fact 1. The physical mouse is polled at 1000 Hz
When the user moves a real mouse by hand, **Mouse Polling Rate Monitor v1.0** shows **980–1000 Hz** (`sigma ~0.06 ms`, status `NATURAL SIGNAL`). So the physical USB channel between MAKCU and the PC really can work at 1000 Hz.

### Fact 2. The script's network injection gives only 120–125 Hz in the monitor
When the script activates any movement mode (both the harmonic sine "Left <-> Right" and the continuous "Big circle 250 px", where the speed is constant and there are no stops):
* In the Python console the `pacer.sync()` timer steadily shows **500.0 Hz** (it sends 500 UDP packets per second).
* However **Mouse Polling Rate Monitor v1.0** in Windows stubbornly shows an average rate of **120–125 Hz** (exactly an interval of **8.0 ms**)!
* Meanwhile the onboard Windows cursor movement logger (`FlightRecorder` in `mouse_flight_log.csv`) shows that the real screen coordinates `GetCursorPos` update at intervals of 1 ms, 2 ms, 3 ms (average effective rate **~445 Hz**).

### Fact 3. An avalanche of bursts when moving by hand and injecting at the same time
When the user moves the mouse by hand against or across the script's movement:
* A huge number of **BURSTS (red bars)** instantly appears in the Polling Rate monitor, and the status switches to `INJECTION` / `SUSPICIOUS`.
* Cause: a report from the physical mouse (USB 3 Host) and a report from the UDP injector (Ethernet) go out onto the USB 1 bus as two independent USB HID reports with a microsecond difference in time ($\Delta t \approx 0.05$ ms). The monitor registers an ultra-short interval and detects a software injection.

### Fact 4. UART speed
A hardware query with the `0xA4` command showed the applied speed `0x003d0900` = **4,000,000 baud (4 Mbaud)**. The link between the chips is not a bottleneck.

---

## 4. Answers to the open questions and the solution of the frequency problem

> [!IMPORTANT]
> **The `buffer_ms` and `interpolation: auto / fixed / off` parameters do NOT belong to the mouse:**
> The hardware request `cfg.info()` on the board returns only `sections: SettingsSection.MOUSE: 4` (`kinds=3`, Mouse + Keyboard).
> The `SettingsSection.CONTROLLER` section (mask 1) is not supported by the hardware (error `status 5: Unsupported`).
> The `buffer_ms` (1..64) and `interpolation` (`off`/`fixed`/`auto`) parameters in the `mak-suite` SDK belong exclusively to the gamepad's analog sticks.
> The only mouse parameter in NOR Flash is **`mouse_spread_percent`** (set to `0%`, the spline is off).

1. **Why is the MAKCU virtual injector clocked at 500 Hz (4 substeps per 8 ms) and falls to 125 Hz?**
   * **USB descriptor:** set to a true 1000 Hz (`bInterval = 1`, 8 microframes of 125 µs = 1.0 ms).
   * **FreeRTOS injector quantum:** the internal task `Mouse Injection Task` on the Device chip has a base dispatch step of **2.0 ms**.
   * On receiving an 8 ms packet (125 Hz UDP) the board generates $8 / 2 = 4$ substeps of 2 ms $\implies$ **500 Hz USB HID**.
   * **Why sending packets with a 4.0 ms step (250 Hz) is unacceptable:** physical measurements proved that the ESP32 substepper is rigidly quantized to an 8 ms window. If the next packet arrives after 4 ms, it cuts off the unfinished substeps 3 and 4, shearing off exactly **33.3% of the distance** (of 300 mickeys in the 200 ms test exactly 199/200 arrived, 99 HID reports instead of 150). On a long 5 s circle this led to asymmetric truncation of the axes and drift ($dx=-12, dy=+10$).

2. **How to reconcile the 1000 Hz ABCurves renderer:**
   * The only hardware mode for opcode `0x18`: **125 Hz UDP (an 8.0 ms frame, aggregation of 8 ABCurves ticks)**.
   * The board manages to fully run all 4 substeps of 2 ms $\implies$ **100.0% path delivery (0 losses), a perfect loop closure and a stable 500 Hz USB HID**.
   * For direct output into every 1 ms USB frame (1000 Hz), bypassing the 8 ms substepper, the text opcode `0x6B` (`m.move_now`) is used.

3. **Eliminating bursts when moving by hand:**
   * Method A: a hardware mask on the hand while aiming (`0x16 MOVE_MASK` `b"\x01\x01\x01\x01"`).
   * Method B: a zero spline `mouse_spread_percent = 0%` and strict synchronization to the 125 Hz frame grid.

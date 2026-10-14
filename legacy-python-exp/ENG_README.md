# legacy-python-exp: early experiments (6–8 October 2026)

*English translation of [`README.md`](README.md) (Russian original).*

Python scripts and notes from the first days of the project, before the solutions were found. Until 2026-10-12 they lived in the root
of the repository. **Do not use for new work**: here the mouse was driven through `0x18 MOVE`
(the firmware interpolator) and the `makxd` SDK, the rate was stuck at ~640–700 Hz, and the conclusions of the analytical
notes about a "700 Hz ceiling" turned out to be wrong.

Current solutions:

- COM, 1000 Hz: [`basics/comport_way/perfect-transport-comport/`](../basics/comport_way/perfect-transport-comport/ENG_README.md)
- UDP/Ethernet, 1000 Hz: [`basics/udp_way/perfect-transport-udp/`](../basics/udp_way/perfect-transport-udp/ENG_README.md)
- project overview: [`ENG_CONTEXT_AND_SETUP.md`](../ENG_CONTEXT_AND_SETUP.md)

| File | What it is |
| --- | --- |
| `udp_mouse_controller.py` | the first working UDP client through the `makxd` SDK (`UdpWireMode.RAW`) |
| `test_udp_raw_move.py` | UDP on bare sockets: building the `0x55 + nonce + DE AD ...` packet |
| `test_binary_only.py`, `test_mouse_basics.py` | binary MAK_API and text KM_API over COM |
| `mouse_pattern_tester.py`, `mouse_trajectory_canvas.py`, `abcurves_substepper.py` | a 1 kHz trajectory tester on `0x18`, visualization, ABCurves slicing |
| `mouse_telemetry_probe.py`, `analyze_telemetry.py`, `fit_circle_trajectory.py`, `test_displacement.py` | path telemetry and its analysis |
| `makcu_control_panel.py`, `maksuite_gui.py`, `maksuite_simple.py` | GUI panels on top of the SDK |
| `test_abcurves_smoke.py` | ABCurves smoke test |
| `POLLING_RATE_AND_QUEUE_DEEP_DIVE.md`, `STATE_AND_FREQUENCY_ANALYSIS.md` | hypotheses about the polling rate (outdated) |
| `*.csv`, `board_settings_backup.*`, `visualizer_crash.log` | data of these scripts (outside git, `.gitignore`) |

The scripts refer to each other and to their `*.csv` files by relative paths: run them from this folder
with `..\.venv\Scripts\python.exe`.

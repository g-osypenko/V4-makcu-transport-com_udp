# legacy-python-exp: ранние эксперименты (6–8 октября 2026)

Python-скрипты и заметки первых дней проекта, до найденных решений. До 2026-10-12 лежали в корне
репозитория. **Для новой работы не использовать**: здесь мышь управлялась через `0x18 MOVE`
(интерполятор прошивки) и SDK `makxd`, частота упиралась в ~640–700 Гц, а выводы аналитических
заметок про «потолок 700 Гц» оказались неверными.

Актуальные решения:

- COM, 1000 Гц: [`basics/comport_way/perfect-transport-comport/`](../basics/comport_way/perfect-transport-comport/README.md)
- UDP/Ethernet, 1000 Гц: [`basics/udp_way/perfect-transport-udp/`](../basics/udp_way/perfect-transport-udp/README.md)
- обзор проекта: [`CONTEXT_AND_SETUP.md`](../CONTEXT_AND_SETUP.md)

| Файл | Что это |
| --- | --- |
| `udp_mouse_controller.py` | первый рабочий UDP-клиент через SDK `makxd` (`UdpWireMode.RAW`) |
| `test_udp_raw_move.py` | UDP на голых сокетах: сборка пакета `0x55 + nonce + DE AD ...` |
| `test_binary_only.py`, `test_mouse_basics.py` | бинарный MAK_API и текстовый KM_API через COM |
| `mouse_pattern_tester.py`, `mouse_trajectory_canvas.py`, `abcurves_substepper.py` | тестер траекторий 1 кГц на `0x18`, визуализация, нарезка ABCurves |
| `mouse_telemetry_probe.py`, `analyze_telemetry.py`, `fit_circle_trajectory.py`, `test_displacement.py` | телеметрия пути и её разбор |
| `makcu_control_panel.py`, `maksuite_gui.py`, `maksuite_simple.py` | GUI-панели поверх SDK |
| `test_abcurves_smoke.py` | дымовой тест ABCurves |
| `POLLING_RATE_AND_QUEUE_DEEP_DIVE.md`, `STATE_AND_FREQUENCY_ANALYSIS.md` | гипотезы о частоте опроса (устарели) |
| `*.csv`, `board_settings_backup.*`, `visualizer_crash.log` | данные этих скриптов (вне git, `.gitignore`) |

Скрипты ссылаются друг на друга и на свои `*.csv` по относительным путям: запускать из этой папки
через `..\.venv\Scripts\python.exe`.

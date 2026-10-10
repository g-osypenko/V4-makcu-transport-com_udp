@echo off
setlocal
REM Build makcu_udp_mover.exe (MinGW-w64 g++ 13+, C++23). No mak-suite SDK:
REM only WinAPI + ws2_32. Shared headers (motion_engine.h, substepper.h,
REM abc_renderer.h, hook_monitor.h, rawinput_monitor.h, rate_probe.h,
REM mover_common.h, mover_transport.h) come from ..\comport_way.
REM ABCurves native Renderer (C99) from ABCurves\runtime\c.
REM (ASCII only: cmd.exe misreads UTF-8 batch files.)

set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "COMMON=%HERE%..\comport_way"
set "ABC=%ROOT%\ABCurves\runtime\c"
set "ABC_OBJ=%ROOT%\build\abcurves_udp"

where g++ >nul 2>&1 || (echo [-] g++ not found in PATH, need MinGW-w64 e.g. C:\mingw64\bin & exit /b 1)

echo [*] Compiling ABCurves Renderer (C99) ...
if not exist "%ABC_OBJ%" mkdir "%ABC_OBJ%"
for %%F in (abc_online abc_fixed abc_w5_stream) do (
    gcc -std=c99 -O2 -Wall -Wextra -ffp-contract=off -I "%ABC%\include" -I "%ABC%\src" -c "%ABC%\src\%%F.c" -o "%ABC_OBJ%\%%F.o" || exit /b 1
)

echo [*] Compiling makcu_udp_mover.exe ...
g++ -std=c++23 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0A00 ^
    -I "%HERE%." -I "%COMMON%" -I "%ABC%\include" ^
    "%HERE%makcu_udp_mover.cpp" ^
    "%ABC_OBJ%\abc_online.o" "%ABC_OBJ%\abc_fixed.o" "%ABC_OBJ%\abc_w5_stream.o" ^
    -lws2_32 -lwinmm ^
    -static ^
    -o "%HERE%makcu_udp_mover.exe" || exit /b 1

echo [+] OK: %HERE%makcu_udp_mover.exe
endlocal

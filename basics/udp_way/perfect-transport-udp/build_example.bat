@echo off
setlocal
REM Build example_line.exe and controls_test.exe: MinGW-w64 g++ (C++17 or newer), no other dependencies.
REM (ASCII only: cmd.exe misreads UTF-8 batch files.)
set "HERE=%~dp0"
where g++ >nul 2>&1 || (echo [-] g++ not found in PATH, need MinGW-w64 e.g. C:\mingw64\bin & exit /b 1)
g++ -std=c++17 -O2 -Wall -Wextra "%HERE%example_line.cpp" -lws2_32 -lwinmm -static -o "%HERE%example_line.exe" || exit /b 1
echo [+] OK: %HERE%example_line.exe
g++ -std=c++17 -O2 -Wall -Wextra "%HERE%controls_test.cpp" -lws2_32 -lwinmm -lpsapi -lgdi32 -static -o "%HERE%controls_test.exe" || exit /b 1
echo [+] OK: %HERE%controls_test.exe
endlocal

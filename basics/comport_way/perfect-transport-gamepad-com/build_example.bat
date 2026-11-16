@echo off
rem Build the gamepad reference example, test and benchmark (COM). MinGW-w64 in C:\mingw64\bin.
rem Run from PowerShell: .\build_example.bat
setlocal
set PATH=C:\mingw64\bin;%PATH%
set FLAGS=-std=c++20 -O2 -Wall -Wextra -static
g++ %FLAGS% example_pad.cpp -lsetupapi -lwinmm -o example_pad.exe || exit /b 1
if exist pad_controls_test.cpp g++ %FLAGS% pad_controls_test.cpp -lsetupapi -lwinmm -lgdi32 -o pad_controls_test.exe || exit /b 1
if exist pad_bench_com.cpp g++ %FLAGS% pad_bench_com.cpp -lsetupapi -lwinmm -lgdi32 -o pad_bench_com.exe || exit /b 1
if exist pad_soak.cpp g++ %FLAGS% pad_soak.cpp -lsetupapi -lwinmm -o pad_soak.exe || exit /b 1
echo OK

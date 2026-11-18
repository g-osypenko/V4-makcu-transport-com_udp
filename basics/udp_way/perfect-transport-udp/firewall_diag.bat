@echo off
REM Firewall diagnostics, read only. Writes firewall_diag.txt next to this file.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0firewall_diag.ps1"
pause

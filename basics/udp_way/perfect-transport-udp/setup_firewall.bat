@echo off
REM Allow MAKCU button events through the Windows firewall (see setup_firewall.ps1). Double click is enough:
REM it asks for admin rights itself. Options go to the script: -Check, -Remove, -RemoteAddress X, -InterfaceAlias X, -WhatIf.
REM (ASCII only: cmd.exe misreads UTF-8 batch files.)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_firewall.ps1" %*
set "CODE=%ERRORLEVEL%"
pause
exit /b %CODE%

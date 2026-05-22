@echo off
REM shot.bat -- capture the running VOTV window to a PNG, with NO in-game toast
REM and NO focus theft (external Windows GDI grab, our own code).
REM
REM Usage:  shot.bat            -> writes tools\shots\shot-<timestamp>.png
REM         shot.bat my.png     -> writes the given path
REM
REM Uses Windows PowerShell 5.1 (powershell.exe) because capture-window.ps1
REM needs System.Drawing. The game must be running.

setlocal
set "TOOLS=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" (
  if not exist "%TOOLS%shots" mkdir "%TOOLS%shots"
  for /f "tokens=2 delims==" %%t in ('wmic os get localdatetime /value 2^>nul ^| find "="') do set "TS=%%t"
  set "OUT=%TOOLS%shots\shot-%TS:~0,14%.png"
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TOOLS%capture-window.ps1" -ProcessName VotV-Win64-Shipping -OutPath "%OUT%"
echo Saved: %OUT%
endlocal

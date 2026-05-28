@echo off
REM Thin shim. Real launcher: tools/mp.py
REM Launches CLIENT #2 from Game_0.9.0n_copy2/ for 3-peer LAN testing.
REM No-arg form connects to 127.0.0.1 (LAN loopback, same machine as host).
if "%~1"=="" (
    python "%~dp0tools\mp.py" client2
) else (
    python "%~dp0tools\mp.py" client2 --peer %~1 %2 %3 %4 %5
)

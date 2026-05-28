@echo off
REM Thin shim. Real launcher: tools/mp.py
REM No-arg form connects to 127.0.0.1 (LAN loopback, same machine as host).
if "%~1"=="" (
    python "%~dp0tools\mp.py" client
) else (
    python "%~dp0tools\mp.py" client --peer %~1 %2 %3 %4 %5
)

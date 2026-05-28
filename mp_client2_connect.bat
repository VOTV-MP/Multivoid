@echo off
REM Thin shim. Real launcher: tools/mp.py
REM Launches CLIENT #2 from Game_0.9.0n_copy2/ for 3-peer LAN testing.
python "%~dp0tools\mp.py" client2 --peer %1 %2 %3 %4 %5

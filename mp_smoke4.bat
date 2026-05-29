@echo off
REM Autonomous 4-PEER LAN smoke (Tier 8 cross-peer relay verdict).
REM Host + 3 clients (Game_0.9.0n + _copy + _copy2 + _dev), staggered connect,
REM then a log-driven verdict that proves each client sees the other clients
REM via the Tier 2 host-relay. Real driver: tools/mp.py smoke4
python "%~dp0tools\mp.py" smoke4 %*

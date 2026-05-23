@echo off
REM mp_host_game.bat -- launch VOTV as the coop HOST (binds the UDP port).
REM
REM Deploys the standalone mod into BOTH game folders (Game_0.9.0n for the host
REM + Game_0.9.0n_copy for the client) so the user only has to run ONE bat after
REM rebuilding. deploy-loader.ps1 is idempotent (skip-if-identical), so a re-run
REM while VOTV is still loaded does NOT fail on the locked xinput1_3.dll -- only
REM a real rebuild changes bytes and then needs the prior instance closed.
REM
REM Configures the harness via environment variables (VOTVCOOP_NET_ROLE / _PORT
REM / _NICK -- harness reads env BEFORE votv-coop.ini), then launches the host.
REM
REM Usage:
REM   mp_host_game.bat                  port=47621, nick=Host
REM   mp_host_game.bat 47700            custom port
REM   mp_host_game.bat 47621 MyNick     custom port + nickname
REM
REM Build first if needed:  cmake --build build\votv-coop --config Release
REM Restore UE4SS after testing: stop-coop.bat

setlocal
set "ROOT=%~dp0"
set "GAMEDIR=%ROOT%Game_0.9.0n"
set "WIN64=%GAMEDIR%\WindowsNoEditor\VotV\Binaries\Win64"
set "COPYDIR=%ROOT%Game_0.9.0n_copy"
set "COPYWIN64=%COPYDIR%\WindowsNoEditor\VotV\Binaries\Win64"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=47621"
set "NICK=%~2"
if "%NICK%"=="" set "NICK=Host"

echo Deploying standalone mod to host folder ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\deploy-loader.ps1" -Standalone -GameWin64 "%WIN64%"
if errorlevel 1 (
  echo.
  echo Deploy failed. Did you build?  cmake --build build\votv-coop --config Release
  pause
  exit /b 1
)

REM Also deploy to the client copy folder (if it exists) so the user doesn't have
REM to remember to redeploy there too. Same idempotent copy -> no-op when bytes
REM already match. Missing copy folder = skip (user is running cross-machine).
if exist "%COPYWIN64%\VotV-Win64-Shipping.exe" (
  echo Deploying standalone mod to client copy folder ...
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\deploy-loader.ps1" -Standalone -GameWin64 "%COPYWIN64%"
  if errorlevel 1 (
    echo WARN: client copy deploy failed -- proceeding with host only.
  )
)

REM Write scenario.txt (play = hands-on with you in control). Newline-free.
<nul set /p "=play" > "%WIN64%\scenario.txt"
del "%WIN64%\votv-coop.log" 2>nul

REM Hand the harness its net config via env vars. The harness reads env BEFORE
REM votv-coop.ini, so this overrides any net.* keys without editing the ini.
set "VOTVCOOP_SCENARIO=play"
set "VOTVCOOP_NET_ROLE=host"
set "VOTVCOOP_NET_PORT=%PORT%"
set "VOTVCOOP_NET_NICK=%NICK%"

echo.
echo Launching VOTV as HOST (port=%PORT%, nick=%NICK%)
echo Tell your friend to run:  mp_client_connect.bat ^<your-LAN-IP^>
echo (Find your LAN IP with  ipconfig  -- e.g. 192.168.x.x)
echo.

start "" "%WIN64%\VotV-Win64-Shipping.exe" -windowed -ResX=1920 -ResY=1080

echo Running. Press F12 for a screenshot; F2 for the dev pos/cam overlay.
echo When done, run stop-coop.bat to restore UE4SS.
endlocal

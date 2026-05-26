@echo off
REM mp_client_connect.bat -- launch VOTV as the coop CLIENT, connecting to a host.
REM
REM Same-machine testing: each instance needs its OWN game folder, because the
REM host process holds xinput1_3.dll loaded and a second instance can't load a
REM different copy from the same dir. The client runs from a SIBLING copy of
REM the game folder: Game_0.9.0n_copy. Make that copy ONCE (Explorer copy-paste
REM is fine; it can be a "bare" copy with no mods -- this script deploys the
REM standalone loader into it).
REM
REM Deploys to BOTH game folders (host + client copy) using the idempotent
REM deploy-loader.ps1 -- a re-run while VOTV is still loaded does NOT fail on
REM the locked xinput1_3.dll (skip-if-identical); only a real rebuild needs the
REM prior instance closed.
REM
REM For cross-machine play (real LAN), the OTHER PC just needs the game folder
REM at Game_0.9.0n (no _copy needed) -- you only need two copies on ONE PC.
REM
REM Usage:
REM   mp_client_connect.bat                       peer=127.0.0.1 (same-box test)
REM   mp_client_connect.bat 192.168.1.42          custom peer IP, port=47621
REM   mp_client_connect.bat 192.168.1.42 47700    custom peer IP + port
REM   mp_client_connect.bat 192.168.1.42 47621 Bob   peer + port + nickname

setlocal
set "ROOT=%~dp0"
set "GAMEDIR=%ROOT%Game_0.9.0n_copy"
set "WIN64=%GAMEDIR%\WindowsNoEditor\VotV\Binaries\Win64"
set "HOSTDIR=%ROOT%Game_0.9.0n"
set "HOSTWIN64=%HOSTDIR%\WindowsNoEditor\VotV\Binaries\Win64"

REM Launch the original VotV-Win64-Shipping.exe (NOT a renamed copy).
REM A rename attempt (VotV-Client.exe) was reverted on 2026-05-26 after the
REM matching host rename produced a 16.7 GB RAM hang -- UE4.27 derives
REM Saved/ + DDC + Crash paths from FPlatformProcess::ExecutableName(), so
REM a renamed binary triggers a first-launch DDC rebuild + CrashReportClient
REM mismatch. Distinct-process identification for OBS / Task Manager is
REM done via DIFFERENT WINDOW TITLES applied by our DLL (shutdown::
REM UpdateWindowTitle -> "VotV (Host)" / "VotV (Client)"). OBS Window
REM Capture, Alt-Tab, Task Manager Apps tab all distinguish by title.
set "CLIENT_EXE=%WIN64%\VotV-Win64-Shipping.exe"

if not exist "%WIN64%\VotV-Win64-Shipping.exe" (
  echo.
  echo ERROR: client game folder not found at:
  echo   %GAMEDIR%
  echo.
  echo Same-machine coop needs a second game folder so the client has its own
  echo xinput1_3.dll ^(the host instance locks the original one^). Make it once:
  echo.
  echo   xcopy /E /I /H /Y "%ROOT%Game_0.9.0n" "%ROOT%Game_0.9.0n_copy"
  echo.
  echo Or just copy the folder in Explorer ^(can be a bare copy, no mods needed --
  echo this script deploys the standalone loader into it^).
  pause
  exit /b 1
)

set "PEER=%~1"
if "%PEER%"=="" set "PEER=127.0.0.1"
set "PORT=%~2"
if "%PORT%"=="" set "PORT=47621"
set "NICK=%~3"
if "%NICK%"=="" set "NICK=Client"

REM Same 1080p window as the host -- per [[feedback-user-prefers-1080-windows]]
REM the user explicitly rejected the smaller 720p default ("I don't like the
REM 1280 720 window make it 1080"). Override with `set RESX=...` / `set RESY=...`
REM for a custom size on a smaller secondary monitor.
if "%RESX%"=="" set "RESX=1920"
if "%RESY%"=="" set "RESY=1080"

echo Deploying standalone mod to client copy folder ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\deploy-loader.ps1" -Standalone -GameWin64 "%WIN64%"
if errorlevel 1 (
  echo.
  echo Deploy failed. Did you build?  cmake --build build\votv-coop --config Release
  pause
  exit /b 1
)

REM Also deploy to the host folder (if it exists) so the user doesn't have to
REM remember to redeploy there too. Idempotent copy -> no-op when bytes already
REM match (i.e. the host VOTV is still running with the same DLL loaded).
if exist "%HOSTWIN64%\VotV-Win64-Shipping.exe" (
  echo Deploying standalone mod to host folder ...
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\deploy-loader.ps1" -Standalone -GameWin64 "%HOSTWIN64%"
  if errorlevel 1 (
    echo WARN: host folder deploy failed -- proceeding with client only.
  )
)

REM Write scenario.txt (play = hands-on with you in control). Newline-free.
<nul set /p "=play" > "%WIN64%\scenario.txt"
del "%WIN64%\votv-coop.log" 2>nul

REM Hand the harness its net config via env vars. Harness reads env BEFORE
REM votv-coop.ini, so this overrides any net.* keys without editing the ini.
set "VOTVCOOP_SCENARIO=play"
set "VOTVCOOP_NET_ROLE=client"
set "VOTVCOOP_NET_PEER=%PEER%"
set "VOTVCOOP_NET_PORT=%PORT%"
set "VOTVCOOP_NET_NICK=%NICK%"

echo.
echo Launching VOTV CLIENT from %GAMEDIR%
echo   peer=%PEER%:%PORT%  nick=%NICK%  window=%RESX%x%RESY%
echo   exe=%CLIENT_EXE%
echo Make sure the host already ran  mp_host_game.bat  on %PEER%.
echo.

start "" "%CLIENT_EXE%" -windowed -ResX=%RESX% -ResY=%RESY%

echo Running. Press F12 for a screenshot; F2 for the dev pos/cam overlay.
echo When done, run stop-coop.bat to restore UE4SS in the host folder (the
echo client copy never had UE4SS to begin with).
endlocal

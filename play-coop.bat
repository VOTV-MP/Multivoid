@echo off
REM play-coop.bat -- one-click hands-on coop test (standalone, no UE4SS).
REM
REM Deploys the standalone mod (xinput1_3 proxy + votv-coop.dll), sets the
REM harness scenario, and launches VOTV windowed. The mod skips into gameplay
REM and spawns a 2nd mainPlayer_C beside you with its body visible -- walk
REM around / turn to see it. UE4SS is disabled for a clean standalone run.
REM
REM Usage:  play-coop.bat              (scenario "play" -- spawn 2nd player, you control)
REM         play-coop.bat orphan       (auto spawn + pose-drive demo)
REM         play-coop.bat newgame      (just skip into gameplay, no 2nd player)
REM Build first if needed:  cmake --build build\votv-coop --config Release
REM Restore UE4SS afterwards with:  stop-coop.bat

setlocal
set "ROOT=%~dp0"
set "WIN64=%ROOT%Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64"

set "SCENARIO=%~1"
if "%SCENARIO%"=="" set "SCENARIO=play"

echo Deploying standalone mod (UE4SS disabled)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\deploy-loader.ps1" -Standalone
if errorlevel 1 (
  echo.
  echo Deploy failed. Did you build?  cmake --build build\votv-coop --config Release
  pause
  exit /b 1
)

REM Write scenario.txt with no trailing newline.
<nul set /p "=%SCENARIO%" > "%WIN64%\scenario.txt"
del "%WIN64%\votv-coop.log" 2>nul

echo Launching VOTV (scenario=%SCENARIO%)...
start "" "%WIN64%\VotV-Win64-Shipping.exe" -windowed -ResX=1920 -ResY=1080
echo.
echo Running. The 2nd player spawns a few seconds after gameplay loads.
echo Press F12 in-game for a screenshot (saved to Win64\coop-screenshots\).
echo When done, run stop-coop.bat to restore UE4SS.
endlocal

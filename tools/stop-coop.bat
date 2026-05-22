@echo off
REM stop-coop.bat -- uninstall the standalone mod and restore UE4SS.
REM Removes the xinput1_3 proxy + votv-coop.dll + markers and re-enables UE4SS
REM (renames dwmapi.dll.off back). Run this after play-coop.bat.

setlocal
set "TOOLS=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%TOOLS%deploy-loader.ps1" -Remove
echo Done. UE4SS restored; standalone mod removed.
endlocal

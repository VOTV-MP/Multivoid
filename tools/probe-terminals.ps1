<#
.SYNOPSIS
  Hands-on UE4SS Lua probe of VOTV story-object terminals (Phase 5T Inc0).

.DESCRIPTION
  Disables the standalone coop proxy temporarily, deploys probe_terminals.lua v3,
  sets the harness scenario to load s_may2026 + run the probe, clears the
  UE4SS log, and launches the dev game copy. The probe auto-fires its
  dangerous tests ONLY when the player is in active terminal-use state
  (mainPlayer.activeInterface != nil), so the user just needs to walk up
  to a terminal and press E.

  Restore-proxy step is done by passing -Restore (after testing is done).

.EXAMPLE
  ./tools/probe-terminals.ps1
  # In game: walk to SAT computer + E, wait 5s, Escape. Done.

  ./tools/probe-terminals.ps1 -Restore
  # After testing: re-enable standalone proxy.
#>
[CmdletBinding()]
param(
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'

$dev = "$PSScriptRoot\..\Game_0.9.0n_dev\WindowsNoEditor\VotV\Binaries\Win64"
if (-not (Test-Path $dev)) { throw "Dev copy not found at $dev" }
$dev = (Resolve-Path $dev).Path

if ($Restore) {
    $disabled = Join-Path $dev 'xinput1_3.dll.probe-disabled'
    if (Test-Path $disabled) {
        Rename-Item $disabled 'xinput1_3.dll' -Force
        Write-Host "standalone proxy RESTORED" -ForegroundColor Green
    } else {
        Write-Host "no .probe-disabled file found; nothing to restore" -ForegroundColor Yellow
    }
    exit 0
}

# 1) Disable standalone proxy temporarily
$proxy = Join-Path $dev 'xinput1_3.dll'
if (Test-Path $proxy) {
    Rename-Item $proxy 'xinput1_3.dll.probe-disabled' -Force
    Write-Host "standalone proxy disabled (will be restored with -Restore)" -ForegroundColor Cyan
}

# 2) Deploy v3 probe (source -> dev copy)
$src = Join-Path $PSScriptRoot 'probes\coopTestHarness\Scripts'
$dst = Join-Path $dev 'Mods\coopTestHarness\Scripts'
Copy-Item (Join-Path $src 'probe_terminals.lua') (Join-Path $dst 'probe_terminals.lua') -Force
Copy-Item (Join-Path $src 'main.lua') (Join-Path $dst 'main.lua') -Force
Write-Host "probe deployed" -ForegroundColor Cyan

# 3) Set scenario
Set-Content -Path (Join-Path $dev 'Mods\coopTestHarness\scenario.txt') `
            -Value 'probe_terminals:s_may2026' -NoNewline
Write-Host "scenario = probe_terminals:s_may2026" -ForegroundColor Cyan

# 4) Clear UE4SS log
Remove-Item (Join-Path $dev 'UE4SS.log') -Force -ErrorAction SilentlyContinue
Write-Host "UE4SS.log cleared" -ForegroundColor Cyan

# 5) Launch the game
$exe = Join-Path $dev 'VotV-Win64-Shipping.exe'
$proc = Start-Process -FilePath $exe -ArgumentList @('-ResX=1920','-ResY=1080','-windowed') `
                      -WorkingDirectory $dev -PassThru
Write-Host ""
Write-Host "Launched PID $($proc.Id)" -ForegroundColor Green
Write-Host ""
Write-Host "============================== USER SCRIPT ===============================" -ForegroundColor Yellow
Write-Host " 1. Wait ~45 seconds for save to load (you should be in gameplay)."
Write-Host " 2. Walk up to the SAT computer (big analog desk with multiple screens)."
Write-Host " 3. Press E to activate the terminal."
Write-Host " 4. Wait ~5 seconds (auto-fire suite runs — watch the screens flicker)."
Write-Host " 5. Press Escape to exit."
Write-Host ""
Write-Host " (Optional repeat at panel_radar / coordRadarDish / serverBox.)"
Write-Host ""
Write-Host " When done, CLOSE THE GAME WINDOW. Then ping me — I'll read the log."
Write-Host " When fully done, run:    ./tools/probe-terminals.ps1 -Restore"
Write-Host "==========================================================================" -ForegroundColor Yellow

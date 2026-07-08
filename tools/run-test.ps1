<#
.SYNOPSIS
    Launch VOTV for autonomous coop testing (windowed, UE4SS, scenario-driven).

.DESCRIPTION
    Writes the scenario file that coopTestHarness reads, then launches the
    shipping exe (which UE4SS hooks via its dwmapi.dll proxy) windowed at a
    fixed resolution. The harness mod skips menus into gameplay per the
    scenario, and CTRL+9 in-game takes screenshots.

    Requires: install-ue4ss.ps1 (substrate) + deploy-probe.ps1 -Name
    coopTestHarness (this harness deployed) to have been run.

.PARAMETER Scenario
    newgame  -> auto New Game into gameplay (most deterministic; default)
    loadlast -> attempt to load the most recent save
    none     -> launch only; drive manually with the harness keybinds

.EXAMPLE
    ./tools/run-test.ps1 -Scenario newgame -ResX 1280 -ResY 720
#>
[CmdletBinding()]
param(
    [ValidateSet('newgame','none','inspect','orphan','drive','probe_terminals')] [string]$Scenario = 'newgame',
    [string]$Save = '',   # if set, load this save slot (e.g. s_ehehe) instead of -Scenario
    [int]$ResX = 1280,
    [int]$ResY = 720,
    [switch]$Fullscreen,
    [string]$Win64Dir = "$PSScriptRoot\..\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    [string[]]$ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
$Win64Dir = (Resolve-Path $Win64Dir).Path
$exe = Join-Path $Win64Dir 'VotV-Win64-Shipping.exe'
if (-not (Test-Path $exe)) { throw "Shipping exe not found: $exe" }

# Write the scenario the harness mod reads on startup.
$harnessDir = Join-Path $Win64Dir 'Mods\coopTestHarness'
if (-not (Test-Path $harnessDir)) {
    throw "coopTestHarness not deployed. Run: ./tools/deploy-probe.ps1 -Name coopTestHarness"
}
$scenarioValue = if ($Save) { "load:$Save" } else { $Scenario }
Set-Content -Path (Join-Path $harnessDir 'scenario.txt') -Value $scenarioValue -NoNewline
Write-Host "Scenario = $scenarioValue" -ForegroundColor Cyan

# UE launch args.
$argList = @("-ResX=$ResX", "-ResY=$ResY")
if ($Fullscreen) { $argList += '-fullscreen' } else { $argList += '-windowed' }
$argList += $ExtraArgs

Write-Host "Launching: $exe $($argList -join ' ')" -ForegroundColor Cyan
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $Win64Dir -PassThru
Write-Host "Launched PID $($proc.Id). UE4SS GUI + harness load shortly." -ForegroundColor Green

# Where HighResShot screenshots land (UE writes under the project's Saved dir).
$shotDirs = @(
    (Join-Path $Win64Dir '..\..\Saved\Screenshots\WindowsNoEditor'),
    (Join-Path $env:LOCALAPPDATA 'VotV\Saved\Screenshots\WindowsNoEditor')
)
Write-Host "Screenshots (CTRL+9 in-game) will be under one of:" -ForegroundColor Green
$shotDirs | ForEach-Object { Write-Host "  $_" }
Write-Host "In-game keybinds: CTRL+8 skip-to-gameplay | CTRL+9 screenshot | CTRL+7 report" -ForegroundColor Green
Write-Host "Spawn-orphan probe (if deployed): CTRL+P spawn | CTRL+O report | CTRL+K destroy"

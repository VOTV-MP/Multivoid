<#
.SYNOPSIS
    Install UE4SS into the VOTV game directory (reproducible setup).

.DESCRIPTION
    Downloads a pinned UE4SS release and extracts it next to
    VotV-Win64-Shipping.exe, then applies our dev-friendly settings tweaks.
    This is ADDITIVE only -- it places a dwmapi.dll proxy + UE4SS.dll +
    Mods/ alongside the shipping exe and modifies NO original game file
    (principle 1). The game install is gitignored, so this script is the
    committed source of truth for how the substrate is set up.

    UE4SS auto-detects the engine version (UE4.27) via AOB scanning; no
    EngineVersionOverride is needed for VOTV.

.NOTES
    Pinned to UE4SS v3.0.1 (latest stable as of 2026-05-21). Bump $Version
    deliberately; don't track upstream HEAD (methodology: pin deps).
#>
[CmdletBinding()]
param(
    # Path to the folder containing VotV-Win64-Shipping.exe.
    [string]$Win64Dir = "$PSScriptRoot\..\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    [string]$Version  = "v3.0.1",
    [switch]$Force     # re-extract even if UE4SS.dll already present
)

$ErrorActionPreference = 'Stop'
$Win64Dir = (Resolve-Path $Win64Dir).Path
$exe = Join-Path $Win64Dir 'VotV-Win64-Shipping.exe'
if (-not (Test-Path $exe)) {
    throw "VotV-Win64-Shipping.exe not found in '$Win64Dir'. Pass -Win64Dir <path>."
}

$marker = Join-Path $Win64Dir 'UE4SS.dll'
if ((Test-Path $marker) -and -not $Force) {
    Write-Host "UE4SS already installed at $Win64Dir (use -Force to reinstall)." -ForegroundColor Yellow
    return
}

$asset   = "UE4SS_$Version.zip"
$url      = "https://github.com/UE4SS-RE/RE-UE4SS/releases/download/$Version/$asset"
$staging = Join-Path $PSScriptRoot '..\build\ue4ss-staging'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
$zip = Join-Path $staging $asset

if (-not (Test-Path $zip)) {
    Write-Host "Downloading $asset ..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $zip
}

Write-Host "Extracting into $Win64Dir ..." -ForegroundColor Cyan
Expand-Archive -Path $zip -DestinationPath $Win64Dir -Force

# --- Dev-friendly settings tweaks ---------------------------------------
# Show the UE4SS debug GUI (log + Live View object browser) on launch so a
# first run is self-verifying and reflection browsing is one window away.
$ini = Join-Path $Win64Dir 'UE4SS-settings.ini'
(Get-Content $ini) -replace '^GuiConsoleVisible\s*=.*', 'GuiConsoleVisible = 1' |
    Set-Content $ini

Write-Host "UE4SS $Version installed." -ForegroundColor Green
Write-Host "Launch VOTV, then in-game press:" -ForegroundColor Green
Write-Host "  CTRL+H        -> C++ headers (CXXHeaderDump/)"
Write-Host "  CTRL+J        -> object dump (UE4SS_ObjectDump.txt)"
Write-Host "  CTRL+Numpad9  -> UHT headers"
Write-Host "  CTRL+Numpad6  -> .usmap mappings"

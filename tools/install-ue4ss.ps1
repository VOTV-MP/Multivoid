<#
.SYNOPSIS
    Install the UE4SS substrate into a VOTV game directory (reproducible setup).

.DESCRIPTION
    Downloads the pinned UE4SS release and places it beside
    VotV-Win64-Shipping.exe. Since WP-2 of the D-3 UE4SS migration
    (2026-08-22) UE4SS is the mod's LOADER on every game copy -- this script
    is the one-time per-copy substrate install; the per-build mod deploy is
    tools/deploy-mod.ps1 / deploy-all.ps1.

    INVARIANT: the installer owns substrate PRESENCE (dwmapi.dll, UE4SS.dll,
    the initial settings/mods.txt seed), NEVER standing settings state.
    - Extraction runs only when UE4SS.dll is absent (or -Force), via a staging
      dir, and NEVER overwrites an existing Mods\mods.txt or UE4SS-settings.ini
      (state files -- e.g. the dev copy's hand `coopTestHarness : 1` row).
    - dwmapi.dll is made present every run (un-parked from dwmapi.dll.off, or
      pulled from the zip) -- a parked loader means the mod never loads.
    - Settings keys are written ONLY on a fresh seed, or when an explicit
      profile switch (-Quiet) is passed on THIS invocation.

    ADDITIVE only: places new files beside the shipping exe and modifies NO
    original game file (principle 1). The game install is gitignored, so this
    script is the committed source of truth for how the substrate is set up.

    UE4SS auto-detects the engine version (UE4.27) via AOB scanning; no
    EngineVersionOverride is needed for VOTV.

.NOTES
    Pinned to UE4SS v3.0.1 (the D-3 decision of record). Bump $Version
    deliberately; don't track upstream HEAD (methodology: pin deps).

.EXAMPLE
    ./tools/install-ue4ss.ps1 -Win64Dir <...>\Win64 -Quiet
    # Play-install profile: GUI console off (no extra window per instance).

    ./tools/install-ue4ss.ps1 -Win64Dir <...>\Win64
    # Dev profile on a FRESH install: GUI console visible (Live View, dumps).
#>
[CmdletBinding()]
param(
    # Path to the folder containing VotV-Win64-Shipping.exe.
    [string]$Win64Dir = "$PSScriptRoot\..\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    [string]$Version  = "v3.0.1",
    [switch]$Force,    # re-extract even if UE4SS.dll already present
    [switch]$Quiet     # profile switch: GuiConsoleEnabled=0 + GuiConsoleVisible=0
)

$ErrorActionPreference = 'Stop'
$Win64Dir = (Resolve-Path $Win64Dir).Path
$exe = Join-Path $Win64Dir 'VotV-Win64-Shipping.exe'
if (-not (Test-Path $exe)) {
    throw "VotV-Win64-Shipping.exe not found in '$Win64Dir'. Pass -Win64Dir <path>."
}

$ue4ssDll = Join-Path $Win64Dir 'UE4SS.dll'
$dwm      = Join-Path $Win64Dir 'dwmapi.dll'
$dwmOff   = Join-Path $Win64Dir 'dwmapi.dll.off'
$ini      = Join-Path $Win64Dir 'UE4SS-settings.ini'

$asset   = "UE4SS_$Version.zip"
$url     = "https://github.com/UE4SS-RE/RE-UE4SS/releases/download/$Version/$asset"
$staging = Join-Path $PSScriptRoot '..\build\ue4ss-staging'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
# Resolve NOW: $stage feeds a FullName.Substring($stage.Length+1) rel-path
# computation below, and an unresolved '..' in the prefix shifts the cut
# (measured 2026-08-22: every extracted rel lost its first 9 chars).
$staging = (Resolve-Path $staging).Path
$zip = Join-Path $staging $asset

function Get-Zip {
    if (-not (Test-Path $zip)) {
        Write-Host "Downloading $asset ..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $url -OutFile $zip
    }
    return $zip
}

# --- 1. Extraction (only when the substrate is absent, or -Force) ------------
$freshSeed = $false
if ((-not (Test-Path $ue4ssDll)) -or $Force) {
    Get-Zip | Out-Null
    $stage = Join-Path $staging "extract_$Version"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $stage -Force
    # Copy over, preserving existing STATE files (mods.txt, UE4SS-settings.ini):
    # a re-extract must never clobber hand-edited mod enable rows or a tuned
    # settings profile.
    $stateFiles = @('Mods\mods.txt', 'UE4SS-settings.ini')
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1)
        $dst = Join-Path $Win64Dir $rel
        $isState = $stateFiles -contains $rel
        if ($isState -and (Test-Path $dst)) {
            Write-Host "  preserved existing $rel" -ForegroundColor DarkGray
            return
        }
        if ($isState -and -not (Test-Path $dst) -and $rel -eq 'UE4SS-settings.ini') {
            $script:freshSeed = $true
        }
        $d = Split-Path -Parent $dst
        if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        Copy-Item $_.FullName $dst -Force
    }
    Remove-Item $stage -Recurse -Force
    Write-Host "UE4SS $Version extracted into $Win64Dir" -ForegroundColor Green
} else {
    Write-Host "UE4SS already present at $Win64Dir (use -Force to re-extract)." -ForegroundColor DarkGray
}

# --- 2. Substrate-whole: the loader chain must be complete every run ---------
if (-not (Test-Path $dwm)) {
    if (Test-Path $dwmOff) {
        Move-Item $dwmOff $dwm -Force
        Write-Host "  dwmapi.dll un-parked (was $((Split-Path -Leaf $dwmOff)))" -ForegroundColor Yellow
    } elseif (Test-Path $ue4ssDll) {
        # UE4SS.dll present but its loader proxy missing: pull dwmapi from the zip.
        Get-Zip | Out-Null
        $stage = Join-Path $staging "dwm_$Version"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        Expand-Archive -Path $zip -DestinationPath $stage -Force
        Copy-Item (Join-Path $stage 'dwmapi.dll') $dwm -Force
        Remove-Item $stage -Recurse -Force
        Write-Host "  dwmapi.dll restored from the pinned zip" -ForegroundColor Yellow
    }
}

# --- 3. Settings: fresh seed OR an explicit profile switch only --------------
function Set-IniKey([string]$key, [string]$value) {
    $content = Get-Content $ini
    $pattern = "^\s*$key\s*=.*"
    if ($content -match $pattern) {
        $content -replace $pattern, "$key = $value" | Set-Content $ini
    } else {
        Add-Content $ini "$key = $value"
    }
}
if (Test-Path $ini) {
    if ($Quiet) {
        Set-IniKey 'GuiConsoleEnabled' '0'
        Set-IniKey 'GuiConsoleVisible' '0'
        Write-Host "  settings: quiet profile (GuiConsoleEnabled=0, GuiConsoleVisible=0)" -ForegroundColor Cyan
    } elseif ($freshSeed) {
        # Dev-friendly fresh-seed default: show the UE4SS debug GUI (log + Live
        # View object browser) so a first run is self-verifying.
        Set-IniKey 'GuiConsoleVisible' '1'
        Write-Host "  settings: fresh seed, dev profile (GuiConsoleVisible=1)" -ForegroundColor Cyan
    } else {
        Write-Host "  settings: existing UE4SS-settings.ini untouched" -ForegroundColor DarkGray
    }
}

# --- 4. Verdict lines (evidence for conversion/deploy logs) ------------------
$gc = ''
if (Test-Path $ini) {
    $gc = ((Get-Content $ini) | Where-Object { $_ -match '^\s*GuiConsole(Enabled|Visible)' }) -join '; '
}
Write-Host ("substrate: UE4SS.dll=" + (Test-Path $ue4ssDll) + " dwmapi.dll=" + (Test-Path $dwm) + " [$gc]") -ForegroundColor Green
Write-Host "Dev shortcuts once in game (dev profile): CTRL+H headers, CTRL+J object dump." -ForegroundColor DarkGray

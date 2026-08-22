# deploy-mod.ps1 -- deploy the Multivoid UE4SS mod folder into one game copy.
#
# WP-2 of the D-3 UE4SS migration (2026-08-22): the mod ships as a real UE4SS
# mod folder -- Mods\Multivoid\dlls\main.dll + Mods\Multivoid\enabled.txt --
# started via the C-ABI start_mod() contract (src/loader/cppmod_entry.cpp).
# UE4SS (or unreal_shimloader) is the loader. The UE4SS substrate itself is
# installed once per game copy by tools/install-ue4ss.ps1; THIS script only
# deploys the current build into the mod folder (idempotent, SHA-skip).
#
# One-time hygiene: the retired standalone-lane files beside the exe
# (xinput1_3.dll, multivoid-<game>-<build>.dll, legacy votv-coop.dll) are
# removed -- a leftover versioned payload beside the exe trips the new
# main.dll's predecessor DISK scan into a deliberate REFUSE (upgrade guard).

[CmdletBinding()]
param(
    [string]$GameWin64 = "D:\Projects\Programming\VOTV_MP\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    [string]$BuildDir  = "D:\Projects\Programming\VOTV_MP\build\votv-coop\Release",
    [switch]$Remove       # uninstall the mod folder (leaves the UE4SS substrate in place)
)

$ErrorActionPreference = "Stop"
$modDir  = Join-Path $GameWin64 "Mods\Multivoid"
$dllDir  = Join-Path $modDir "dlls"
$dst     = Join-Path $dllDir "main.dll"
$enabled = Join-Path $modDir "enabled.txt"
$marker  = Join-Path $GameWin64 "multivoid-loaded.txt"

if ($Remove) {
    if (Test-Path $modDir) { Remove-Item $modDir -Recurse -Force }
    Remove-Item $marker -ErrorAction SilentlyContinue
    "mod folder removed from $GameWin64 (UE4SS substrate left in place)."
    return
}

# The build artifact keeps the versioned release-identity name
# multivoid-<game>-<build>.dll (WP-4 owns the distribution re-home); pick the
# newest one in the build dir -- a proto bump renames the output and older
# artifacts may linger there. It is deployed AS main.dll (the UE4SS mod-folder
# contract name).
$payloadSrc = Get-ChildItem (Join-Path $BuildDir "multivoid-*.dll") -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $payloadSrc) { throw "no multivoid-*.dll in $BuildDir -- build first: cmake --build build/votv-coop --config Release" }

# Substrate presence check (evidence, not a gate: a deploy before install-ue4ss
# is legal, the mod just cannot load until the substrate is there).
if (-not (Test-Path (Join-Path $GameWin64 "UE4SS.dll"))) {
    Write-Host "  WARN: UE4SS.dll not found beside the exe -- run tools/install-ue4ss.ps1 -Win64Dir `"$GameWin64`"" -ForegroundColor Yellow
}

# Inline .NET SHA256 rather than Get-FileHash on purpose (2026-05-30): under
# mp.py's child powershell with a degraded PSModulePath, Get-FileHash fails to
# autoload and aborts the whole deploy. The .NET type has no module dependency.
function Get-Sha256Hex($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($path))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}
function Copy-IfChanged($src, $dst) {
    if ((Test-Path $dst) -and ((Get-Sha256Hex $src) -eq (Get-Sha256Hex $dst))) {
        return $false  # already up-to-date (a RUNNING game holds the mapped DLL locked; identical bytes are fine)
    }
    Copy-Item $src $dst -Force
    return $true
}

# --- One-time standalone-lane hygiene (the retired xinput-proxy install) -----
foreach ($stale in @("xinput1_3.dll", "votv-coop.dll")) {
    $p = Join-Path $GameWin64 $stale
    if (Test-Path $p) {
        try { Remove-Item $p -Force -ErrorAction Stop; Write-Host "  removed retired-lane $stale" -ForegroundColor Yellow }
        catch { Write-Host "  WARN: retired-lane $stale is LOCKED (game running?) -- delete before next launch" -ForegroundColor Red }
    }
}
Get-ChildItem (Join-Path $GameWin64 "multivoid-*.dll") -ErrorAction SilentlyContinue | ForEach-Object {
    try { Remove-Item $_.FullName -Force -ErrorAction Stop; Write-Host "  removed retired-lane $($_.Name)" -ForegroundColor Yellow }
    catch { Write-Host "  WARN: retired-lane $($_.Name) is LOCKED (game running?) -- the mod will REFUSE to start beside it" -ForegroundColor Red }
}

# --- The mod folder ----------------------------------------------------------
if (-not (Test-Path $dllDir)) { New-Item -ItemType Directory -Force -Path $dllDir | Out-Null }
if (-not (Test-Path $enabled)) { New-Item -ItemType File -Force -Path $enabled | Out-Null }
$copied = Copy-IfChanged $payloadSrc.FullName $dst
# Marker freshness: boot re-plants multivoid-loaded.txt beside the exe; remove
# the previous run's so each launch's marker is this run's evidence.
Remove-Item $marker -ErrorAction SilentlyContinue

$sha = (Get-Sha256Hex $dst).Substring(0, 16)
$state = if ($copied) { "updated" } else { "up-to-date" }
"deployed mod ($state) -> $dst  [$($payloadSrc.Name) sha256=$sha]"

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
# multivoid-<game>-<build>.dll (WP-4 owns the distribution re-home); a proto bump
# renames the output and older artifacts linger beside it. It is deployed AS
# main.dll (the UE4SS mod-folder contract name).
#
# PICK BY DECLARED BUILD NUMBER, NOT BY MTIME (2026-08-25). The old selector was
# `Sort-Object LastWriteTime -Descending`, which makes the payload a function of
# which file the filesystem touched last rather than of which build is newest --
# so a rebuild of an OLD tag, a restored backup, or a `touch` silently ships the
# wrong DLL, and the only thing standing between that and the user is a hash the
# operator has to remember to check. The memory index has flagged this twice.
# The rule here is now the SAME one the xinput proxy applies at load time (scan
# multivoid-*.dll, take the highest build), so the deployer and the loader can
# never disagree about which artifact is current.
#
# THE ORDERING IS TOTAL, and it has to be: the build number is NOT a unique key.
# `multivoid-0.9.0n-141.dll` and `multivoid-0.9.0o-141.dll` both parse to 141 (a
# game-target bump without a proto bump leaves both in build/), and Sort-Object
# is not stable without -Stable -- so a single sort key would pick arbitrarily
# and hand back exactly the "which DLL actually shipped" ambiguity this selector
# was written to kill. Mtime returns as the TIE-BREAK only, never as the key.
$payloadSrc = Get-ChildItem (Join-Path $BuildDir "multivoid-*.dll") -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^multivoid-.*-(\d+)\.dll$' } |
    Sort-Object -Property `
        @{Expression = { [int]([regex]::Match($_.Name, '^multivoid-.*-(\d+)\.dll$').Groups[1].Value) }; Descending = $true}, `
        @{Expression = { $_.LastWriteTime }; Descending = $true}, `
        @{Expression = { $_.Name }; Descending = $true} |
    Select-Object -First 1
if (-not $payloadSrc) { throw "no multivoid-*.dll in $BuildDir -- build first: cmake --build build/votv-coop --config Release" }

# ...AND THE WINNER MUST BE THE ONE **THIS SOURCE TREE** DECLARES. Highest-build alone answers a
# different question than the operator is asking: old artifacts are documented to linger in
# $BuildDir, so checking out an older tag (a bisect, or reproducing a field bug) picks the STALE
# higher-numbered DLL and deploys code that is not in the working tree -- a case the old mtime rule
# happened to get right. Both halves of the identity are declared in exactly one place each, so read
# them and refuse a mismatch instead of trusting an ordering.
$repoRoot = Split-Path -Parent $PSScriptRoot
$expectTarget = $null; $expectBuild = $null
$cml = Join-Path $repoRoot "src/votv-coop/CMakeLists.txt"
$proto = Join-Path $repoRoot "src/votv-coop/include/coop/net/protocol.h"
if (Test-Path $cml) {
    $m = [regex]::Match((Get-Content $cml -Raw), 'set\(VOTVCOOP_GAME_TARGET\s+"([^"]+)"\)')
    if ($m.Success) { $expectTarget = $m.Groups[1].Value }
}
if (Test-Path $proto) {
    foreach ($ln in (Get-Content $proto)) {
        $m = [regex]::Match($ln, '^\s*inline constexpr uint16_t kProtocolVersion\s*=\s*(\d+)\s*;')
        if ($m.Success) { $expectBuild = $m.Groups[1].Value; break }
    }
}
if ($expectTarget -and $expectBuild) {
    $expectName = "multivoid-$expectTarget-$expectBuild.dll"
    if ($payloadSrc.Name -ne $expectName) {
        throw ("payload/source mismatch -- this tree declares $expectName (CMakeLists " +
               "VOTVCOOP_GAME_TARGET + protocol.h kProtocolVersion) but the newest artifact in " +
               "$BuildDir is $($payloadSrc.Name). Build this tree before deploying, or delete the " +
               "stale artifact. Refusing to ship code that is not the code you are looking at.")
    }
} else {
    Write-Host ("  WARN: could not read the declared identity from CMakeLists.txt / protocol.h -- " +
                "deploying $($payloadSrc.Name) on the build-number rule alone") -ForegroundColor Yellow
}

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

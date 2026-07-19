# deploy-loader.ps1 -- install the STANDALONE shipping loader into VOTV.
#
# Copies the xinput1_3.dll proxy + votv-coop.dll payload next to the shipping
# exe. On game start the proxy is loaded (VOTV imports XInputGetState/SetState
# from xinput1_3.dll, and the exe directory wins the DLL search order over
# System32); its two exports are forwarded to System32 xinput1_4.dll, and it
# loads votv-coop.dll automatically -- no injection, no UE4SS (RULE No.3).
#
# This REPLACES the dev-only inject.ps1 path. UE4SS (dwmapi.dll proxy) is left
# untouched; it coexists. Use -Standalone to also disable UE4SS for a clean
# no-UE4SS proof (renames dwmapi.dll <-> dwmapi.dll.off), and -Remove to undo.

[CmdletBinding()]
param(
    [string]$GameWin64 = "D:\Projects\Programming\VOTV_MP\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    [string]$BuildDir  = "D:\Projects\Programming\VOTV_MP\build\votv-coop\Release",
    [switch]$Standalone,  # also disable UE4SS (rename dwmapi.dll) for a clean proof
    [switch]$Remove       # uninstall the loader (and restore UE4SS)
)

$ErrorActionPreference = "Stop"
$proxy   = Join-Path $GameWin64 "xinput1_3.dll"
$marker  = Join-Path $GameWin64 "votv-coop-loaded.txt"
$dwm     = Join-Path $GameWin64 "dwmapi.dll"
$dwmOff  = Join-Path $GameWin64 "dwmapi.dll.off"

if ($Remove) {
    Remove-Item $proxy, $marker -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $GameWin64 "multivoid-*.dll") -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item (Join-Path $GameWin64 "votv-coop.dll") -ErrorAction SilentlyContinue  # legacy name
    if (Test-Path $dwmOff) { Move-Item $dwmOff $dwm -Force }   # re-enable UE4SS
    "loader removed; UE4SS restored if it was disabled."
    return
}

if (-not (Test-Path "$BuildDir\xinput1_3.dll")) { throw "build first: cmake --build build/votv-coop --config Release" }

# v122 (multivoid rename): the payload is the versioned multivoid-<game>-<build>.dll.
# Pick the newest one in the build dir (a proto bump renames the output; older
# artifacts may linger there). In the GAME dir, any OTHER multivoid-*.dll and the
# legacy votv-coop.dll are STALE VERSION FILES -- exactly what the proxy's
# duplicate-detector pops the in-game warning about -- so the deploy removes them.
$payloadSrc = Get-ChildItem (Join-Path $BuildDir "multivoid-*.dll") -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $payloadSrc) { throw "no multivoid-*.dll in $BuildDir -- build first" }
$payload = Join-Path $GameWin64 $payloadSrc.Name

# Idempotent copy: skip if the destination is already byte-identical to the source
# (e.g. a VOTV instance is running and holds the file locked, but we did NOT rebuild
# since the last deploy -- the lock is fine, the bytes already match). Without this
# guard a re-run of any mp_*.bat after a launch would fail with "file used by another
# process" even though there's nothing to actually copy.
#
# Hashing uses inline .NET SHA256 rather than Get-FileHash on purpose (2026-05-30):
# when this script is launched by mp.py's child powershell from a degraded
# environment (stripped PSModulePath / autoloading off), Get-FileHash --
# which lives in the auto-loaded Microsoft.PowerShell.Utility module -- fails to
# resolve ("not recognized as a cmdlet") and aborts the whole deploy (and the
# smoke that calls it). The .NET type has no module dependency, so the deploy is
# robust regardless of the ambient PowerShell host/environment.
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
        return  # already up-to-date
    }
    Copy-Item $src $dst -Force
}
Copy-IfChanged "$BuildDir\xinput1_3.dll" $proxy
Copy-IfChanged $payloadSrc.FullName $payload
# Stale-version hygiene: other multivoid-*.dll files + the legacy votv-coop.dll
# would trip the proxy's duplicate popup on next boot -- remove them (locked file
# = a running game; warn, the popup will tell the user too).
Get-ChildItem (Join-Path $GameWin64 "multivoid-*.dll") -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $payloadSrc.Name } | ForEach-Object {
        try { Remove-Item $_.FullName -Force -ErrorAction Stop; Write-Host "  removed STALE $($_.Name)" -ForegroundColor Yellow }
        catch { Write-Host "  WARN: stale $($_.Name) is LOCKED (game running?) -- delete before next launch" -ForegroundColor Red }
    }
$legacy = Join-Path $GameWin64 "votv-coop.dll"
if (Test-Path $legacy) {
    try { Remove-Item $legacy -Force -ErrorAction Stop; Write-Host "  removed LEGACY votv-coop.dll" -ForegroundColor Yellow }
    catch { Write-Host "  WARN: legacy votv-coop.dll is LOCKED (game running?) -- delete before next launch" -ForegroundColor Red }
}
Remove-Item $marker -ErrorAction SilentlyContinue

if ($Standalone -and (Test-Path $dwm)) { Move-Item $dwm $dwmOff -Force }  # disable UE4SS

"deployed loader to $GameWin64" + $(if ($Standalone) { " (UE4SS disabled)" } else { " (UE4SS left enabled)" })
Get-ChildItem $GameWin64 | Where-Object { $_.Name -match 'xinput1_3|multivoid|votv-coop|dwmapi|UE4SS\.dll' } |
    Select-Object Name, Length | Format-Table -AutoSize

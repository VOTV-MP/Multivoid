<#
.SYNOPSIS
    Install the UE4SS substrate into a VOTV game directory (reproducible setup).

.DESCRIPTION
    Downloads the pinned UE4SS build and places it beside
    VotV-Win64-Shipping.exe. Since WP-2 of the D-3 UE4SS migration
    (2026-08-22) UE4SS is the mod's LOADER on every game copy -- this script
    is the one-time per-copy substrate install; the per-build mod deploy is
    tools/deploy-mod.ps1 / deploy-all.ps1.

    INVARIANT: the installer owns substrate PRESENCE (dwmapi.dll, UE4SS.dll,
    the bundled Lua mods, the initial settings/mods.txt seed), NEVER standing
    settings state.
    - Extraction runs only when UE4SS.dll is absent (or -Force), via a staging
      dir, and NEVER overwrites an existing Mods\mods.txt or UE4SS-settings.ini
      (state files -- e.g. the dev copy's hand `coopTestHarness : 1` row).
    - dwmapi.dll is made present every run (un-parked from dwmapi.dll.off, or
      pulled from the pinned payload) -- a parked loader means the mod never
      loads.
    - Settings keys are written ONLY on a fresh seed, or when an explicit
      profile switch (-Quiet) is passed on THIS invocation.

    ADDITIVE only: places new files beside the shipping exe and modifies NO
    original game file (principle 1). The game install is gitignored, so this
    script is the committed source of truth for how the substrate is set up.

    UE4SS auto-detects the engine version (UE4.27) via AOB scanning; no
    EngineVersionOverride is needed for VOTV.

.NOTES
    WHY THIS PIN (measured 2026-08-31, docs/UE4SS_ARC.md section 9):

    The pinned payload is the UE4SS build carried by the Thunderstore package
    `Thunderstore-unreal_shimloader` -- Git SHA e31aaaa6, a 2026-05-07 snapshot
    of the upstream experimental channel. It replaced the v3.0.1 RELEASE build
    (Git SHA d935b5b, built 2024-02-14) because v3.0.1 is ~48 fps SLOWER on
    VOTV: a three-arm, PID-asserted experiment on one save / one mod build /
    one install measured old loader 70 fps vs this one 118 fps, with the
    de-confounding arm (old loader, same mod set) at 75 -- so the loader build
    is the cause, not the Lua mod set.

    It is also the build every mod-manager player already runs, which is the
    point: the manual lane and the r2modman lane now install the same bytes.

    Two upstream sources were rejected deliberately:
    - GitHub `experimental-latest` is a ROLLING tag (its assets are re-uploaded
      in place), so it is not a pin at all; its current build also moves the
      tree to `ue4ss/` with the proxy at the game root, which would ripple into
      deploy-mod.ps1 and the Thunderstore package shape.
    - GitHub `v3.0.1` is the slow arm above.

    A Thunderstore package version is IMMUTABLE (docs/THUNDERSTORE.md), so
    naming one is a true pin. Bump $Version deliberately; don't track a rolling
    channel (methodology: pin deps). The expected UE4SS.dll hash is pinned
    alongside it and the install FAILS CLOSED on a mismatch.

.EXAMPLE
    ./tools/install-ue4ss.ps1 -Win64Dir <...>\Win64 -Quiet
    # Play-install profile: GUI console off (no extra window per instance).

    ./tools/install-ue4ss.ps1 -Win64Dir <...>\Win64
    # Dev profile on a FRESH install: GUI console visible (Live View, dumps).

    ./tools/install-ue4ss.ps1 -Win64Dir <...>\Win64 -ZipPath D:\dl\shim.zip
    # Offline / blocked-CDN: install from a hand-downloaded package zip.
#>
[CmdletBinding()]
param(
    # Path to the folder containing VotV-Win64-Shipping.exe.
    [string]$Win64Dir = "$PSScriptRoot\..\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64",
    # Thunderstore `unreal_shimloader` package version -- it CARRIES the pinned
    # UE4SS build (see .NOTES). Immutable once published.
    [string]$Version  = "1.1.7",
    # Explicit local source zip, for an offline install or a blocked CDN.
    [string]$ZipPath,
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

# The pinned build's identity. The install fails CLOSED if the resolved payload
# does not carry exactly this UE4SS.dll -- a silently-different loader is the
# defect this pin exists to prevent (a rolling channel serves new bytes at an
# unchanged URL).
$expectedDllMd5 = '8A78269B9EC1704B708F43BD508F5D40'

$pkg     = "Thunderstore-unreal_shimloader-$Version"
$url     = "https://thunderstore.io/package/download/Thunderstore/unreal_shimloader/$Version/"
$staging = Join-Path $PSScriptRoot '..\build\ue4ss-staging'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
# Resolve NOW: $payload feeds a FullName.Substring($payload.Length+1) rel-path
# computation below, and an unresolved '..' in the prefix shifts the cut
# (measured 2026-08-22: every extracted rel lost its first 9 chars).
$staging = (Resolve-Path $staging).Path
$zip = Join-Path $staging "$pkg.zip"

function Get-Md5([string]$path) { (Get-FileHash $path -Algorithm MD5).Hash }

# Locate the directory holding UE4SS.dll inside an extracted package. The
# package nests the substrate under 'UE4SS\' beside shimloader's OWN proxy at
# the zip root -- that root dwmapi.dll is shimloader's and must NOT be
# installed; the one we want is UE4SS\dwmapi.dll, which lands flat in Win64.
function Find-PayloadRoot([string]$dir) {
    $hit = Get-ChildItem $dir -Recurse -File -Filter 'UE4SS.dll' | Select-Object -First 1
    if (-not $hit) { throw "No UE4SS.dll found under '$dir' -- not a UE4SS package?" }
    return $hit.Directory.FullName
}

# Source resolution, in order: explicit -ZipPath, then a mod manager's own
# package cache (if r2modman/TSMM already downloaded it, use those bytes --
# the hash gate below makes that safe), then the Thunderstore download.
function Resolve-Payload {
    if ($ZipPath) {
        if (-not (Test-Path $ZipPath)) { throw "-ZipPath '$ZipPath' not found." }
        $stage = Join-Path $staging "src_zip"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        Expand-Archive -Path $ZipPath -DestinationPath $stage -Force
        Write-Host "  source: -ZipPath $ZipPath" -ForegroundColor Cyan
        return (Find-PayloadRoot $stage)
    }

    $cacheGlobs = @(
        "$env:APPDATA\r2modmanPlus-local\*\cache\Thunderstore-unreal_shimloader\$Version",
        "$env:APPDATA\Thunderstore Mod Manager\DataFolder\*\cache\Thunderstore-unreal_shimloader\$Version"
    )
    foreach ($g in $cacheGlobs) {
        foreach ($d in (Get-ChildItem -Path $g -Directory -ErrorAction SilentlyContinue)) {
            $dll = Get-ChildItem $d.FullName -Recurse -File -Filter 'UE4SS.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($dll -and ((Get-Md5 $dll.FullName) -eq $expectedDllMd5)) {
                Write-Host "  source: mod-manager cache $($d.FullName)" -ForegroundColor Cyan
                return $dll.Directory.FullName
            }
        }
    }

    if (-not (Test-Path $zip)) {
        Write-Host "Downloading $pkg ..." -ForegroundColor Cyan
        try {
            Invoke-WebRequest -Uri $url -OutFile $zip
        } catch {
            throw ("Could not download $pkg from Thunderstore: $($_.Exception.Message)`n" +
                   "  The download redirects to gcdn.thunderstore.io, which some networks reset.`n" +
                   "  Fetch the zip by hand (or let r2modman download it once) and re-run with`n" +
                   "  -ZipPath <path-to-$pkg.zip>.")
        }
    }
    $stage = Join-Path $staging "extract_$Version"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $stage -Force
    Write-Host "  source: $url" -ForegroundColor Cyan
    return (Find-PayloadRoot $stage)
}

# Resolve + VERIFY once, lazily: only the paths that actually need bytes call
# this, so a no-op run does no network I/O.
$script:payloadDir = $null
function Get-Payload {
    if ($script:payloadDir) { return $script:payloadDir }
    $p = Resolve-Payload
    $got = Get-Md5 (Join-Path $p 'UE4SS.dll')
    if ($got -ne $expectedDllMd5) {
        throw ("UE4SS.dll hash mismatch -- REFUSING to install.`n" +
               "  expected $expectedDllMd5 (the pinned build, Git SHA e31aaaa6)`n" +
               "  got      $got`n" +
               "  Source: $p")
    }
    Write-Host "  payload verified: UE4SS.dll md5 $got" -ForegroundColor DarkGray
    $script:payloadDir = $p
    return $p
}

# --- 1. Extraction (only when the substrate is absent, or -Force) ------------
$freshSeed = $false
if ((-not (Test-Path $ue4ssDll)) -or $Force) {
    $payload = Get-Payload
    # Copy over, preserving existing STATE files (mods.txt, UE4SS-settings.ini):
    # a re-extract must never clobber hand-edited mod enable rows or a tuned
    # settings profile. The bundled Lua mods are PAYLOAD, not state -- they are
    # version-locked to the DLL's Lua API and are refreshed with it (running a
    # 2024 mod set against this loader is what silently drops mods, measured
    # 2026-08-31).
    $stateFiles = @('Mods\mods.txt', 'UE4SS-settings.ini')
    Get-ChildItem $payload -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($payload.Length + 1)
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
    Write-Host "UE4SS ($pkg) extracted into $Win64Dir" -ForegroundColor Green
} else {
    Write-Host "UE4SS already present at $Win64Dir (use -Force to re-extract)." -ForegroundColor DarkGray
    $have = Get-Md5 $ue4ssDll
    if ($have -ne $expectedDllMd5) {
        Write-Host ("  WARNING: installed UE4SS.dll is md5 $have, NOT the pinned $expectedDllMd5.`n" +
                    "           Re-run with -Force to install the pinned build.") -ForegroundColor Yellow
    }
}

# --- 2. Substrate-whole: the loader chain must be complete every run ---------
if (-not (Test-Path $dwm)) {
    if (Test-Path $dwmOff) {
        Move-Item $dwmOff $dwm -Force
        Write-Host "  dwmapi.dll un-parked (was $((Split-Path -Leaf $dwmOff)))" -ForegroundColor Yellow
    } elseif (Test-Path $ue4ssDll) {
        # UE4SS.dll present but its loader proxy missing: pull dwmapi from the
        # pinned payload.
        Copy-Item (Join-Path (Get-Payload) 'dwmapi.dll') $dwm -Force
        Write-Host "  dwmapi.dll restored from the pinned payload" -ForegroundColor Yellow
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
$dllMd5 = if (Test-Path $ue4ssDll) { Get-Md5 $ue4ssDll } else { 'ABSENT' }
Write-Host ("substrate: UE4SS.dll=" + (Test-Path $ue4ssDll) + " dwmapi.dll=" + (Test-Path $dwm) + " [$gc]") -ForegroundColor Green
Write-Host ("substrate: UE4SS.dll md5=$dllMd5 (pin $pkg)") -ForegroundColor Green
Write-Host "Dev shortcuts once in game (dev profile): CTRL+H headers, CTRL+J object dump." -ForegroundColor DarkGray

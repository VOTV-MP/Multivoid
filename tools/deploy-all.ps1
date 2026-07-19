# deploy-all.ps1 -- deploy the standalone loader to all four game copies.
#
# 2026-05-25 (3-copy convention; see docs/RE_WORKFLOW.md):
#   Game_0.9.0n_HOST/     -- HOST    (user's hands-on host play)
#   Game_0.9.0n_CLIENT_1/ -- CLIENT  (user's hands-on client play)
#   Game_0.9.0n_CLIENT_3/  -- DEV     (Claude's autonomous LAN test + RE work)
# 2026-05-28 PR-4.2+ (4-copy convention -- 3-peer LAN multi-client tests):
#   Game_0.9.0n_CLIENT_2/ -- CLIENT2 (second hands-on client for 3-peer test)
#
# Each gets the same xinput1_3.dll proxy + multivoid-<game>-<build>.dll payload
# (the Paper-style versioned artifact, 2026-07-19). The dev
# copy ADDITIONALLY has UE4SS installed via the dwmapi.dll alternate proxy
# slot (UE4SS coexists with our standalone DLL there; the user-play copies
# stay UE4SS-free per RULE 3).
#
# Each copy keeps its OWN Saved/ directory (logs, screenshots, save games)
# so the autonomous LAN test in _dev/ cannot collide with the user's host
# or client play state. See tools/lan-test.ps1 which now uses _dev/.
#
# Usage:
#   .\tools\deploy-all.ps1                 # deploy current build to all 3
#   .\tools\deploy-all.ps1 -Standalone     # ALSO disable UE4SS in dev copy
#                                            (renames dwmapi.dll, for a "what
#                                            the user gets" simulation)
#   .\tools\deploy-all.ps1 -Remove         # uninstall loader from all 3

[CmdletBinding()]
param(
    [switch]$Standalone,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build\votv-coop\Release"

$targets = @(
    @{Name="HOST"   ; Path=Join-Path $root "Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64"},
    @{Name="CLIENT" ; Path=Join-Path $root "Game_0.9.0n_CLIENT_1\WindowsNoEditor\VotV\Binaries\Win64"},
    @{Name="CLIENT2"; Path=Join-Path $root "Game_0.9.0n_CLIENT_2\WindowsNoEditor\VotV\Binaries\Win64"},
    @{Name="DEV"    ; Path=Join-Path $root "Game_0.9.0n_CLIENT_3\WindowsNoEditor\VotV\Binaries\Win64"}
)

$deployScript = Join-Path $PSScriptRoot "deploy-loader.ps1"

# Custom client-puppet mesh pak (docs/COOP_CLIENT_MODEL.md). Shipped to EVERY
# peer (client-side visual asset); UE4 auto-mounts any .pak under Content/Paks/
# at startup, so dropping it under Content/Paks/LogicMods/votv-coop/ makes the
# mesh resident before our boot thread runs. Optional: absent pak == puppets keep
# the kel skin (graceful-degrade in coop::client_model).
# 2026-07-02: model renamed to its ORIGINAL name -- scientist.pak -> hl_einstein_v1sc.pak.
$clientPak = Join-Path $root "research\pak_re\hl_einstein_v1sc.pak"

foreach ($t in $targets) {
    if (-not (Test-Path $t.Path)) {
        Write-Host "[deploy-all] SKIP $($t.Name): path does not exist -- $($t.Path)" -ForegroundColor Yellow
        continue
    }
    Write-Host "[deploy-all] === $($t.Name) === $($t.Path)" -ForegroundColor Cyan
    $args = @{GameWin64=$t.Path; BuildDir=$buildDir}
    # -Standalone applies to the dev copy only (it has UE4SS to disable);
    # the user-play copies don't have UE4SS to begin with.
    if ($Standalone -and $t.Name -eq "DEV") { $args.Standalone = $true }
    if ($Remove) { $args.Remove = $true }
    & $deployScript @args

    # Deploy (or, with -Remove, remove) the client-model pak. $t.Path is
    # ...\VotV\Binaries\Win64; the pak lives under ...\VotV\Content\Paks\LogicMods.
    $votvDir = Split-Path -Parent (Split-Path -Parent $t.Path)   # Win64 -> Binaries -> VotV
    $pakDir  = Join-Path $votvDir "Content\Paks\LogicMods\votv-coop"
    $pakDest = Join-Path $pakDir "hl_einstein_v1sc.pak"
    # One-time hygiene: the pre-rename deliverable must not stay mounted alongside the
    # renamed one (two paks with the same package content = double-mount ambiguity).
    $stalePak = Join-Path $pakDir "scientist.pak"
    if (-not $Remove -and (Test-Path $stalePak)) {
        try { Remove-Item $stalePak -Force -ErrorAction Stop; Write-Host "  removed STALE scientist.pak" -ForegroundColor Yellow }
        catch { Write-Host "  WARN: stale scientist.pak is LOCKED (game running?) -- remove it before the next launch" -ForegroundColor Red }
    }
    if ($Remove) {
        if (Test-Path $pakDest) { Remove-Item $pakDest -Force; Write-Host "  removed client pak" -ForegroundColor DarkGray }
    } elseif (Test-Path $clientPak) {
        if (-not (Test-Path $pakDir)) { New-Item -ItemType Directory -Force -Path $pakDir | Out-Null }
        # Idempotent (same pattern as deploy-loader's DLL copy): a RUNNING game
        # holds its pak mapped, so Copy-Item throws IOException even when there is
        # nothing to update -- which aborted the whole deploy + the client launch
        # while the HOST was up (2026-07-02). Reads are share-allowed: hash-compare
        # and skip when byte-identical. A genuinely STALE pak under a running game
        # still fails loudly (correct -- a mapped pak cannot be hot-swapped).
        # Inline .NET SHA256 (NOT Get-FileHash): under mp.py's nested/degraded
        # PowerShell the Utility module fails to autoload and Get-FileHash aborts
        # the whole deploy (same rationale as deploy-loader.ps1's helper).
        function Get-Sha256HexLocal($path) {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { return [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($path))).Replace('-', '') }
            finally { $sha.Dispose() }
        }
        $same = (Test-Path $pakDest) -and
                ((Get-Sha256HexLocal $pakDest) -eq (Get-Sha256HexLocal $clientPak))
        if ($same) {
            Write-Host "  client pak up-to-date (skip)" -ForegroundColor DarkGray
        } else {
            Copy-Item $clientPak $pakDest -Force
            Write-Host "  client pak -> $pakDest" -ForegroundColor DarkGray
        }
        # v93 skins: ship the preview sidecar (<name>.png next to the pak -- the F1
        # browser tile). Plain file, never mapped by the game -> simple copy.
        $prevSrc = [IO.Path]::ChangeExtension($clientPak, "png")
        if (Test-Path $prevSrc) {
            Copy-Item $prevSrc (Join-Path $pakDir ([IO.Path]::GetFileName($prevSrc))) -Force
            Write-Host "  client pak preview png -> $pakDir" -ForegroundColor DarkGray
        }
    } else {
        Write-Host "  SKIP client pak: source missing ($clientPak)" -ForegroundColor Yellow
    }
}

Write-Host "[deploy-all] done." -ForegroundColor Green

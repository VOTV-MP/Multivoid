# deploy-all.ps1 -- deploy the Multivoid mod folder to all four game copies.
#
# 2026-05-25 (3-copy convention; see docs/RE_WORKFLOW.md):
#   Game_0.9.0n_HOST/     -- HOST    (user's hands-on host play)
#   Game_0.9.0n_CLIENT_1/ -- CLIENT  (user's hands-on client play)
#   Game_0.9.0n_CLIENT_3/  -- DEV     (Claude's autonomous LAN test + RE work)
# 2026-05-28 PR-4.2+ (4-copy convention -- 3-peer LAN multi-client tests):
#   Game_0.9.0n_CLIENT_2/ -- CLIENT2 (second hands-on client for 3-peer test)
#
# WP-2 of the D-3 UE4SS migration (2026-08-22): every copy runs the UE4SS lane
# -- the mod is Mods\Multivoid\dlls\main.dll + enabled.txt, deployed per copy
# by tools/deploy-mod.ps1 (which also removes the retired xinput-proxy files).
# The UE4SS substrate (dwmapi.dll + UE4SS.dll + settings) is a ONE-TIME per-copy
# install owned by tools/install-ue4ss.ps1; this script never touches it.
#
# Each copy keeps its OWN Saved/ directory (logs, screenshots, save games)
# so the autonomous LAN test cannot collide with the user's host or client
# play state.
#
# Usage:
#   .\tools\deploy-all.ps1                 # deploy current build to all 4
#   .\tools\deploy-all.ps1 -Remove         # remove the mod folder from all 4

[CmdletBinding()]
param(
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

$deployScript = Join-Path $PSScriptRoot "deploy-mod.ps1"

# Custom client-puppet mesh pak (docs/COOP_CLIENT_MODEL.md). Shipped to EVERY
# peer (client-side visual asset); UE4 auto-mounts any .pak under Content/Paks/
# at startup, so dropping it under Content/Paks/LogicMods/multivoid/ makes the
# mesh resident before our boot thread runs. Optional: absent pak == puppets keep
# the kel skin (graceful-degrade in coop::client_model).
# THE SOURCE IS assets/paks -- THE SAME DIRECTORY THE RELEASE PACKAGES, and that is the
# whole point: a dev rig that carries different skins from a player's install cannot
# reproduce a player's bug. This used to point at research\pak_re\hl_einstein_v1sc.pak, a
# leftover of the pre-2026-08-29 skin lane, so every deploy re-installed a pak the release
# has not shipped since -- and never installed scientists.pak at all. It also confounded a
# measurement on 2026-09-01: a skin resolved on the dev rigs ONLY because this line kept
# putting it back, on a run investigating why it did not resolve for the user.
$pakSrcDir = Join-Path $root "assets\paks"

foreach ($t in $targets) {
    if (-not (Test-Path $t.Path)) {
        Write-Host "[deploy-all] SKIP $($t.Name): path does not exist -- $($t.Path)" -ForegroundColor Yellow
        continue
    }
    Write-Host "[deploy-all] === $($t.Name) === $($t.Path)" -ForegroundColor Cyan
    $args = @{GameWin64=$t.Path; BuildDir=$buildDir}
    if ($Remove) { $args.Remove = $true }
    & $deployScript @args

    # Deploy (or, with -Remove, remove) the client-model pak. $t.Path is
    # ...\VotV\Binaries\Win64; the pak lives under ...\VotV\Content\Paks\LogicMods.
    $votvDir = Split-Path -Parent (Split-Path -Parent $t.Path)   # Win64 -> Binaries -> VotV
    $pakDir  = Join-Path $votvDir "Content\Paks\LogicMods\multivoid"
    if ($Remove) {
        if (Test-Path $pakDir) { Remove-Item $pakDir -Recurse -Force; Write-Host "  removed client paks" -ForegroundColor DarkGray }
    } elseif (Test-Path $pakSrcDir) {
        if (-not (Test-Path $pakDir)) { New-Item -ItemType Directory -Force -Path $pakDir | Out-Null }
        # Inline .NET SHA256 (NOT Get-FileHash): under mp.py's nested/degraded PowerShell the
        # Utility module fails to autoload and Get-FileHash aborts the whole deploy.
        function Get-Sha256HexLocal($path) {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { return [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($path))).Replace('-', '') }
            finally { $sha.Dispose() }
        }
        # A MIRROR, NOT AN ACCUMULATION. Copying without pruning is why a pak retired on
        # 2026-08-29 was still mounted on every dev rig days later: nothing ever removed what
        # we stopped shipping, so the rigs drifted from the release and kept resolving a skin
        # a player could not have. Pruning is safe HERE and only here -- this subfolder is
        # ours; other mods' paks live one level up in LogicMods and are never touched.
        $want = @{}
        Get-ChildItem $pakSrcDir -File | Where-Object { $_.Extension -in '.pak', '.png', '.bmp' } | ForEach-Object {
            $want[$_.Name] = $true
            $dst = Join-Path $pakDir $_.Name
            # Idempotent: a RUNNING game holds its pak mapped, so an unconditional copy throws
            # even when there is nothing to update -- which aborted the whole deploy while the
            # host was up (2026-07-02). Reads are share-allowed, so hash-compare and skip.
            if ((Test-Path $dst) -and ((Get-Sha256HexLocal $dst) -eq (Get-Sha256HexLocal $_.FullName))) { return }
            Copy-Item $_.FullName $dst -Force
            Write-Host "  client pak -> $($_.Name)" -ForegroundColor DarkGray
        }
        Get-ChildItem $pakDir -File -ErrorAction SilentlyContinue | Where-Object { -not $want.ContainsKey($_.Name) } | ForEach-Object {
            try { Remove-Item $_.FullName -Force -ErrorAction Stop; Write-Host "  pruned retired $($_.Name)" -ForegroundColor Yellow }
            catch { Write-Host "  WARN: retired $($_.Name) is LOCKED (game running?) -- delete before next launch" -ForegroundColor Red }
        }
    } else {
        Write-Host "  SKIP client paks: source missing ($pakSrcDir)" -ForegroundColor Yellow
    }
}

Write-Host "[deploy-all] done." -ForegroundColor Green

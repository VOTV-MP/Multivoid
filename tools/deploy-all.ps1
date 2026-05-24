# deploy-all.ps1 -- deploy the standalone loader to all three game copies.
#
# 2026-05-25 (3-copy convention; see docs/RE_WORKFLOW.md):
#   Game_0.9.0n/     -- HOST  (user's hands-on host play)
#   Game_0.9.0n_copy/ -- CLIENT (user's hands-on client play)
#   Game_0.9.0n_dev/  -- DEV    (Claude's autonomous LAN test + RE work)
#
# Each gets the same xinput1_3.dll proxy + votv-coop.dll payload. The dev
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
    @{Name="HOST"  ; Path=Join-Path $root "Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64"},
    @{Name="CLIENT"; Path=Join-Path $root "Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64"},
    @{Name="DEV"   ; Path=Join-Path $root "Game_0.9.0n_dev\WindowsNoEditor\VotV\Binaries\Win64"}
)

$deployScript = Join-Path $PSScriptRoot "deploy-loader.ps1"

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
}

Write-Host "[deploy-all] done." -ForegroundColor Green

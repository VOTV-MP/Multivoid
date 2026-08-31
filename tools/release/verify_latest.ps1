# verify_latest.ps1 -- the STABLE-ritual closing check (design D3): after the
# master env constants are updated, /v1/latest must report exactly the newest
# published stable. Fold-aware (R23): "the newest BARE-tag row whose state(N)
# == PUBLISHED" -- a retracted N has a published row too; the terminal closes
# it. Drilled to FAIL pre-env-step and PASS post-env-step at the first real
# stable ritual (deferred drill, R22).

param(
    [string]$LedgerPath = (Join-Path $PSScriptRoot 'LEDGER.tsv'),
    [string]$MasterLatestUrl = '',  # default: derived from kOfficialMasterUrl in protocol.h
    # Pass when the master's COOP_LATEST_* was deliberately pointed at a DEV
    # prerelease. Without it this script asserts the stable contract above and
    # would call a dev-advertising master an "unrecorded release" -- a false
    # accusation, which is worse than no check. See Get-NewestPublished.
    [switch]$AllowDev
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

if (-not $MasterLatestUrl) {
    # ONE definition of the master endpoint (protocol.h); schemeless = TLS.
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $hdr = Get-Content (Join-Path $repoRoot 'src/votv-coop/include/coop/net/protocol.h') -Raw
    $m = [regex]::Match($hdr, 'kOfficialMasterUrl\s*=\s*"([^"]+)"')
    if (-not $m.Success) { throw 'kOfficialMasterUrl not found in protocol.h' }
    $MasterLatestUrl = "https://$($m.Groups[1].Value)/v1/latest"
}

$kind = if ($AllowDev) { 'published (dev admitted)' } else { 'published stable' }
$expected = Get-NewestPublished -Rows (Read-Ledger -Path $LedgerPath).Rows -IncludeDev:$AllowDev

Write-Host "verify_latest: querying $MasterLatestUrl"
$resp = $null
try { $resp = Invoke-RestMethod -Uri $MasterLatestUrl -TimeoutSec 15 } catch {
    Write-Host "verify_latest: FAIL -- master unreachable: $($_.Exception.Message)"
    exit 1
}
$masterProto = 0
if ($resp -and ($resp.PSObject.Properties.Name -contains 'proto')) { $masterProto = [int]$resp.proto }

if ($null -eq $expected) {
    if ($masterProto -le 0) {
        Write-Host "verify_latest: OK_EMPTY -- no $kind in the ledger; master has no released record (proto<=0)"
        exit 0
    }
    $hint = if ($AllowDev) { '' } else { ' -- if the master was deliberately pointed at a DEV prerelease, re-run with -AllowDev' }
    Write-Host "verify_latest: FAIL -- master reports proto=$masterProto but the ledger has NO $kind (unrecorded release?)$hint"
    exit 1
}

Write-Host "verify_latest: ledger newest $kind = N=$($expected.N) game=$($expected.Game) tag=$($expected.TagName)"
Write-Host "verify_latest: master reports proto=$masterProto mod='$($resp.mod)'"
if ($masterProto -ne $expected.N) {
    Write-Host "verify_latest: FAIL -- master proto $masterProto != ledger N $($expected.N) from the newest $kind (env constants not updated / stale?)"
    exit 1
}
Write-Host 'verify_latest: PASS'
exit 0

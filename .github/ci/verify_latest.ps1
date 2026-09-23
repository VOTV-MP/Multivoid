# verify_latest.ps1 -- the STABLE-ritual closing check (design D3): after every master's env
# constants are updated, each master's /v1/latest must report exactly the newest published stable.
# The masters are the official slots in protocol.h (kOfficialMasterSlots): a player reads the update
# check from the master they chose, so one master left behind tells its players a wrong verdict.
# Fold-aware (R23): "the newest BARE-tag row whose state(N) == PUBLISHED" -- a retracted N has a
# published row too; the terminal closes it. Drilled to FAIL pre-env-step and PASS post-env-step at
# the first real stable ritual (deferred drill, R22).

param(
    [string]$LedgerPath = (Join-Path $PSScriptRoot 'LEDGER.tsv'),
    # Default: every slot of kOfficialMasterSlots in protocol.h.
    [string[]]$MasterLatestUrls = @(),
    # Pass when the masters' COOP_LATEST_* was deliberately pointed at a DEV
    # prerelease. Without it this script asserts the stable contract above and
    # would call a dev-advertising master an "unrecorded release" -- a false
    # accusation, which is worse than no check. See Get-NewestPublished.
    [switch]$AllowDev
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

if ($MasterLatestUrls.Count -eq 0) {
    # ONE definition of the masters (protocol.h): comma-separated label=address slots, and a bare
    # host:port means TLS, as the client's own grammar says.
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $hdr = Get-Content (Join-Path $repoRoot 'src/votv-coop/include/coop/net/protocol.h') -Raw
    $m = [regex]::Match($hdr, 'kOfficialMasterSlots\s*=\s*"([^"]+)"')
    if (-not $m.Success) { throw 'kOfficialMasterSlots not found in protocol.h' }
    $MasterLatestUrls = @(foreach ($slot in $m.Groups[1].Value.Split(',')) {
        $address = $slot.Substring($slot.IndexOf('=') + 1).Trim()
        if ($address -match '^[a-z]+://') { "$address/v1/latest" } else { "https://$address/v1/latest" }
    })
}

$kind = if ($AllowDev) { 'published (dev admitted)' } else { 'published stable' }
$expected = Get-NewestPublished -Rows (Read-Ledger -Path $LedgerPath).Rows -IncludeDev:$AllowDev
if ($null -ne $expected) {
    Write-Host "verify_latest: ledger newest $kind = N=$($expected.N) game=$($expected.Game) tag=$($expected.TagName)"
}

$failed = 0
foreach ($url in $MasterLatestUrls) {
    Write-Host "verify_latest: querying $url"
    $resp = $null
    try { $resp = Invoke-RestMethod -Uri $url -TimeoutSec 15 } catch {
        Write-Host "verify_latest: FAIL -- $url unreachable: $($_.Exception.Message)"
        $failed++
        continue
    }
    $masterProto = 0
    if ($resp -and ($resp.PSObject.Properties.Name -contains 'proto')) { $masterProto = [int]$resp.proto }

    if ($null -eq $expected) {
        if ($masterProto -le 0) {
            Write-Host "verify_latest: OK_EMPTY -- no $kind in the ledger; $url has no released record (proto<=0)"
            continue
        }
        $hint = if ($AllowDev) { '' } else { ' -- if the masters were deliberately pointed at a DEV prerelease, re-run with -AllowDev' }
        Write-Host "verify_latest: FAIL -- $url reports proto=$masterProto but the ledger has NO $kind (unrecorded release?)$hint"
        $failed++
        continue
    }

    Write-Host "verify_latest: $url reports proto=$masterProto mod='$($resp.mod)'"
    if ($masterProto -ne $expected.N) {
        Write-Host "verify_latest: FAIL -- $url proto $masterProto != ledger N $($expected.N) from the newest $kind (env constants not updated / stale?)"
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Host "verify_latest: FAIL -- $failed of $($MasterLatestUrls.Count) master(s)"
    exit 1
}
Write-Host "verify_latest: PASS ($($MasterLatestUrls.Count) master(s))"
exit 0

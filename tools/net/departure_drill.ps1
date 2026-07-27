# tools/net/departure_drill.ps1 -- the arc-A DEPARTURE drill; the case the
# 4-peer smoke does NOT cover.
#
# Lives in tools/ rather than a scratchpad because a scratchpad is per-session
# and this instrument is the only thing that exercises the arc's central claim.
# It drives tools/mp.py (tracked, but its local edits are never committed).
#
# smoke4 only ever ADDS peers. Arc A's whole claim is about the other direction:
# a peer LEAVING must (a) fire one ledger row transition on the host, (b) reach
# the OTHER CLIENTS as a playerNo=0 roster row -- which is the only way a
# departure can reach them, there being no PlayerLeft kind -- and (c) run the
# per-person teardown fan-out ON A CLIENT, where the old IsSlotReady edge never
# fired at all.
#
# Choreography: let smoke4 bring up host + 3 clients, wait until the host's
# ledger shows slot 3 occupied, kill ONE client's process, let the session run
# on, then read what the survivors logged.

$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Programming\VOTV_MP'
$hostLog = Join-Path $root 'Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log'
# Baseline the log length: this file is APPENDED across runs, so matching on its
# whole contents finds the PREVIOUS run's roster and fires the drill before any
# peer exists (measured: that is exactly what happened on the first attempt).
$baseLines = 0
if (Test-Path $hostLog) { $baseLines = (Get-Content -LiteralPath $hostLog | Measure-Object -Line).Lines }
Write-Host "[drill] host log baseline = $baseLines lines"

Write-Host '[drill] launching smoke4 in the background...'
$smoke = Start-Process -FilePath 'python' -ArgumentList 'tools/mp.py','smoke4','--duration','180' `
    -WorkingDirectory $root -PassThru -WindowStyle Minimized

# Wait for all FOUR peers to be up. Deliberately NOT by parsing the host log:
# multivoid.log is ROTATED at boot (multivoid.prev.log), so a line-count baseline
# can never be exceeded and the wait hangs forever -- measured on attempt 2. The
# process set is the honest trigger; the log is for the VERDICT, not the cue.
$deadline = (Get-Date).AddMinutes(6)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $ps = @(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue)
    if ($ps.Count -ge 4) { $ready = $true; break }
}
if (-not $ready) {
    Write-Host '[drill] FAIL -- four peers never came up; nothing to test.'
    try { Stop-Process -Id $smoke.Id -Force } catch {}
    exit 1
}
# Settle: all three clients must finish save transfer + world load + join before a
# departure means anything. smoke4's own monitor window is widened to 180 s
# (--duration) so the peers are still alive when we kill one -- measured on
# attempt 3: the default window expired and smoke4 killed everything during the
# settle, leaving nothing to test.
Write-Host '[drill] four processes up; settling 60 s for world-ready + full roster.'
Start-Sleep -Seconds 60

# Kill exactly one client process (the highest-PID VotV client = the last joiner).
$clients = Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -like '*Client*' } | Sort-Object Id
if ($clients.Count -lt 2) {
    Write-Host "[drill] FAIL -- expected >=2 client processes, saw $($clients.Count)"
    try { Stop-Process -Id $smoke.Id -Force } catch {}
    exit 1
}
$victim = $clients[-1]
Write-Host "[drill] killing victim PID=$($victim.Id) title='$($victim.MainWindowTitle)'"
$killAt = Get-Date
Stop-Process -Id $victim.Id -Force

# Let the departure propagate: host reconcile (per tick) -> row transition ->
# roster pulse (fast window ~1 s) -> the surviving clients' apply.
Start-Sleep -Seconds 30
Write-Host "[drill] departure window elapsed; harvesting logs."

$logs = @{
    HOST    = $hostLog
    CLIENT1 = Join-Path $root 'Game_0.9.0n_CLIENT_1\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log'
    CLIENT2 = Join-Path $root 'Game_0.9.0n_CLIENT_2\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log'
    CLIENT3 = Join-Path $root 'Game_0.9.0n_CLIENT_3\WindowsNoEditor\VotV\Binaries\Win64\multivoid.log'
}
foreach ($k in 'HOST','CLIENT1','CLIENT2','CLIENT3') {
    $p = $logs[$k]
    Write-Host ''
    Write-Host "========== $k =========="
    if (-not (Test-Path $p)) { Write-Host '  (no log)'; continue }
    $lines = Get-Content -LiteralPath $p -Tail 20000
    $hit = $lines | Select-String -Pattern 'ledger: slot|left the game|puppet destroyed|OnPeerLeft|released .* claim|retired .* proxy mirror|drained .* wire prop mirror|destroyed .* mirror'
    if ($hit) { $hit | ForEach-Object { Write-Host "  $_" } } else { Write-Host '  (no departure-related lines)' }
}

Write-Host ''
Write-Host '[drill] done -- letting smoke4 finish its own teardown.'
try { Wait-Process -Id $smoke.Id -Timeout 300 } catch {}

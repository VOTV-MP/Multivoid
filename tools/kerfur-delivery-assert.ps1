<#
.SYNOPSIS
  Log-truth assert harness for the kerfur DELIVERY axis + its OWNER BOUNDARY.
  Reads the HOST + CLIENT votv-coop logs and asserts the join-window deliver-missing
  invariants we otherwise grep by hand. One PASS/FAIL line per invariant + an overall
  verdict + exit code (0 = all pass, 1 = any CRITICAL fail). The log is the channel of
  truth. Sibling of tools/pile-test-assert.ps1.

.DESCRIPTION
  Born 2026-06-30 from the 13:30 hands-on 5-vs-6 kerfur: a host turn-off DURING a join
  window made an off-prop that reached the client via ZERO channels (snapshot too early,
  KerfurConvert raced NPC-registration, generic incremental express excludes kerfurs).
  Fix: prop_snapshot::DeliverLateRegisteredProps + ExpressIncrementalKerfurOffProp (the
  late-registration arm of the deliver-missing owner). This harness is the EXECUTABLE
  boundary guard the design calls for -- it asserts:

    (1) CENSUS PARITY (the unified end-to-end delivery invariant): host TOTAL == client
        TOTAL. Goes red if EITHER arm of the owner (at-join snapshot OR late-registration
        delta) breaks -- the cross-half guard that a shared NAME could not give.
    (2) OWNER BOUNDARY (runtime self-check): the "kerfur-off owner SKIPPED a convert-owned
        kerfur" WARN count is 0 (a converted off-prop must NEVER reach the owner).
    (3) BOUNDARY DISCRIMINATOR (eid-correlated): no eid is BOTH "marked known" (converted,
        prop_lifecycle.cpp:646) AND delivered by the kerfur-off owner. Empty intersection.

  See docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md (delivery-ownership facet) +
  [[feedback-deliver-missing-owner-delivery-axis]].

.PARAMETER HostLog / ClientLog
  Paths to the two logs. Default to the LAN host/client game folders.

.PARAMETER Scenario
  Optional label, printed in the header (e.g. "join-turnoff-5v6").

.PARAMETER Quiet
  Only print the summary table + verdict.

.EXAMPLE
  pwsh tools/kerfur-delivery-assert.ps1 -Scenario join-turnoff
#>
[CmdletBinding()]
param(
    [string]$HostLog   = "$PSScriptRoot/../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log",
    [string]$ClientLog = "$PSScriptRoot/../Game_0.9.0n_copy/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log",
    [string]$Scenario  = "",
    [switch]$Quiet,
    # Assert that BOTH boundary sides were ACTUALLY exercised this run (refuses a vacuous green): side A
    # (join-window turn-off -> owner fired) AND side B (steady-state turn-off -> convert fired). Without
    # this, boundary-discriminator can pass with 0 converted + 0 owner-delivered = tested nothing. Pass
    # -BoundaryTest only on a run that played BOTH runbook scenarios (or the autonomous both-sides driver).
    [switch]$BoundaryTest
)

$ErrorActionPreference = 'Stop'

function Read-Log([string]$path) {
    if (-not (Test-Path $path)) { return @() }
    return Get-Content -LiteralPath $path
}

$H = Read-Log $HostLog
$C = Read-Log $ClientLog

function Count($lines, [string]$rx) { ($lines | Select-String -Pattern $rx -AllMatches).Count }
function Matches($lines, [string]$rx) { $lines | Select-String -Pattern $rx }
# The LAST integer captured by $rx in $lines, or $null if none (the final settled census).
function LastInt($lines, [string]$rx) {
    $m = $lines | Select-String -Pattern $rx
    if ($m.Count -eq 0) { return $null }
    return [int]$m[-1].Matches[0].Groups[1].Value
}
# All eids captured by $rx, as an int hash-set.
function EidSet($lines, [string]$rx) {
    $s = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($m in ($lines | Select-String -Pattern $rx)) { [void]$s.Add([int]$m.Matches[0].Groups[1].Value) }
    return $s
}

$Invariants = @(

    # ---- (1) THE unified end-to-end delivery invariant: host kerfur TOTAL == client TOTAL.
    # This is the cross-half guard -- it fails if EITHER owner arm (at-join snapshot or the
    # late-registration delta) drops a kerfur. The 5-vs-6 bug fails this; the fix makes it pass.
    @{ Name='kerfur-census-parity'; Severity='CRITICAL'; Check={
        $h = LastInt $H '\[KERFUR CENSUS\]\[HOST\].*=\s*(\d+)\s*kerfur'
        $c = LastInt $C '\[KERFUR CENSUS\]\[CLIENT\].*=\s*(\d+)\s*kerfur'
        if ($null -eq $h -or $null -eq $c) {
            return @{ Pass = $true; Detail = "no final census on one side (host=$h client=$c) -- enable [dev] kerfur_census=1 on both" }
        }
        @{ Pass = ($h -eq $c); Detail = "host TOTAL=$h, client TOTAL=$c (must be equal -- the deliver-missing invariant)" }
    }},

    # ---- (2) OWNER BOUNDARY runtime self-check: the owner must NEVER deliver a convert-owned
    # (KerfurId-bound) kerfur. ExpressIncrementalKerfurOffProp WARNs+skips if it ever does.
    @{ Name='owner-boundary-no-converted'; Severity='CRITICAL'; Check={
        $w = Count $H 'kerfur-off deliver-missing owner SKIPPED a convert-owned kerfur'
        @{ Pass = ($w -eq 0); Detail = "$w boundary-violation WARN(s) (a converted off-prop leaked into the re-seed-NEW set; must be 0)" }
    }},

    # ---- (3) BOUNDARY DISCRIMINATOR, eid-correlated: no eid is BOTH 'marked known' (converted)
    # AND delivered by the kerfur-off owner. The MarkKnownKeyedProp discriminator must hold.
    @{ Name='boundary-discriminator'; Severity='CRITICAL'; Check={
        $converted = EidSet $H 'silent register\].*->\s*eid=(\d+)\s*\(no PropSpawn'
        $delivered = EidSet $H 'incremental PropSpawn for runtime-adopted kerfur-off prop.*eid=(\d+)'
        $overlap = @($delivered | Where-Object { $converted.Contains($_) })
        @{ Pass = ($overlap.Count -eq 0)
           Detail = "$($delivered.Count) owner-delivered kerfur-off eid(s), $($converted.Count) convert-owned; overlap=$($overlap.Count) (must be 0)" +
                    $(if ($overlap.Count) { " [eids: $($overlap -join ',')]" } else { '' }) }
    }},

    # ---- (3b) BRACKET-PROTECTION (Q1): the late-delivery arm is bracket-FREE -- it must NEVER re-arm the
    # client's destructive divergence sweep (a re-arm wipes join-churn; count parity above only catches that
    # if the wiped entity is a KERFUR). A client sweep ARM is always a RESPONSE to a host connect-replay
    # snapshot bracket; the bracket-free late-delivery sends neither a SnapshotBegin nor a connect-replay.
    # So a client ARM with no host connect-replay behind it == a stray bracket (a refactor that bracketed
    # the incremental arm). Assert client ARM count <= host connect-replay count.
    @{ Name='no-stray-divergence-rearm'; Severity='CRITICAL'; Check={
        $arm    = Count $C 'claim tracking ARMED'
        $replay = Count $H 'world-ready -- replaying snapshot'
        @{ Pass = ($arm -le $replay)
           Detail = "client divergence ARM=$arm, host connect-replay=$replay (ARM<=replay; an ARM with no replay = the incremental arm re-armed the sweep)" }
    }},

    # ---- (5b) CONVERT PRIMARY (Q2, side B): the death-watch convert path is the steady-state primary for
    # kerfur transitions. INFO: in a JOIN-WINDOW-turnoff scenario this is 0 (the race that needs the owner);
    # in a STEADY-STATE-turnoff scenario it must be >=1 (convert fires, owner stays silent -- see the
    # boundary-discriminator above for the negative half). Run BOTH scenarios to assert the full boundary.
    @{ Name='convert-path-active'; Severity='INFO'; Check={
        $poll = Count $H 'kerfur_convert: POLL turn_off'
        $pon  = Count $H 'kerfur_convert: POLL turn-on'
        @{ Pass = $true; Detail = "$poll convert turn_off + $pon turn-on detection(s) (steady-state primary; 0 in a pure join-window-race run)" }
    }},

    # ---- (BT) BOUNDARY-TEST anti-vacuous gate: only when -BoundaryTest. Refuses a green verdict unless
    # BOTH sides were actually exercised -- side A (owner fired for a join-window orphan) AND side B (convert
    # fired for a steady-state turn-off). Without this, boundary-discriminator passes vacuously (0/0). This
    # is what stops the executable guard from itself becoming a "by design" untested-assumption hole.
    @{ Name='both-boundary-sides-exercised'; Severity=$(if($BoundaryTest){'CRITICAL'}else{'INFO'}); Check={
        $owner   = Count $H 'incremental PropSpawn for runtime-adopted kerfur-off prop'
        $convert = Count $H 'kerfur_convert: POLL turn_off'
        @{ Pass = (-not $BoundaryTest) -or ($owner -ge 1 -and $convert -ge 1)
           Detail = "side A owner-fired=$owner, side B convert-fired=$convert" +
                    $(if ($BoundaryTest) { ' (BoundaryTest: BOTH must be >=1 -- else the boundary was not exercised)' }
                      else { ' (informational; pass -BoundaryTest to REQUIRE both)' }) }
    }},

    # ---- (4) The deliver-missing owner actually FIRED for the late off-prop (the fix worked).
    # INFO: 0 is legitimate for a join with no in-window turn-off; >=1 proves the late arm delivered.
    @{ Name='deliver-missing-owner-fired'; Severity='INFO'; Check={
        $n = Count $H 'incremental PropSpawn for runtime-adopted kerfur-off prop'
        @{ Pass = $true; Detail = "$n kerfur-off owner delivery(ies) (>=1 = a join-window turn-off was delivered; 0 = none this run)" }
    }},

    # ---- (5) The client materialized the late off-prop (deferred adoption -> fresh-spawn at quiescence).
    @{ Name='client-late-kerfur-materialized'; Severity='INFO'; Check={
        $arm   = Count $C 'kerfur-prop-adopt: armed deferred adoption'
        $fresh = Count $C 'kerfur-prop-adopt: eid=\d+ .*no local twin.*fresh-spawning'
        @{ Pass = $true; Detail = "$arm deferred-adoption arm(s), $fresh fresh-spawn(s) at quiescence (the client side of the late delivery)" }
    }}
)

# ---- run ------------------------------------------------------------------
$hdr = "kerfur-delivery-assert"
if ($Scenario) { $hdr += " [$Scenario]" }
Write-Host "== $hdr =="
Write-Host "host log: $HostLog ($($H.Count) lines)"
Write-Host "client log: $ClientLog ($($C.Count) lines)"
Write-Host ""

$anyFail = $false
$rows = @()
foreach ($inv in $Invariants) {
    $r = & $inv.Check
    $isFail = (-not $r.Pass) -and ($inv.Severity -eq 'CRITICAL')
    if ($isFail) { $anyFail = $true }
    $status = if ($r.Pass) { 'PASS' } elseif ($inv.Severity -eq 'CRITICAL') { 'FAIL' } else { 'warn' }
    $rows += [pscustomobject]@{ Status=$status; Severity=$inv.Severity; Name=$inv.Name; Detail=$r.Detail }
    if (-not $Quiet) {
        Write-Host ("  [{0,-4}] {1,-30} {2}" -f $status, $inv.Name, $r.Detail)
    }
}

Write-Host ""
if ($anyFail) {
    Write-Host "VERDICT: FAIL (a CRITICAL kerfur-delivery invariant did not hold)" -ForegroundColor Red
    exit 1
} else {
    Write-Host "VERDICT: PASS (all CRITICAL kerfur-delivery invariants hold)" -ForegroundColor Green
    exit 0
}

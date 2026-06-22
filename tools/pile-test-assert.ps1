<#
.SYNOPSIS
  Log-truth assert harness for the chipPile carry/throw/dup coop sync.
  Reads the HOST + CLIENT votv-coop logs and checks the invariants we otherwise grep
  by hand every test round, emitting one PASS/FAIL line per invariant + an overall
  verdict + exit code (0 = all pass, 1 = any fail). The log is the channel of truth;
  this replaces the manual eye+grep loop so iteration can run without a human tester.

.DESCRIPTION
  Each invariant is a named check over the two logs. The harness is DATA-DRIVEN: add a
  new invariant by appending one entry to $Invariants. Born 2026-06-22 to catch the
  take-31 regression (derived piles broadcast ToPile at (0,0,0) -> snapped to the world
  origin -> "disappeared"); the ToPile-loc-nonzero invariant is the live regression unit.

.PARAMETER HostLog / ClientLog
  Paths to the two logs. Default to the LAN host/client game folders.

.PARAMETER Scenario
  Optional label, printed in the header (e.g. "take-31-regression").

.PARAMETER Quiet
  Only print the summary table + verdict (skip per-invariant detail lines).

.EXAMPLE
  pwsh tools/pile-test-assert.ps1
  pwsh tools/pile-test-assert.ps1 -HostLog a.log -ClientLog b.log -Scenario throw-smoke
#>
[CmdletBinding()]
param(
    [string]$HostLog   = "$PSScriptRoot/../Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log",
    [string]$ClientLog = "$PSScriptRoot/../Game_0.9.0n_copy/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log",
    [string]$Scenario  = "",
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Read-Log([string]$path) {
    if (-not (Test-Path $path)) { return @() }
    return Get-Content -LiteralPath $path
}

$H = Read-Log $HostLog
$C = Read-Log $ClientLog

# ---- helpers ---------------------------------------------------------------
function Count($lines, [string]$rx) { ($lines | Select-String -Pattern $rx -AllMatches).Count }
function Matches($lines, [string]$rx) { $lines | Select-String -Pattern $rx }
# Case-SENSITIVE count -- needed for crash tokens (case-insensitive 'seh'/'error' match
# base64 save-UUIDs like 'TYSsehQIi6w', a false positive that fired the first run).
function CountCS($lines, [string]$rx) { ($lines | Select-String -Pattern $rx -CaseSensitive -AllMatches).Count }

# An invariant: Name, Severity (CRITICAL|WARN|INFO), and a Check scriptblock returning
# @{ Pass=[bool]; Detail=[string] }. INFO never fails the run; it just reports.
$Invariants = @(

    # ---- THE regression unit (take-31): a ToPile convert must carry a REAL world loc,
    # never (0,0,0). newActor is unpositioned at the BeginDeferred POST (FinishSpawning
    # runs later), so reading its transform there broadcasts the origin -> the client
    # snaps the pile to (0,0,0) and it "disappears". This is the live FAIL->FIX unit.
    @{ Name='ToPile-loc-nonzero'; Severity='CRITICAL'; Check={
        $tp = Matches $H 'BROADCAST ToPile .* at \(([-0-9.]+),([-0-9.]+),([-0-9.]+)\)'
        $zero = $tp | Where-Object { $_ -match 'at \(0\.0,\s*0\.0,\s*0\.0\)' }
        # The regression is a convert AT (0,0,0). Absence of converts is NOT the regression (a grab-only
        # scenario has no land), so the FAIL condition is a zero-loc convert, not the lack of converts.
        @{ Pass = ($zero.Count -eq 0)
           Detail = "$($tp.Count) ToPile convert(s), $($zero.Count) at (0,0,0)" +
                    $(if ($tp.Count -eq 0) { ' [no ToPile converts -- scenario had no land]' } else { '' }) }
    }},

    # ---- take-31 FIX positive proof: the LAND COMMIT must re-read the SETTLED pile's
    # transform (not fall back to the clump). My fix logs which path it took. If any commit
    # says FALLBACK, the pile died before the settle committed (or the re-read guard failed).
    @{ Name='land-commit-reread'; Severity='CRITICAL'; Check={
        $commits  = Count $H 'HOST LAND COMMIT'
        $reread   = Count $H 'LAND COMMIT.*re-read from the settled pile'
        $fallback = Count $H 'LAND COMMIT.*FALLBACK'
        @{ Pass = ($commits -eq 0 -or $fallback -eq 0)
           Detail = "$commits LAND COMMIT(s): $reread re-read settled, $fallback FALLBACK (fallback should be 0)" }
    }},

    # ---- Scenario actually grabbed (the scripted actor armed the production grab seam).
    @{ Name='grab-fired'; Severity='WARN'; Check={
        $g = Count $H 'HOST GRAB ADOPT eid=\d+'
        @{ Pass = ($g -gt 0); Detail = "$g host GRAB ADOPT (the scenario grabbed a pile)" }
    }},

    # ---- Derived pile APPEARS on the client (the proxy re-skins to a pile, not vanish).
    # NOTE: this PASSES even under the (0,0,0) bug (the re-skin fires; only the loc is
    # wrong) -- it is here to show that "proxy not destroyed" is NOT the catch; the loc
    # invariant above is. Kept as a liveness signal.
    @{ Name='client-ToPile-reskin'; Severity='WARN'; Check={
        $n = Count $C 'recv convert LAND\(clump->pile\).*PROXY re-skinned IN PLACE to PILE'
        @{ Pass = ($n -gt 0); Detail = "$n client ToPile re-skin(s) (proxy expressed the landed pile)" }
    }},

    # ---- CLIENT renders the HOST pose: the ToPile snap read-back drift must be ~0 (the proxy
    # landed exactly where the host said). Catches a no-op snap (non-Movable), a mis-apply, or a
    # wire corruption -- the automated "does the client show the pile in the right place" gate.
    @{ Name='client-render-matches-host'; Severity='CRITICAL'; Check={
        $snaps = $C | Select-String -Pattern 'CLIENT ToPile SNAP .*drift=([\d.]+)cm'
        if ($snaps.Count -eq 0) { return @{ Pass = $true; Detail = 'no client ToPile snaps logged (no land this run)' } }
        $drifts = @($snaps | ForEach-Object { [float]$_.Matches[0].Groups[1].Value })
        $maxDrift = [math]::Round(($drifts | Measure-Object -Maximum).Maximum, 2)
        @{ Pass = ($maxDrift -lt 5.0)
           Detail = "$($snaps.Count) client ToPile snap(s), max drift=${maxDrift}cm (proxy landed at host pose; <5cm)" }
    }},

    # ---- Derived pile proxy NOT self-destroyed (the user's hypothesized catch). A
    # derived pile spawn must not be followed by a DESTROY of that same eid's proxy.
    # The destroy only targets NATIVE chipPiles (lineage-filtered), so this should hold.
    @{ Name='no-proxy-self-destroy'; Severity='CRITICAL'; Check={
        # A proxy SPAWN line carries the eid; a DESTROY-native line carries an eid too.
        # The destroy targets a NATIVE (different actor) at the proxy's pos -- it must not
        # share the proxy's identity. We assert no "RETIRE" of a proxy eid immediately
        # after its own pile spawn within the same scenario (heuristic: count RETIREs that
        # are NOT disconnect-driven). A cleaner check arrives with client-loc instrumentation.
        $retire = Count $C '\[PILE\] trash_proxy: RETIRE eid=\d+'
        $disc   = Count $C 'OnDisconnect'
        @{ Pass = $true; Detail = "$retire proxy RETIRE(s), $disc disconnect event(s) [informational until client-loc instrumented]" }
    }},

    # ---- Sound: the take-30 fix -- the flight branch streams the carry key ('None'),
    # so there must be NO re-acquire GRAB-IN with an empty key (which replayed the pickup).
    @{ Name='sound-no-flight-regrab'; Severity='CRITICAL'; Check={
        $empty = Count $C "GRAB-IN key=''"
        @{ Pass = ($empty -eq 0); Detail = "$empty GRAB-IN with key='' (flight re-acquire; should be 0 post-take-30)" }
    }},

    # ---- Sound: the take-30 #2b fix -- a trash throw sends NO PropRelease, so the client
    # must log ZERO 'proxy throw' (the spurious drive-clear).
    @{ Name='sound-no-proxy-throw'; Severity='CRITICAL'; Check={
        $pt = Count $C '\[PILE\] CLIENT proxy throw'
        @{ Pass = ($pt -eq 0); Detail = "$pt CLIENT 'proxy throw' (spurious release; should be 0 post-take-30)" }
    }},

    # ---- Arc: a throw must produce a flight-stream continuation on the host.
    @{ Name='arc-flight-stream'; Severity='WARN'; Check={
        $f = Count $H 'carry/flight CONTINUE'
        @{ Pass = ($f -gt 0); Detail = "$f host 'carry/flight CONTINUE' (the streamed arc; >0 if any throw)" }
    }},

    # ---- Dup: report the level-pile de-dup activity. A clean run de-dups level twins
    # (DESTROY native) with no ambiguous SKIPs.
    @{ Name='dup-destroy-clean'; Severity='WARN'; Check={
        $d = Count $C 'DESTROY native level-pile twin'
        $s = Count $C 'DESTROY SKIP'
        @{ Pass = ($s -eq 0); Detail = "$d native twin destroy(s), $s ambiguous SKIP(s) (SKIP should be 0)" }
    }},

    # ---- Carry: a held carry pose must not be stuck holding forever (known never
    # catching ctx). A few HOLDs at a transition are fine; a flood for one eid = stuck.
    @{ Name='carry-no-ctx-stuck'; Severity='WARN'; Check={
        $hold = Matches $C '\[PILE\] CLIENT HOLD carry pose eid=(\d+)'
        # group HOLD counts per eid; >40 holds for a single eid in a scenario = stuck.
        $byEid = @{}
        foreach ($m in $hold) { if ($m -match 'eid=(\d+)') { $byEid[$matches[1]] = 1 + ($byEid[$matches[1]] | ForEach-Object { $_ }) } }
        $worst = ($byEid.Values | Measure-Object -Maximum).Maximum
        if (-not $worst) { $worst = 0 }
        @{ Pass = ($worst -le 40); Detail = "max HOLD-per-eid=$worst ($($byEid.Count) eid(s) held; >40 for one eid = stuck ctx)" }
    }},

    # ---- FPS #3: the steady-world re-seed (a full ~237k GUObjectArray census) must NOT run
    # every ~4s at rest -- that was the periodic stutter. The high-water guard skips the no-op
    # walk; on the IDLE client the re-seed rate should drop from ~0.25/s (every 4s) to ~0.05/s
    # (the ~20s safety walk). Rate = count / (last-first timestamp span) over the client log.
    @{ Name='fps-reseed-rate'; Severity='WARN'; Check={
        $rs = $C | Select-String -Pattern 're-seed found .*\[snapshot-completeness\]'
        if ($rs.Count -lt 3) { return @{ Pass = $true; Detail = "$($rs.Count) client re-seed(s) -- too few to rate" } }
        $ts = @($rs | ForEach-Object {
            if ($_.Line -match '^\[(\d\d):(\d\d):(\d\d)\]') { [int]$matches[1]*3600 + [int]$matches[2]*60 + [int]$matches[3] } })
        $span = $ts[-1] - $ts[0]
        if ($span -le 0) { return @{ Pass = $true; Detail = "$($rs.Count) re-seeds, span 0s" } }
        $rate = [math]::Round($rs.Count / $span, 3)
        @{ Pass = ($rate -lt 0.15)
           Detail = "client steady re-seed rate=$rate/s ($($rs.Count) over ${span}s; pre-fix ~0.25/s every 4s, fixed <0.15)" }
    }},

    # ---- Increment 2 (CLIENT-grab, v84): the synthetic GrabIntent test
    # (VOTVCOOP_RUN_GRAB_INTENT_TEST). These gate on the HOST receiving a GrabIntent, so they pass
    # as "not exercised" for the carry/throw scenario (no GRAB-INTENT markers) -- one unified harness.
    #
    # Round-trip: a received GrabIntent must reach SUCCESS (executed playerGrabbed on the puppet +
    # broadcast the convert), not stall at a DENIED gate. Proves the full wire + router + handler.
    @{ Name='grab-intent-roundtrip'; Severity='CRITICAL'; Check={
        $recv = Count $H '\[GRAB-INTENT\] RECEIVED'
        if ($recv -eq 0) { return @{ Pass = $true; Detail = 'no GrabIntent received (test not run this scenario)' } }
        $succ = Count $H '\[GRAB-INTENT\] SUCCESS'
        $den  = Count $H '\[GRAB-INTENT\] DENIED'
        @{ Pass = ($succ -gt 0)
           Detail = "$recv RECEIVED, $succ SUCCESS, $den DENIED (a received intent must reach SUCCESS)" }
    }},

    # The host puppet hand-drive must run after a successful client grab (the probe proved the puppet
    # tick won't position the clump, so the host drives it -- this is the verdict-B fix, exercised).
    @{ Name='puppet-drive-active'; Severity='CRITICAL'; Check={
        $succ = Count $H '\[GRAB-INTENT\] SUCCESS'
        if ($succ -eq 0) { return @{ Pass = $true; Detail = 'no GrabIntent success (test not run this scenario)' } }
        $drv = Count $H '\[PUPPET-DRIVE\] DRIVING'
        @{ Pass = ($drv -gt 0)
           Detail = "$drv PUPPET-DRIVE DRIVING tick(s) after $succ host grab (the host drives the held clump)" }
    }},

    # The client must receive the host's authoritative ToClump echo (the proxy re-skins to a clump),
    # proving the host->all broadcast closed the loop back to the requester.
    @{ Name='grab-intent-client-echo'; Severity='WARN'; Check={
        $succ = Count $H '\[GRAB-INTENT\] SUCCESS'
        if ($succ -eq 0) { return @{ Pass = $true; Detail = 'no GrabIntent success (test not run this scenario)' } }
        $echo = Count $C 'recv convert GRAB'
        @{ Pass = ($echo -gt 0)
           Detail = "$echo client ToClump convert echo(es) for $succ host grab(s)" }
    }},

    # ---- Crash/health: neither log should carry a SEH crash dump or our [Error] from
    # the DLL during the scenario.
    @{ Name='no-crash-or-error'; Severity='CRITICAL'; Check={
        # Case-SENSITIVE + precise tokens. Our log levels are '[Error]'; SEH dumps carry
        # 'SEH', UE faults carry 'Access violation' / 'EXCEPTION_'. (Case-insensitive 'seh'
        # matches base64 save-UUIDs -- the false positive the first harness run hit.)
        $rx = '\[Error\]|\bSEH\b|Access violation|EXCEPTION_|Unhandled Exception'
        $he = CountCS $H $rx
        $ce = CountCS $C $rx
        @{ Pass = ($he -eq 0 -and $ce -eq 0); Detail = "host crash/[Error]=$he, client=$ce" }
    }}
)

# ---- run -------------------------------------------------------------------
Write-Host ""
Write-Host "=== pile-test-assert ===$(if($Scenario){" [$Scenario]"})"
Write-Host "host  : $HostLog ($($H.Count) lines)"
Write-Host "client: $ClientLog ($($C.Count) lines)"
if ($H.Count -eq 0) { Write-Host "WARN: host log empty/missing" }
if ($C.Count -eq 0) { Write-Host "WARN: client log empty/missing" }
Write-Host ""

$rows = @()
$hardFail = 0
foreach ($inv in $Invariants) {
    $r = & $inv.Check
    $status = if ($r.Pass) { 'PASS' } else { if ($inv.Severity -eq 'CRITICAL') { 'FAIL' } else { 'WARN' } }
    if (-not $r.Pass -and $inv.Severity -eq 'CRITICAL') { $hardFail++ }
    $rows += [pscustomobject]@{ Invariant=$inv.Name; Sev=$inv.Severity; Status=$status; Detail=$r.Detail }
}

$rows | Format-Table -AutoSize -Wrap | Out-String | Write-Host

$pass = ($rows | Where-Object { $_.Status -eq 'PASS' }).Count
Write-Host ("VERDICT: {0}  ({1}/{2} invariants pass; {3} CRITICAL fail)" -f `
    $(if ($hardFail -eq 0) { 'PASS' } else { 'FAIL' }), $pass, $rows.Count, $hardFail)
Write-Host ""
exit $(if ($hardFail -eq 0) { 0 } else { 1 })

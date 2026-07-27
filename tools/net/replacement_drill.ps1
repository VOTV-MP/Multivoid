# tools/net/replacement_drill.ps1 -- the arc-A REPLACEMENT drill + the
# SUCCESSOR-BAN drill, in one run.
#
# Companion to departure_drill.ps1, which proved the simpler half (a peer leaves
# and nobody takes the seat). This one drives the case the design calls the whole
# reason the ledger carries a TOKEN rather than a boolean:
#
#   slot 3's occupant leaves, a DIFFERENT person takes slot 3, and the receiver's
#   only evidence is the successor's row -- because the emptying row is INJECTED
#   LOST on one client ([dev] roster_drop_empty_rows=1). That client must still
#   perform death-then-birth: tear the departed person down, install the new one,
#   and never render both.
#
# CLIENT_1 runs with the injection; CLIENT_2 runs WITHOUT it as the control, so a
# pass on both is evidence about the mechanism rather than about the timing of
# one run. CLIENT_3 is the seat that changes hands: killed mid-session, then
# relaunched under a different nickname so the two occupants are distinguishable
# on sight in every log.
#
# The HOST simultaneously runs [dev] roster_token_selftest=1, which captures a
# moderation token when a slot is first occupied, HOLDS it across the departure
# (what an open ban modal does), and fires the real moderation::BanPlayer with it
# once someone else holds the seat. That must ABORT and write no ban row -- a
# permanent IP ban landing on a successor is the destructive bug arc A closes.
#
# Both dev flags are set here and RESTORED in the finally block, so the drill
# leaves the installs exactly as it found them.
#
# Instrument discipline, learned the hard way on departure_drill.ps1
# (memory/lesson_baseline_an_instrument_on_something_the_system_does_not_reset.md):
#   - the process is VotV-Win64-Shipping; 'VotV' is only the window title
#   - multivoid.log is ROTATED at boot, so it is graded, never used as a cue
#   - peers are identified by their EXE PATH (one install folder each), not by title
#   - every failure path exits non-zero with a named reason; "nothing to test"
#     must never be able to read as "test passed"

$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Programming\VOTV_MP'
$rel  = 'WindowsNoEditor\VotV\Binaries\Win64'
$dirs = @{
    HOST    = Join-Path $root "Game_0.9.0n_HOST\$rel"
    CLIENT1 = Join-Path $root "Game_0.9.0n_CLIENT_1\$rel"
    CLIENT2 = Join-Path $root "Game_0.9.0n_CLIENT_2\$rel"
    CLIENT3 = Join-Path $root "Game_0.9.0n_CLIENT_3\$rel"
}
$banlist = Join-Path $dirs.HOST 'multivoid-banlist.txt'
$victimSlot = 3
$shotsDir = Join-Path (Join-Path $root 'research') 'roster_shots'
New-Item -ItemType Directory -Force -Path $shotsDir | Out-Null

# The game's ini reader is byte-oriented; a BOM in front of a section header is a
# parse hazard. Windows PowerShell 5.1's `-Encoding UTF8` WRITES a BOM and pwsh 7's
# does not, so neither the arm nor the restore may go through Set-Content.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-IniText {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Set-DevFlag {
    param([string]$Ini, [string]$Key, [string]$Value)
    $lines = @(Get-Content -LiteralPath $Ini)
    $out = New-Object System.Collections.Generic.List[string]
    $done = $false
    foreach ($l in $lines) {
        if ($l -cmatch "^\s*$Key\s*=") { if (-not $done) { $out.Add("$Key=$Value"); $done = $true }; continue }
        $out.Add($l)
        if (-not $done -and $l -cmatch '^\s*\[dev\]\s*$') { $out.Add("$Key=$Value"); $done = $true }
    }
    if (-not $done) { $out.Add('[dev]'); $out.Add("$Key=$Value") }
    Write-IniText $Ini (($out -join "`r`n") + "`r`n")
}

function Get-PeerProcs {
    param([string]$Dir)
    @(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) })
}

# Photograph a peer at a named moment. Auto-runbook rule (USER 2026-07-27): where a
# check needs EYES, the drill produces the SCREEN and the human gives the verdict on
# it -- handing the check back as "irreducibly human" is the wrong output. The
# moments here are chosen so a GHOST (both bodies at once) or an INHERITED skin
# would be visible in the pair, which no log line can show.
function Save-PeerShot {
    param([string]$Dir, [string]$Moment, [string]$Label)
    $p = @(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue |
           Where-Object { $_.Path -and $_.Path.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) })
    if ($p.Count -lt 1) { Write-Host "  [shot] $Label/$Moment -- process gone"; return }
    $out = Join-Path $shotsDir "$Label`_$Moment.png"
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $root 'tools\capture_window.ps1') -ProcId $p[0].Id -Out $out | Out-Null
    if (Test-Path $out) { Write-Host ("  [shot] {0} -> {1:N0} B" -f (Split-Path $out -Leaf), (Get-Item $out).Length) }
    else { Write-Host "  [shot] $Label/$Moment -- capture produced no file" }
}

# Wait until a peer's log contains at least $Count matches of $Rx. Returns $false
# on timeout so the caller can throw with a MEANINGFUL reason -- every wait in
# this drill is a stated precondition, never a stopwatch, because a peer that is
# not ready yet fails the assertions in a way that reads as a code defect.
function Wait-ForLogHits {
    param([string]$Dir, [string]$Rx, [int]$Count, [int]$TimeoutMin)
    $log = Join-Path $Dir 'multivoid.log'
    $deadline = (Get-Date).AddMinutes($TimeoutMin)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $log) {
            $n = @(Get-Content -LiteralPath $log -Tail 80000 |
                   Select-String -Pattern $Rx -CaseSensitive).Count
            if ($n -ge $Count) { return $true }
        }
        Start-Sleep -Seconds 5
    }
    return $false
}

# ---- arm ---------------------------------------------------------------------
$backup = @{}
foreach ($k in 'HOST','CLIENT1','CLIENT2') {
    $p = Join-Path $dirs[$k] 'multivoid.ini'
    $backup[$k] = Get-Content -LiteralPath $p -Raw
}
$banlistBefore = if (Test-Path $banlist) { Get-Content -LiteralPath $banlist -Raw } else { $null }
Write-Host "[drill] banlist before: $(if ($null -eq $banlistBefore) { 'ABSENT' } else { "$($banlistBefore.Length) B" })"

$smoke = $null
$fail = New-Object System.Collections.Generic.List[string]
try {
    Set-DevFlag (Join-Path $dirs.HOST    'multivoid.ini') 'roster_token_selftest'   '1'
    Set-DevFlag (Join-Path $dirs.CLIENT1 'multivoid.ini') 'roster_drop_empty_rows'  '1'
    Set-DevFlag (Join-Path $dirs.CLIENT2 'multivoid.ini') 'roster_drop_empty_rows'  '0'
    Write-Host '[drill] armed: HOST roster_token_selftest=1, CLIENT1 loss injection ON, CLIENT2 control OFF'

    Write-Host '[drill] launching smoke4 (--duration 900) in the background...'
    $smoke = Start-Process -FilePath 'python' -ArgumentList 'tools/mp.py','smoke4','--duration','900' `
        -WorkingDirectory $root -PassThru -WindowStyle Minimized

    # Cue on the PROCESS SET; the logs are for the verdict only.
    $deadline = (Get-Date).AddMinutes(7)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if (@(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue).Count -ge 4) { break }
    }
    if (@(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue).Count -lt 4) {
        throw 'four peers never came up; nothing to test'
    }
    # PRECONDITION, waited for rather than assumed: the victim must actually HAVE
    # A BODY on both observers before it is killed. A fixed settle measured 75 s
    # from "four processes up", but the peers come up staggered -- CLIENT_3 got
    # only 23 s, never finished its world load, never streamed a pose, and so no
    # puppet was ever spawned for it. The teardown assertions then failed against
    # a peer that had nothing to tear down, which reads as a code defect and is
    # not one. Cue on the observers' own spawn line; time out with a named reason.
    $poseRx = "first remote pose on slot $victimSlot -> auto-spawning puppet"
    if (-not (Wait-ForLogHits $dirs.CLIENT1 $poseRx 1 8)) {
        throw "CLIENT_1 never spawned a puppet for slot $victimSlot -- the victim has no body to destroy"
    }
    if (-not (Wait-ForLogHits $dirs.CLIENT2 $poseRx 1 4)) {
        throw "CLIENT_2 never spawned a puppet for slot $victimSlot -- the victim has no body to destroy"
    }
    Write-Host '[drill] the victim has a body on both observers; 20 s settle, then the kill.'
    Start-Sleep -Seconds 20
    # MOMENT 1 -- the predecessor alive and embodied. The baseline every later shot
    # is read against (their skin/colour is what the successor must NOT inherit).
    Save-PeerShot $dirs.CLIENT1 'before' 'client1'
    Save-PeerShot $dirs.CLIENT2 'before' 'client2'

    $victim = Get-PeerProcs $dirs.CLIENT3
    if ($victim.Count -lt 1) { throw "CLIENT_3 process not found; cannot free slot $victimSlot" }
    Write-Host "[drill] killing CLIENT_3 PID=$($victim[0].Id) -- slot $victimSlot falls vacant"
    Stop-Process -Id $victim[0].Id -Force

    # Let the host reconcile + pulse. On CLIENT_1 every emptying row is dropped in
    # this window, so it must still believe #4 is present when the successor lands.
    Start-Sleep -Seconds 25

    Write-Host '[drill] relaunching CLIENT_3 as "Successor" -- it must take the same slot'
    $relaunch = Start-Process -FilePath 'python' `
        -ArgumentList 'tools/mp.py','client3','--nick','Successor' `
        -WorkingDirectory $root -PassThru -WindowStyle Minimized
    try { Wait-Process -Id $relaunch.Id -Timeout 180 } catch {}

    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        if ((Get-PeerProcs $dirs.CLIENT3).Count -ge 1) { break }
    }
    if ((Get-PeerProcs $dirs.CLIENT3).Count -lt 1) { throw 'the successor process never started' }

    # Wait for the successor to be genuinely MIRRORED, not for a stopwatch. The
    # host prints one `established MIRROR Player Element ... for peerSlot=N` per
    # occupant, and it is that line -- not the accept -- that puts a non-zero eid
    # into the roster row the other clients need. A fixed sleep measured 100 s and
    # the first run ended with the successor still mid-save-transfer, so the
    # interesting half was never observable.
    # The successor's mirror on the host is what puts a non-zero eid into the
    # roster row the other clients need (2 = the original occupant's, then this).
    $mirrorRx = "established MIRROR Player Element .* for peerSlot=$victimSlot"
    if (-not (Wait-ForLogHits $dirs.HOST $mirrorRx 2 5)) {
        throw "the successor never got a host-side mirror for slot $victimSlot"
    }
    # ...and then the successor must get a BODY on both observers, for the same
    # reason the victim had to have one: "no ghost frame" is a statement about two
    # bodies, and it is unfalsifiable if the second one never appears.
    if (-not (Wait-ForLogHits $dirs.CLIENT1 $poseRx 2 5)) {
        throw "CLIENT_1 never spawned a puppet for the successor in slot $victimSlot"
    }
    if (-not (Wait-ForLogHits $dirs.CLIENT2 $poseRx 2 4)) {
        throw "CLIENT_2 never spawned a puppet for the successor in slot $victimSlot"
    }
    Write-Host '[drill] successor mirrored + embodied on both observers; 20 s settle.'
    Start-Sleep -Seconds 20
    # MOMENT 2 -- the successor embodied. Against MOMENT 1 this is where a GHOST
    # (both bodies) or an INHERITED skin/colour/nameplate would be visible.
    Save-PeerShot $dirs.CLIENT1 'after' 'client1'
    Save-PeerShot $dirs.CLIENT2 'after' 'client2'
    Save-PeerShot $dirs.HOST    'after' 'host'

    $alive = @(Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue).Count
    Write-Host "[drill] harvest time; $alive peer processes alive"
    if ($alive -lt 4) { $fail.Add("only $alive/4 peers alive at harvest -- the successor did not survive") }

    # ---- verdicts -------------------------------------------------------------
    $log = @{}
    foreach ($k in 'HOST','CLIENT1','CLIENT2','CLIENT3') {
        $p = Join-Path $dirs[$k] 'multivoid.log'
        $log[$k] = if (Test-Path $p) { @(Get-Content -LiteralPath $p -Tail 60000) } else { @() }
        if ($log[$k].Count -eq 0) { $fail.Add("$k -- no log content to grade") }
    }
    # EVERY call site wraps the result in @(). A PowerShell function returning a
    # one-element collection hands back the bare element, and `$a + $b` on two
    # bare MatchInfo objects throws op_Addition -- which is exactly how the first
    # run died AFTER the peers were already gone. Same unwrap hazard as the
    # peerconn gate's control 4; it is a property of the shell, not of one script.
    function Hits { param([string[]]$Lines, [string]$Rx) @($Lines | Select-String -Pattern $Rx -CaseSensitive) }

    Write-Host ''
    Write-Host '========== HOST: the successor-ban drill =========='
    $handOver = @(Hits $log.HOST "roster_token_selftest: slot $victimSlot changed hands")
    $tokPass  = @(Hits $log.HOST 'roster_token_selftest: PASS')
    $fired    = @(Hits $log.HOST 'roster_token_selftest: firing the REAL BanPlayer')
    $aborted  = @(Hits $log.HOST 'moderation: ban of #\d+ ABORTED')
    foreach ($h in ($handOver + $tokPass + $fired + $aborted)) { Write-Host "  $h" }
    if ($handOver.Count -lt 1) { $fail.Add("HOST -- the selftest never saw slot $victimSlot change hands") }
    if ($tokPass.Count  -lt 1) { $fail.Add('HOST -- no token PASS line (stale refused + live accepted)') }
    if ($fired.Count    -lt 1) { $fail.Add('HOST -- the real BanPlayer was never fired with the stale token') }
    if ($aborted.Count  -lt 1) { $fail.Add('HOST -- BanPlayer did NOT abort; the successor may have been banned') }

    $banlistAfter = if (Test-Path $banlist) { Get-Content -LiteralPath $banlist -Raw } else { $null }
    Write-Host "  banlist after: $(if ($null -eq $banlistAfter) { 'ABSENT' } else { "$($banlistAfter.Length) B" })"
    if ($banlistAfter -ne $banlistBefore) { $fail.Add('HOST -- the banlist CHANGED; a ban was written for a stale token') }

    Write-Host ''
    Write-Host '========== HOST: the hand-over itself =========='
    foreach ($h in @(Hits $log.HOST "ledger: slot $victimSlot (emptied|occupied|REPLACED)")) { Write-Host "  $h" }

    foreach ($peer in 'CLIENT1','CLIENT2') {
        $injected = ($peer -ceq 'CLIENT1')
        Write-Host ''
        Write-Host "========== $peer ($(if ($injected) { 'LOSS INJECTED' } else { 'control' })) =========="
        $dropped  = @(Hits $log[$peer] 'DROPPED the empty row')
        $emptied  = @(Hits $log[$peer] "ledger: slot $victimSlot emptied")
        $replaced = @(Hits $log[$peer] "ledger: slot $victimSlot REPLACED")
        $occupied = @(Hits $log[$peer] "ledger: slot $victimSlot occupied")
        $destroy  = @(Hits $log[$peer] "net: peer slot $victimSlot \(#\d+\) left -- puppet destroyed")
        # The death/birth pair is graded on what the players Registry prints
        # UNCONDITIONALLY, not on `installed cross-peer identity`: that line is
        # deliberately gated on the install branch (it is a smoke counter), so a
        # client that already holds a puppet element takes the silent else-branch
        # and a drill grading on it reports a false absence. Measured on run 1.
        $released = @(Hits $log[$peer] "released Player Element .* for peerSlot=$victimSlot")
        $establish= @(Hits $log[$peer] "established MIRROR Player Element .* for peerSlot=$victimSlot")
        foreach ($h in ($dropped + $emptied + $replaced + $occupied + $destroy + $released + $establish)) { Write-Host "  $h" }

        if ($injected) {
            if ($dropped.Count -lt 1) { $fail.Add("$peer -- the loss injection never fired; nothing was tested") }
            if ($emptied.Count -gt 0) { $fail.Add("$peer -- saw the slot EMPTIED although every empty row was dropped") }
            if ($replaced.Count -lt 1) {
                $fail.Add("$peer -- no REPLACED transition; the successor's row did not read as death+birth")
            }
        } else {
            if ($dropped.Count -gt 0) { $fail.Add("$peer -- the control peer dropped rows; it is not a control") }
            if ($emptied.Count -lt 1) { $fail.Add("$peer -- the control never saw the departure at all") }
            if ($occupied.Count -lt 2) { $fail.Add("$peer -- the control never saw the successor arrive") }
        }
        # Both peers, either path: the departed body must be destroyed, and the
        # successor's element must be established AFTER the departed one was
        # released -- that ordering is what "no ghost frame" means in a log.
        if ($destroy.Count -lt 1)   { $fail.Add("$peer -- the departed puppet was never destroyed") }
        if ($released.Count -lt 1)  { $fail.Add("$peer -- the departed Player Element was never released") }
        if ($establish.Count -lt 2) { $fail.Add("$peer -- the successor's Player Element was never established") }
        if ($released.Count -ge 1 -and $establish.Count -ge 2) {
            if ($establish[-1].LineNumber -lt $released[-1].LineNumber) {
                $fail.Add("$peer -- the successor's element was established BEFORE the departed one was released (ghost frame)")
            }
        }
    }

    Write-Host ''
    Write-Host '========== HotPathGuard (all peers) =========='
    foreach ($k in 'HOST','CLIENT1','CLIENT2','CLIENT3') {
        $v = Hits $log[$k] 'HotPathGuard'
        Write-Host "  $k : $($v.Count)"
        if ($v.Count -gt 0) { $fail.Add("$k -- $($v.Count) HotPathGuard violation(s)") }
    }
}
finally {
    foreach ($k in 'HOST','CLIENT1','CLIENT2') {
        Write-IniText (Join-Path $dirs[$k] 'multivoid.ini') $backup[$k]
    }
    Write-Host ''
    Write-Host '[drill] dev flags restored to their pre-drill values.'
    if ($smoke) { try { Stop-Process -Id $smoke.Id -Force } catch {} }
    try { & python (Join-Path $root 'tools\mp.py') kill | Out-Null } catch {}
}

Write-Host ''
if ($fail.Count -gt 0) {
    Write-Host "[drill] FAIL -- $($fail.Count) check(s):"
    foreach ($f in $fail) { Write-Host "  - $f" }
    exit 1
}
Write-Host '[drill] PASS -- replacement (with the departure row lost) and successor-ban both held.'
exit 0

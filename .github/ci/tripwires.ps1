# tripwires.ps1 -- the UE4SS-switch decision trip-wires (VERSION_MIGRATION.md section 11).
#
# ADVISORY, always exit 0: a FIRED wire re-opens the F1 DECISION, it never blocks a
# release. Run from the RELEASE.md step-0 bullet; paste the output into the written
# release handoff. Remote-content checks can never FAIL a build (the stale-body
# lesson) -- the only hard logic here reads LOCAL files (the state file + section 11).
#
# Verdicts per wire: QUIET / FIRED / CHECK-UNREACHABLE (network-down is never read
# as 'not fired'). Plus OVERDUE-DECISION: a repeat FIRED/UNREACHABLE with no newer
# dated decision line in section 11 -- the mechanical no-wallpaper detector.
#
# Re-quiet discipline (no-wallpaper): a FIRED wire's disposition is a dated
#   'TRIPWIRE-DECISION <wire> <YYYY-MM-DD>: <text>'
# line appended to docs/VERSION_MIGRATION.md section 11 PLUS the matching constant
# update below, in the SAME commit. Constants here update ONLY together with such a line.
#
# Drills (all four verdict shapes; drills never write the real state file):
#   -DrillFired        check (a)'s code path against the public control repo -> FIRED shape
#   -DrillStableFloor  check (b) with a lower frozen floor (e.g. 2.0.0) -> FIRED on the real feed
#   -DrillOffline      both checks + the positive control pointed at dead targets -> CHECK-UNREACHABLE
#   -DrillOverdue      seeded prior-FIRED state + the must-NOT-clear control (an unrelated
#                      dated line must not clear it; only a matching TRIPWIRE-DECISION does)

param(
    [string]$StatePath = (Join-Path $PSScriptRoot 'tripwires_state.json'),
    [string]$MigrationDoc = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'docs/VERSION_MIGRATION.md'),
    [string]$DrillStableFloor,
    [switch]$DrillFired,
    [switch]$DrillOffline,
    [switch]$DrillOverdue
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- Decision constants (update ONLY with a dated TRIPWIRE-DECISION line in section 11) ---
# wire-a: the private engine core whose 404 blocks F2 AND F3 (measured 2026-07-26,
#         three ways: repo 404 w/ positive control; zero public forks/mirrors on all
#         of GitHub; zDEV release asset ships zero headers/libs).
$WireATarget  = 'https://github.com/Re-UE4SS/UEPseudo'
$WireAControl = 'https://github.com/UE4SS-RE/RE-UE4SS'   # positive transport control (404 vs network-down)
# wire-b: the newest UE4SS STABLE that existed when the F1 decision was taken
#         (releases/latest -> v3.0.1 / prerelease=false / 2024-02-14, measured 2026-07-26).
$WireBRepo     = 'UE4SS-RE/RE-UE4SS'
$WireBBaseline = [version]'3.0.1'

$isDrill = $DrillFired -or $DrillOffline -or $DrillOverdue -or $DrillStableFloor
$today = Get-Date -Format 'yyyy-MM-dd'

function Test-WireA([string]$Target, [string]$Control) {
    git ls-remote $Target HEAD 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        return @{ verdict = 'FIRED'; detail = "$Target is now REACHABLE -- the F2/F3 blocker fell; re-open the fork (section 11)" }
    }
    git ls-remote $Control HEAD 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        return @{ verdict = 'QUIET'; detail = "$Target still unreachable (404-class) while the control repo answers" }
    }
    return @{ verdict = 'CHECK-UNREACHABLE'; detail = 'both target AND control unreachable -- network/transport down, wire NOT judged' }
}

function Test-WireB([string]$Repo, [version]$Floor) {
    $releases = $null
    try {
        $raw = gh api "repos/$Repo/releases?per_page=100" 2>$null
        if ($LASTEXITCODE -eq 0 -and $raw) { $releases = @($raw | ConvertFrom-Json) }
    } catch { $releases = $null }
    if ($null -eq $releases) {
        return @{ verdict = 'CHECK-UNREACHABLE'; detail = "gh api releases for $Repo failed -- wire NOT judged" }
    }
    $stables = @(); $prereleases = @()
    foreach ($r in $releases) {
        if ($r.draft) { continue }
        if ($r.prerelease) { $prereleases += $r; continue }
        $m = [regex]::Match($r.tag_name, '^v?(\d+\.\d+\.\d+)')
        if ($m.Success) { $stables += [pscustomobject]@{ ver = [version]$m.Groups[1].Value; tag = $r.tag_name; date = $r.published_at } }
    }
    $maxStable = $stables | Sort-Object ver -Descending | Select-Object -First 1
    # the prerelease FILTER self-proves each run: the live feed carries a newer-dated
    # prerelease (experimental-latest, 2024-12-29) that must appear on this line, skipped.
    $newestPre = $prereleases | Sort-Object { $_.published_at } -Descending | Select-Object -First 1
    $preLine = if ($newestPre) { "newest SKIPPED prerelease: $($newestPre.tag_name) ($($newestPre.published_at))" } else { 'no prereleases in feed (filter positive control ABSENT this run)' }
    # repo-health context (a frozen/archived upstream stays visible without a 4th verdict)
    $health = ''
    try {
        $meta = gh api "repos/$Repo" 2>$null | ConvertFrom-Json
        if ($LASTEXITCODE -eq 0 -and $meta) { $health = "repo health: archived=$($meta.archived), pushed_at=$($meta.pushed_at)" }
    } catch { }
    if ($null -eq $maxStable) {
        return @{ verdict = 'CHECK-UNREACHABLE'; detail = 'feed returned no parseable stable releases -- wire NOT judged'; context = @($preLine, $health) }
    }
    if ($maxStable.ver -gt $Floor) {
        return @{ verdict = 'FIRED'; detail = "newest stable $($maxStable.tag) ($($maxStable.date)) > frozen baseline $Floor -- the 'stable is stale' leg fell; re-open (section 11)"; context = @($preLine, $health) }
    }
    return @{ verdict = 'QUIET'; detail = "newest stable $($maxStable.tag) ($($maxStable.date)) == baseline $Floor"; context = @($preLine, $health) }
}

# OVERDUE-DECISION: repeat FIRED/UNREACHABLE with no TRIPWIRE-DECISION line for THIS
# wire dated on/after the previous verdict's date. Anchors on the labeled marker only --
# an unrelated dated documentize edit must NOT clear it (drilled).
function Test-Overdue([string]$Wire, [string]$CurrentVerdict, $State, [string]$DocText) {
    if ($CurrentVerdict -eq 'QUIET') { return $false }
    if ($null -eq $State -or -not ($State.PSObject.Properties.Name -contains $Wire)) { return $false }
    $prev = $State.$Wire
    if ($prev.verdict -eq 'QUIET') { return $false }
    foreach ($m in [regex]::Matches($DocText, "TRIPWIRE-DECISION $([regex]::Escape($Wire)) (\d{4}-\d{2}-\d{2}):")) {
        if ($m.Groups[1].Value -ge $prev.date) { return $false }
    }
    return $true
}

function Show([string]$Wire, $Result) {
    Write-Host "TRIPWIRE ${Wire}: $($Result.verdict) -- $($Result.detail)"
    if ($Result.ContainsKey('context')) { foreach ($c in $Result.context) { if ($c) { Write-Host "  $c" } } }
}

# ------------------------------- drills -------------------------------
if ($DrillOffline) {
    $a = Test-WireA -Target 'https://tripwire-drill.invalid/none' -Control 'https://tripwire-drill.invalid/none2'
    $b = Test-WireB -Repo 'tripwire-drill.invalid/none' -Floor $WireBBaseline
    Show 'wire-a(offline-drill)' $a; Show 'wire-b(offline-drill)' $b
    $pass = ($a.verdict -eq 'CHECK-UNREACHABLE') -and ($b.verdict -eq 'CHECK-UNREACHABLE')
    Write-Host "DRILL offline: $(if ($pass) {'PASS -- both wires CHECK-UNREACHABLE, network-down never reads as not-fired'} else {'FAIL'})"
    exit 0
}
if ($DrillFired) {
    $a = Test-WireA -Target $WireAControl -Control $WireAControl
    Show 'wire-a(fired-drill)' $a
    Write-Host "DRILL fired-shape: $(if ($a.verdict -eq 'FIRED') {'PASS -- a reachable target produces the FIRED shape'} else {'FAIL'})"
    exit 0
}
if ($DrillStableFloor) {
    $b = Test-WireB -Repo $WireBRepo -Floor ([version]$DrillStableFloor)
    Show "wire-b(floor-drill $DrillStableFloor)" $b
    Write-Host "DRILL stable-floor: $(if ($b.verdict -eq 'FIRED') {'PASS -- the comparison half fires on the real feed'} else {'FAIL'})"
    exit 0
}
if ($DrillOverdue) {
    $seed = [pscustomobject]@{ 'wire-a' = [pscustomobject]@{ verdict = 'FIRED'; date = '2026-07-25' } }
    $noDecision   = "some doc text`nedited 2026-07-26 by documentize`n"                       # unrelated dated line
    $withDecision = $noDecision + "TRIPWIRE-DECISION wire-a 2026-07-26: drill disposition`n"
    $o1 = Test-Overdue 'wire-a' 'FIRED' $seed $noDecision      # must be OVERDUE
    $o2 = Test-Overdue 'wire-a' 'FIRED' $seed $withDecision    # must be cleared
    Write-Host "DRILL overdue step1 (repeat FIRED, unrelated dated line only): $(if ($o1) {'OVERDUE-DECISION printed -- PASS (must-NOT-clear control held)'} else {'FAIL'})"
    Write-Host "DRILL overdue step2 (matching TRIPWIRE-DECISION line): $(if (-not $o2) {'cleared -- PASS'} else {'FAIL'})"
    exit 0
}

# ------------------------------ real run ------------------------------
$state = $null
if (Test-Path -LiteralPath $StatePath) { $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json }
$docText = if (Test-Path -LiteralPath $MigrationDoc) { Get-Content -LiteralPath $MigrationDoc -Raw } else { '' }

$a = Test-WireA -Target $WireATarget -Control $WireAControl
$b = Test-WireB -Repo $WireBRepo -Floor $WireBBaseline
Show 'wire-a' $a; Show 'wire-b' $b
Write-Host 'TRIPWIRE wire-c: (monitor-less by design) a recook breaks the mod loudly; the forced migration-playbook run re-opens the fork.'
Write-Host 'TRIPWIRE human-carried doors: zDEV asset starts shipping headers/libs; a successor fork becomes the live line.'

foreach ($w in @(@('wire-a', $a), @('wire-b', $b))) {
    if (Test-Overdue $w[0] $w[1].verdict $state $docText) {
        Write-Host "TRIPWIRE $($w[0]): OVERDUE-DECISION -- repeat $($w[1].verdict) with no newer 'TRIPWIRE-DECISION $($w[0]) <date>:' line in section 11. Append the decision line + re-freeze in the SAME commit."
    }
}

# state write (real runs only; the human commits it in the release flow)
[pscustomobject]@{
    'wire-a' = [pscustomobject]@{ verdict = $a.verdict; date = $today }
    'wire-b' = [pscustomobject]@{ verdict = $b.verdict; date = $today }
} | ConvertTo-Json | Set-Content -LiteralPath $StatePath -Encoding utf8
Write-Host "tripwires: state written to $StatePath (commit it with the release flow)"
exit 0

# tools/net/peerconn_gate.ps1 -- the STANDING per-slot OCCUPANCY GENERATION gate
# (arc A T3; design research/findings/join-identity/
# votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md).
#
# THE RULE: every write to `peerConns_[...]` carries an explicit GEN: annotation
# saying what happens to the slot's occupancy generation there -- `mint` (a new
# occupant), `clear` (the slot emptied), or `none` WITH A REASON. A new write
# site therefore breaks the build until someone decides, which is the whole
# point: the generation is the only thing that can express "person X left and
# person Y took the slot with no empty moment in between", and a write site that
# forgets it silently reintroduces the bug.
#
# WHY THIS SCRIPT EXISTS RATHER THAN A COMMENT: the design's own census of these
# sites listed FIVE and there are SEVEN -- it grepped `.store(` and the two
# `.exchange(0)` clears (Session::Stop, Session::Kick) were invisible to it. The
# Kick one is load-bearing: without its clear, a kicked slot keeps a live
# generation and the ledger never sees the row empty. So the gate censuses by
# OPERATION KIND, not by one verb
# (memory/lesson_census_the_operation_kind_not_only_the_sites.md).
#
# Jobs (each with a fixture-injected MUST-FIRE control run every invocation --
# a green run first proves every detector can go red):
#   1. UNANNOTATED SITE -- a peerConns_ write with no GEN: annotation in its
#      leading comment block.
#   2. UNKNOWN ANNOTATION -- a GEN: annotation that is not mint/clear/none.
#   3. REASONLESS `none` -- `GEN: none` must be followed by `--` and prose.
#   4. EMPTY CENSUS -- zero sites found means the field was renamed and this
#      gate went blind; that fails rather than passing green.
#
# Case-sensitivity: every match is -cmatch / Ordinal on purpose
# (lesson_powershell_defaults_are_case_insensitive_everywhere).
# Exit codes are EXPLICIT (lesson_gha_pwsh_step_exits_with_last_child_code).

param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }

$srcDir = Join-Path $Root 'src\votv-coop\src\coop\net'

$violations      = New-Object System.Collections.Generic.List[string]
$controlFailures = New-Object System.Collections.Generic.List[string]

# A write site = the field, any subscript, then a MUTATING atomic verb. Reads
# (.load()) are not sites. Adding a verb here is how the census stays honest if
# the code ever uses compare_exchange/fetch_* on this field.
$siteRx  = [regex]'peerConns_\[[^\]]*\]\.(store|exchange|compare_exchange_strong|compare_exchange_weak|fetch_or|fetch_and)\('
# The annotation, as it appears in the leading comment block.
$annRx   = [regex]'GEN:\s*(?<kind>[a-z]+)(?<rest>.*)$'
$kindsOk = @('mint', 'clear', 'none')
# How far above a site we look for its annotation. A site's own line counts too
# (index 0), so this is "the site line plus the 8 lines above it".
$lookback = 8

# ---- the classifier, as a pure function so the fixture controls feed it
# synthetic corpora through the SAME code path --------------------------------
function Test-PeerConnSites {
    param([string[]]$Lines, [string]$Label)
    $found = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if (-not $siteRx.IsMatch($Lines[$i])) { continue }
        # A site inside a comment is documentation, not code.
        if ($Lines[$i] -cmatch '^\s*(//|\*)') { continue }
        $ann = $null
        $from = [Math]::Max(0, $i - $lookback)
        for ($j = $i; $j -ge $from; $j--) {
            $m = $annRx.Match($Lines[$j])
            if ($m.Success) { $ann = $m; break }
        }
        $found.Add([pscustomobject]@{
            Label = $Label; Line = $i + 1; Text = $Lines[$i].Trim(); Ann = $ann
        })
    }
    return $found
}

function Get-SiteViolations {
    param([object[]]$Sites)
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($s in $Sites) {
        if (-not $s.Ann) {
            $out.Add("$($s.Label):$($s.Line) -- peerConns_ write with NO `GEN:` annotation: $($s.Text)")
            continue
        }
        $kind = $s.Ann.Groups['kind'].Value
        if ($kindsOk -cnotcontains $kind) {
            $out.Add("$($s.Label):$($s.Line) -- unknown annotation `GEN: $kind` (expected mint/clear/none)")
            continue
        }
        if ($kind -ceq 'none' -and $s.Ann.Groups['rest'].Value -cnotmatch '--\s*\S') {
            $out.Add("$($s.Label):$($s.Line) -- `GEN: none` must carry a reason (`GEN: none -- <why>`)")
        }
    }
    return $out
}

# ---- fixture MUST-FIRE controls (every run) ---------------------------------
$ctlUnannotated = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        peerConns_[slot].store(hConn);'
)
if ((Get-SiteViolations $ctlUnannotated).Count -ne 1) {
    $controlFailures.Add('control 1: an unannotated site did NOT fire')
}
$ctlUnknown = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        // GEN: maybe',
    '        peerConns_[slot].store(hConn);'
)
if ((Get-SiteViolations $ctlUnknown).Count -ne 1) {
    $controlFailures.Add('control 2: an unknown annotation kind did NOT fire')
}
$ctlReasonless = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        // GEN: none',
    '        peerConns_[0].store(hConn);'
)
if ((Get-SiteViolations $ctlReasonless).Count -ne 1) {
    $controlFailures.Add('control 3: a reasonless `GEN: none` did NOT fire')
}
$ctlExchange = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        const uint32_t hConn = peerConns_[i].exchange(0);'
)
if ($ctlExchange.Count -ne 1) {
    $controlFailures.Add('control 4: the census MISSED an .exchange( site (the verb-blindness this gate exists for)')
}
$ctlGreen = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        // GEN: none -- a client never mints.',
    '        peerConns_[0].store(hConn);',
    '        // GEN: clear',
    '        peerConns_[1].exchange(0);'
)
if ($ctlGreen.Count -ne 2 -or (Get-SiteViolations $ctlGreen).Count -ne 0) {
    $controlFailures.Add('control 5: a well-annotated corpus was NOT accepted')
}
$ctlComment = Test-PeerConnSites -Label 'fixture' -Lines @(
    '        // peerConns_[slot].store(hConn) is how the accept edge registers.'
)
if ($ctlComment.Count -ne 0) {
    $controlFailures.Add('control 6: a commented-out mention was counted as a site')
}

if ($controlFailures.Count -gt 0) {
    Write-Host 'peerconn_gate: CONTROL FAILURE -- the detectors themselves are broken:'
    foreach ($c in $controlFailures) { Write-Host "  $c" }
    exit 1
}

# ---- the real census --------------------------------------------------------
if (-not (Test-Path $srcDir)) { Write-Host "peerconn_gate: FAIL -- missing $srcDir"; exit 1 }
$allSites = New-Object System.Collections.Generic.List[object]
foreach ($f in Get-ChildItem -Path $srcDir -Filter *.cpp -File) {
    $lines = Get-Content -LiteralPath $f.FullName
    foreach ($s in (Test-PeerConnSites -Label $f.Name -Lines $lines)) { $allSites.Add($s) }
}

if ($allSites.Count -eq 0) {
    Write-Host 'peerconn_gate: FAIL -- censused ZERO peerConns_ write sites.'
    Write-Host '  The field was renamed or moved and this gate went blind. A gate that'
    Write-Host '  finds nothing must fail, not pass (a zero-row green is not evidence).'
    exit 1
}

foreach ($v in (Get-SiteViolations $allSites)) { $violations.Add($v) }

$byKind = @{ mint = 0; clear = 0; none = 0 }
foreach ($s in $allSites) {
    if ($s.Ann) {
        $k = $s.Ann.Groups['kind'].Value
        if ($byKind.ContainsKey($k)) { $byKind[$k]++ }
    }
}
Write-Host ("peerconn_gate: sites={0} (mint={1} clear={2} none={3}) controls=6/6" -f `
    $allSites.Count, $byKind['mint'], $byKind['clear'], $byKind['none'])

if ($violations.Count -gt 0) {
    Write-Host 'peerconn_gate: FAIL'
    foreach ($v in $violations) { Write-Host "  $v" }
    Write-Host ''
    Write-Host '  Every peerConns_ write decides the slot occupancy generation. Annotate the'
    Write-Host '  site with one of:'
    Write-Host '    // GEN: mint            -- a new occupant takes the slot'
    Write-Host '    // GEN: clear           -- the slot emptied (put it AFTER the inbox erase)'
    Write-Host '    // GEN: none -- <why>   -- deliberately no generation change'
    exit 1
}

Write-Host 'peerconn_gate: PASS'
exit 0

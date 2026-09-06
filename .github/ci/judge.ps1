# judge.ps1 -- the refuse-to-publish predicate (design D3, R16/R17/R18).
# Executed from the MAIN checkout (checkout#2) in release-core; also runnable
# locally for a dry-run. Evaluates ALL preconditions with NO short-circuit and
# emits the full verdict vector -- one labeled PASS/FAIL/SKIP line per check --
# so a drill asserts its NAMED line and an early failure can never mask the
# branch under test.
#
# Publish allowed IFF state(N) == EXPECTED AND tag sha == row sha AND tag game
# == row game AND no OTHER tag/release carries N besides the triggering tag +
# this run's own draft; plus tag-commit proto == N, main HEAD proto > N.
# Completion is judged by tag-ASSOCIATION (R18): a live release on the
# triggering tag whose body 'source:' == the tag sha -> ALREADY_PUBLISHED no-op.
#
# Exit codes: 0 = PUBLISH, 10 = ALREADY_PUBLISHED (no-op success), 1 = REFUSE.

param(
    [Parameter(Mandatory)][string]$TagName,
    [string]$TagShaEvidence = '',            # caller-passed sha; EVIDENCE only -- the judge resolves the tag itself
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$Repo = 'VOTV-MP/Multivoid',
    [string]$Remote = 'origin',
    [switch]$SkipApi                          # local dry-run: release-API checks -> labeled SKIP (never PASS)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$checks = [System.Collections.Generic.List[object]]::new()
function Add-Check([string]$Name, [string]$Result, [string]$Detail) {
    $checks.Add([pscustomobject]@{ Name = $Name; Result = $Result; Detail = $Detail })
    Write-Host ("CHECK {0}: {1} -- {2}" -f $Name, $Result, $Detail)
}

Push-Location $RepoRoot
try {

# 1. TAG_FORMAT
$tag = ConvertFrom-ReleaseTag $TagName
if ($tag) { Add-Check 'TAG_FORMAT' 'PASS' "game=$($tag.Game) N=$($tag.N) dev=$($tag.Dev)" }
else      { Add-Check 'TAG_FORMAT' 'FAIL' "'$TagName' does not match the tag grammar" }

# 2. TAG_RESOLVE -- rev-list is authoritative (annotated-tag safe); the passed
#    sha is logged as evidence only.
$tagSha = $null
$resolved = git rev-list -n1 $TagName 2>$null
if ($LASTEXITCODE -eq 0 -and $resolved) {
    $tagSha = $resolved.Trim()
    $note = if ($TagShaEvidence -and $TagShaEvidence -ne $tagSha) { " (caller evidence $TagShaEvidence differs -- annotated tag?)" } else { '' }
    Add-Check 'TAG_RESOLVE' 'PASS' "$TagName -> $tagSha$note"
} else {
    Add-Check 'TAG_RESOLVE' 'FAIL' "tag '$TagName' not resolvable in this repo (fetch tags first)"
}

# 3. TAG_PROTO -- proto at the tag commit == N == the tag's number.
if ($tag -and $tagSha) {
    $p = Get-ProtoAtCommit -Commitish $tagSha
    if ($null -eq $p)        { Add-Check 'TAG_PROTO' 'FAIL' 'kProtocolVersion unreadable at the tag commit' }
    elseif ($p -eq $tag.N)   { Add-Check 'TAG_PROTO' 'PASS' "proto at tag commit = $p = N" }
    else                     { Add-Check 'TAG_PROTO' 'FAIL' "proto at tag commit = $p, tag N = $($tag.N)" }
} else { Add-Check 'TAG_PROTO' 'FAIL' 'unevaluable (tag format/resolve failed)' }

# 4. MAIN_PROTO -- main HEAD proto > N (the consume commit bumped past it).
if ($tag) {
    $pm = Get-ProtoAtCommit -Commitish "$Remote/main"
    if ($null -eq $pm)       { Add-Check 'MAIN_PROTO' 'FAIL' "kProtocolVersion unreadable at $Remote/main" }
    elseif ($pm -gt $tag.N)  { Add-Check 'MAIN_PROTO' 'PASS' "proto at $Remote/main = $pm > N = $($tag.N)" }
    else                     { Add-Check 'MAIN_PROTO' 'FAIL' "proto at $Remote/main = $pm, expected > N = $($tag.N) (consume commit not landed?)" }
} else { Add-Check 'MAIN_PROTO' 'FAIL' 'unevaluable (tag format failed)' }

# 4b. NOTES_OK -- the changelog authority file for N exists on THIS (main)
#     checkout and passes the format lint (pre-build refusal: no 40-min build
#     is wasted on a missing/malformed notes file). Semantic truth of the
#     prose is human-gated at RELEASE.md step 0.5, not judged here.
if ($tag) {
    $notesPath = Get-ReleaseNotesPath -N $tag.N
    if (-not (Test-Path -LiteralPath $notesPath)) {
        Add-Check 'NOTES_OK' 'FAIL' "notes file missing: tools/release/notes/b$($tag.N).md (write it on main, re-run)"
    } else {
        $notesViolations = @(Test-ReleaseNotesFormat -Content (Get-Content -LiteralPath $notesPath -Raw))
        if ($notesViolations.Count -eq 0) { Add-Check 'NOTES_OK' 'PASS' "notes/b$($tag.N).md present, format clean" }
        else { Add-Check 'NOTES_OK' 'FAIL' "notes/b$($tag.N).md format: $($notesViolations -join '; ')" }
    }
} else { Add-Check 'NOTES_OK' 'FAIL' 'unevaluable (tag format failed)' }

# 5. LEDGER_PARSE + 6. LEDGER_STATE + 7. LEDGER_SHA + 8. LEDGER_GAME
$ledgerPath = Join-Path $PSScriptRoot 'LEDGER.tsv'
$state = $null
$ledger = Read-Ledger -Path $ledgerPath
if ($ledger.Errors.Count -gt 0) { Add-Check 'LEDGER_PARSE' 'FAIL' ($ledger.Errors -join '; ') }
else                            { Add-Check 'LEDGER_PARSE' 'PASS' "$($ledger.Rows.Count) rows (empty ledger = valid start)" }
if ($tag) {
    $state = Get-LedgerState -Rows $ledger.Rows -N $tag.N
    switch ($state.State) {
        'EXPECTED'  { Add-Check 'LEDGER_STATE' 'PASS' "state(N=$($tag.N)) = EXPECTED" }
        'PUBLISHED' { Add-Check 'LEDGER_STATE' 'FAIL' "state(N=$($tag.N)) = PUBLISHED (closed; only a completion no-op is valid)" }
        'TERMINAL'  { Add-Check 'LEDGER_STATE' 'FAIL' "N is terminal ($($state.TerminalClass))" }
        default     { Add-Check 'LEDGER_STATE' 'FAIL' "state(N=$($tag.N)) = NONE -- no consume row (tag without consume)" }
    }
    if ($state.State -in @('EXPECTED', 'PUBLISHED')) {
        if ($tagSha -and $tagSha -eq $state.SourceSha) { Add-Check 'LEDGER_SHA' 'PASS' "tag sha == row sha ($tagSha)" }
        else { Add-Check 'LEDGER_SHA' 'FAIL' "tag sha $tagSha != row sha $($state.SourceSha) (retag allowed only TOWARD the row's sha)" }
        if ($tag.Game -eq $state.Game) { Add-Check 'LEDGER_GAME' 'PASS' "tag game == row game ($($tag.Game))" }
        else { Add-Check 'LEDGER_GAME' 'FAIL' "tag game $($tag.Game) != row game $($state.Game)" }
        if ($tag.TagName -ne $state.TagName) {
            # same N+sha+game but a different dev/stable shape than consumed
            Add-Check 'LEDGER_TAGNAME' 'FAIL' "tag '$TagName' != consumed tagName '$($state.TagName)'"
        } else { Add-Check 'LEDGER_TAGNAME' 'PASS' "tag name matches the consume row" }
    } else {
        Add-Check 'LEDGER_SHA'  'FAIL' 'unevaluable (no open row)'
        Add-Check 'LEDGER_GAME' 'FAIL' 'unevaluable (no open row)'
        Add-Check 'LEDGER_TAGNAME' 'FAIL' 'unevaluable (no open row)'
    }
} else {
    foreach ($c in 'LEDGER_STATE', 'LEDGER_SHA', 'LEDGER_GAME', 'LEDGER_TAGNAME') { Add-Check $c 'FAIL' 'unevaluable (tag format failed)' }
}

# 9. FOREIGN_TAG -- no OTHER remote tag carries N (N collides across game
#    targets in the loader, so same-N ANY-game counts; non-grammar v* tags
#    cannot carry an N -> WARN via lint, ignored here).
if ($tag) {
    $foreign = @()
    $lsRemote = git ls-remote --tags $Remote 'v*' 2>$null
    if ($LASTEXITCODE -ne 0) { Add-Check 'FOREIGN_TAG' 'FAIL' "git ls-remote --tags $Remote failed" }
    else {
        foreach ($line in @($lsRemote)) {
            if ($line -notmatch 'refs/tags/(?<name>\S+?)(\^\{\})?$') { continue }
            $name = $Matches['name']
            $other = ConvertFrom-ReleaseTag $name
            if ($other -and $other.N -eq $tag.N -and $name -ne $TagName) { $foreign += $name }
        }
        if ($foreign.Count -eq 0) { Add-Check 'FOREIGN_TAG' 'PASS' "no other remote tag carries N=$($tag.N)" }
        else { Add-Check 'FOREIGN_TAG' 'FAIL' "foreign tag(s) carry N=$($tag.N): $($foreign -join ', ')" }
    }
} else { Add-Check 'FOREIGN_TAG' 'FAIL' 'unevaluable (tag format failed)' }

# 10. FOREIGN_RELEASE + COMPLETION (tag-association, R18).
$completion = 'NONE'
if ($SkipApi) {
    Add-Check 'FOREIGN_RELEASE' 'SKIP' 'offline dry-run (-SkipApi) -- NOT a pass'
    Add-Check 'COMPLETION' 'SKIP' 'offline dry-run (-SkipApi)'
} elseif ($tag) {
    $releases = $null
    try {
        $releases = @(gh api "repos/$Repo/releases?per_page=100" --paginate 2>$null | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0) { throw 'gh api failed' }
    } catch { Add-Check 'FOREIGN_RELEASE' 'FAIL' 'release API unreachable (fail-closed)' }
    if ($null -ne $releases) {
        $foreignRel = @(); $onTag = @()
        foreach ($rel in $releases) {
            $rtag = ConvertFrom-ReleaseTag $rel.tag_name
            if ($rel.tag_name -eq $TagName) { $onTag += $rel; continue }
            if ($rtag -and $rtag.N -eq $tag.N) { $foreignRel += $rel.tag_name }
        }
        if ($foreignRel.Count -eq 0) { Add-Check 'FOREIGN_RELEASE' 'PASS' "no other release carries N=$($tag.N)" }
        else { Add-Check 'FOREIGN_RELEASE' 'FAIL' "foreign release(s) carry N=$($tag.N): $($foreignRel -join ', ')" }
        $live = @($onTag | Where-Object { -not $_.draft })
        if ($live.Count -eq 0) {
            Add-Check 'COMPLETION' 'PASS' 'no live release on the triggering tag (stale drafts, if any, are deleted at publish)'
        } else {
            $bodySha = Get-ReleaseBodySource $live[0].body
            if ($null -eq $bodySha) { $completion = 'RELEASE_BODY_UNPARSEABLE'; Add-Check 'COMPLETION' 'FAIL' 'live release body has no parseable source: key (RELEASE_BODY_UNPARSEABLE -- reconcile)' }
            elseif ($bodySha -eq $tagSha) { $completion = 'ALREADY_PUBLISHED'; Add-Check 'COMPLETION' 'PASS' "live release source == tag sha -> ALREADY_PUBLISHED no-op" }
            else { $completion = 'RELEASE_TAG_MISMATCH'; Add-Check 'COMPLETION' 'FAIL' "live release source $bodySha != tag sha $tagSha (RELEASE_TAG_MISMATCH -- reconcile)" }
        }
    }
} else {
    Add-Check 'FOREIGN_RELEASE' 'FAIL' 'unevaluable (tag format failed)'
    Add-Check 'COMPLETION' 'FAIL' 'unevaluable (tag format failed)'
}

} finally { Pop-Location }

# --- Verdict --------------------------------------------------------------
# ALREADY_PUBLISHED: the completion association holds and the ledger is not
# terminal for N (a terminal N with a live release is a lint reconcile, and
# LEDGER_STATE=FAIL(terminal) keeps the refusal). A fresh PUBLISH needs every
# check PASS (SKIPs from -SkipApi block a real publish by construction).
$stateName = if ($state) { $state.State } else { 'NONE' }
$verdict = 'REFUSE'
if ($completion -eq 'ALREADY_PUBLISHED' -and $stateName -in @('EXPECTED', 'PUBLISHED')) {
    $verdict = 'ALREADY_PUBLISHED'
} else {
    $nonPass = @($checks | Where-Object { $_.Result -ne 'PASS' })
    if ($nonPass.Count -eq 0) { $verdict = 'PUBLISH' }
}
Write-Host "VERDICT: $verdict"
switch ($verdict) {
    'PUBLISH'           { exit 0 }
    'ALREADY_PUBLISHED' { exit 10 }
    default             { exit 1 }
}

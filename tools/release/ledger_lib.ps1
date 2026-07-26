# ledger_lib.ps1 -- shared primitives for the release lane (tag grammar, ledger
# parse, the state(N) fold, proto extraction, release-body machine keys).
# Dot-source this file; it defines functions only, no side effects.
# Design of record: research/findings/tooling/votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md (section 3 D3).

Set-StrictMode -Version Latest

# ONE tag grammar for the whole lane: v<game>-b<N>[-dev].
# <game> = d.d.d + optional single letter (e.g. 0.9.0n); <N> = decimal, no leading zero.
$script:TagRegex = '^v(?<game>\d+\.\d+\.\d+[a-z]?)-b(?<n>[1-9]\d*)(?<dev>-dev)?$'

# Repo-relative path of the wire-revision header (kProtocolVersion) -- the same
# file CMakeLists regex-parses for the build number.
$script:ProtocolHeaderPath = 'src/votv-coop/include/coop/net/protocol.h'

function Get-ReleaseTagRegex { $script:TagRegex }

function ConvertFrom-ReleaseTag {
    param([Parameter(Mandatory)][string]$TagName)
    if ($TagName -cnotmatch $script:TagRegex) { return $null }   # -c: the grammar is case-SENSITIVE (PS -match is not)
    [pscustomobject]@{
        TagName = $TagName
        Game    = $Matches['game']
        N       = [int]$Matches['n']
        Dev     = [bool]($Matches.ContainsKey('dev') -and $Matches['dev'])
    }
}

# --- Ledger --------------------------------------------------------------
# File format (tools/release/LEDGER.tsv): '#' comments and blank lines ignored;
# every row = 6 tab-separated fields:  kind  N  game  tagName  sourceSha  date
# kind in { consume | published | burn | retracted }. Append-only, HUMAN-written.

function Read-Ledger {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "ledger not found: $Path" }
    $rows = @(); $errors = @(); $lineNo = 0
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        $lineNo++
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $f = $t -split "`t+"
        if ($f.Count -ne 6) { $errors += "line ${lineNo}: expected 6 tab-separated fields, got $($f.Count)"; continue }
        $kind, $n, $game, $tagName, $sha, $date = $f
        if ($kind -notin @('consume', 'published', 'burn', 'retracted')) { $errors += "line ${lineNo}: unknown kind '$kind'"; continue }
        if ($n -notmatch '^[1-9]\d*$') { $errors += "line ${lineNo}: bad N '$n' (decimal, no leading zero)"; continue }
        if ($sha -cnotmatch '^[0-9a-f]{40}$') { $errors += "line ${lineNo}: sourceSha must be a full lowercase 40-hex sha, got '$sha'"; continue }
        if ($date -notmatch '^\d{4}-\d{2}-\d{2}$') { $errors += "line ${lineNo}: date must be YYYY-MM-DD, got '$date'"; continue }
        $tag = ConvertFrom-ReleaseTag $tagName
        if (-not $tag) { $errors += "line ${lineNo}: tagName '$tagName' fails the tag grammar"; continue }
        if ($tag.N -ne [int]$n) { $errors += "line ${lineNo}: tagName N ($($tag.N)) != column N ($n)"; continue }
        if ($tag.Game -ne $game) { $errors += "line ${lineNo}: tagName game ($($tag.Game)) != column game ($game)"; continue }
        $rows += [pscustomobject]@{
            Line = $lineNo; Kind = $kind; N = [int]$n; Game = $game
            TagName = $tagName; SourceSha = $sha; Date = $date; Dev = $tag.Dev
        }
    }
    [pscustomobject]@{ Rows = $rows; Errors = $errors }
}

# state(N) = fold of the ledger rows carrying N, in file order (design D3, R16/R19):
#   consume {sha,game}  -> EXPECTED(sha, game)        (the mint expectation, in-flight)
#   published           -> PUBLISHED                  (closed, API-free)
#   burn / retracted    -> TERMINAL forever           (never republishes)
# Grammar misuse (second consume over an unclosed mint, published without a
# consume, burn over PUBLISHED, ...) lands in .Faults -- the lint fails on them;
# the fold itself stays conservative (TERMINAL sticks, a bad consume never
# overwrites an existing state).
function Get-LedgerState {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows,
          [Parameter(Mandatory)][int]$N)
    $st = [pscustomobject]@{
        N = $N; State = 'NONE'; SourceSha = $null; Game = $null; TagName = $null
        TerminalClass = $null; ConsumeDate = $null; Faults = @()
    }
    foreach ($r in @($Rows | Where-Object { $_.N -eq $N })) {
        switch ($r.Kind) {
            'consume' {
                if ($st.State -eq 'NONE') {
                    $st.State = 'EXPECTED'; $st.SourceSha = $r.SourceSha; $st.Game = $r.Game
                    $st.TagName = $r.TagName; $st.ConsumeDate = $r.Date
                } else {
                    $st.Faults += "line $($r.Line): consume over state $($st.State) for N=$N (ambiguous mint)"
                }
            }
            'published' {
                if ($st.State -eq 'TERMINAL') {
                    $st.Faults += "line $($r.Line): published over TERMINAL for N=$N"
                } else {
                    if ($st.State -ne 'EXPECTED') {
                        $st.Faults += "line $($r.Line): published without an open consume for N=$N (state was $($st.State))"
                    } elseif ($r.SourceSha -ne $st.SourceSha -or $r.TagName -ne $st.TagName) {
                        $st.Faults += "line $($r.Line): published row sha/tag disagrees with the consume row for N=$N"
                    }
                    $st.State = 'PUBLISHED'
                }
            }
            'burn' {
                if ($st.State -eq 'PUBLISHED') { $st.Faults += "line $($r.Line): burn over PUBLISHED for N=$N (bytes were public -- use retracted)" }
                $st.State = 'TERMINAL'; $st.TerminalClass = 'burn'
            }
            'retracted' {
                if ($st.State -notin @('PUBLISHED', 'EXPECTED')) { $st.Faults += "line $($r.Line): retracted with no publish/consume history for N=$N (state was $($st.State))" }
                $st.State = 'TERMINAL'; $st.TerminalClass = 'retracted'
            }
        }
    }
    $st
}

# The newest BARE-tag row whose state(N) == PUBLISHED (fold-aware -- a retracted
# N has a published row too; the terminal closes it; R23). Returns $null if no
# stable has ever been published.
function Get-NewestStablePublished {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows)
    $cands = @($Rows | Where-Object { $_.Kind -eq 'published' -and -not $_.Dev })
    for ($i = $cands.Count - 1; $i -ge 0; $i--) {
        $st = Get-LedgerState -Rows $Rows -N $cands[$i].N
        if ($st.State -eq 'PUBLISHED') { return $st }
    }
    $null
}

# --- Proto extraction ----------------------------------------------------

# kProtocolVersion at a given commit, read via `git show` (no checkout needed).
# Same regex family as CMakeLists.txt:30. Returns $null if absent/unparseable.
function Get-ProtoAtCommit {
    param([Parameter(Mandatory)][string]$Commitish, [string]$GitDir = '.')
    $content = git -C $GitDir show "${Commitish}:$script:ProtocolHeaderPath" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $content) { return $null }
    $m = [regex]::Match(($content -join "`n"), 'kProtocolVersion\s*=\s*(\d+)')
    if ($m.Success) { [int]$m.Groups[1].Value } else { $null }
}

# --- Release-body machine keys (R22) -------------------------------------
# ONE format shared by the publish step, the completion check, and the
# RELEASE.md template:  'source: <40hex>'  +  'sha256: <64hex>  <filename>'.

# The machine-key grammars, shared by the writer, the completion parser, the
# publish backstop asserts, the notes-format lint, and NOTES_DRIFT (one format,
# one parser). Case-SENSITIVE via [regex] -- never the PS -match default.
$script:SourceLineRegex = '(?m)^source:\s*[0-9a-f]{40}\s*$'
$script:Sha256LineRegex = '(?m)^sha256:\s*[0-9a-f]{64}\s\s\S+\s*$'

# Anchor phrases shared VERBATIM between the release-body Install block and
# docs/INSTALL.md (ledger_lint INSTALL_CONSISTENT asserts they appear in the
# doc). Reword only both together.
$script:InstallFolderAnchor    = 'WindowsNoEditor\VotV\Binaries\Win64'
$script:InstallDeleteOldAnchor = 'delete the old `multivoid-*.dll`'
$script:InstallGuideUrl        = 'https://github.com/VOTV-MP/Multivoid/blob/main/docs/INSTALL.md'

# --- Game target (the identity's game half) --------------------------------
# THE one PS-side parser of VOTVCOOP_GAME_TARGET (CMakeLists.txt is the code
# authority; no other tools/release script may re-parse it). Parser-miss FAILs
# loudly as UNREADABLE -- never returns $null into a comparison (the
# ABSENT/UNREADABLE tri-state lesson).
$script:CMakeListsPath = 'src/votv-coop/CMakeLists.txt'

function Get-GameTargetFromCMake {
    param([string]$CMakePath = $script:CMakeListsPath)
    if (-not (Test-Path -LiteralPath $CMakePath)) { throw "UNREADABLE game target: $CMakePath not found" }
    $content = Get-Content -LiteralPath $CMakePath -Raw
    $m = [regex]::Match($content, '(?m)^set\(VOTVCOOP_GAME_TARGET\s+"(?<t>[^"]+)"\)')
    if (-not $m.Success) { throw "UNREADABLE game target: set(VOTVCOOP_GAME_TARGET ""..."") not found in $CMakePath" }
    $m.Groups['t'].Value
}

# --- Release notes (the changelog authority; tools/release/notes/) ---------
function Get-ReleaseNotesPath {
    param([Parameter(Mandatory)][int]$N)
    Join-Path $PSScriptRoot "notes/b$N.md"
}

# Format lint for a notes file (judge NOTES_OK + local drills). Returns a list
# of violation strings; empty list = OK. Semantic truth is human-gated -- this
# checks FORMAT only.
function Test-ReleaseNotesFormat {
    param([AllowEmptyString()][string]$Content)
    $violations = @()
    if (-not $Content -or -not $Content.Trim()) { $violations += 'notes file is empty'; return $violations }
    $firstLine = ($Content -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if ($firstLine -and $firstLine.TrimStart().StartsWith('#')) {
        $violations += "notes must not open with a markdown heading (the body template owns the heading): '$firstLine'"
    }
    if ([regex]::IsMatch($Content, $script:SourceLineRegex)) { $violations += 'notes contain a source:-grammar line (machine-key collision)' }
    if ([regex]::IsMatch($Content, $script:Sha256LineRegex)) { $violations += 'notes contain a sha256:-grammar line (machine-key collision)' }
    $violations
}

# Normalize prose for NOTES_DRIFT comparison: CRLF -> LF, strip trailing
# whitespace per line, trim outer blank lines. ORDINAL equality after this --
# never a case-insensitive compare.
function Get-NormalizedProse {
    param([AllowEmptyString()][string]$Text)
    if ($null -eq $Text) { return '' }
    $lines = ($Text -replace "`r`n", "`n") -split "`n" | ForEach-Object { $_.TrimEnd() }
    (($lines -join "`n").Trim("`n"))
}

# Extract the '## What's new' section from a live release body (NOTES_DRIFT).
# Returns $null when the section is ABSENT -- callers must label that outcome,
# never conflate it with an empty section.
function Get-ReleaseBodyWhatsNew {
    param([AllowEmptyString()][string]$Body)
    if (-not $Body) { return $null }
    $m = [regex]::Match($Body, '(?ms)^## What''s new[ \t]*\r?\n(?<sec>.*?)(?=^## |\z)')
    if (-not $m.Success) { return $null }
    $m.Groups['sec'].Value
}

# --- The ONE body writer (publish, retro regeneration, recovery republish) --
function New-ReleaseBody {
    param([Parameter(Mandatory)][string]$SourceSha,
          [Parameter(Mandatory)][hashtable]$Sha256ByFile,   # filename -> 64-hex
          [Parameter(Mandatory)][string]$NotesContent,      # the b<N>.md content (What's new)
          [switch]$Dev,
          [string[]]$ExtraLines = @())
    $payload = @($Sha256ByFile.Keys | Where-Object { $_ -clike 'multivoid-*.dll' })
    if ($payload.Count -ne 1) { throw "New-ReleaseBody: expected exactly one multivoid-*.dll in the sha map, got $($payload.Count)" }
    $lines = @()
    if ($Dev) { $lines += 'Development build -- not hands-on verified.' }
    $lines += $ExtraLines
    $lines += ''
    $lines += "## What's new"
    $lines += ''
    $lines += (Get-NormalizedProse $NotesContent)
    $lines += ''
    $lines += '## Install'
    $lines += ''
    $lines += "You need **both** files below: ``$($payload[0])`` (the mod) + ``xinput1_3.dll`` (the loader)."
    $lines += "Drop them into ``$($script:InstallFolderAnchor)`` inside your game install."
    $lines += "Updating? Replace the mod DLL only -- and $($script:InstallDeleteOldAnchor)."
    $lines += "Full guide: $($script:InstallGuideUrl)"
    $lines += ''
    $lines += '## Build provenance'
    $lines += ''
    $lines += "source: $SourceSha"
    foreach ($f in ($Sha256ByFile.Keys | Sort-Object)) {
        $lines += "sha256: $($Sha256ByFile[$f].ToLowerInvariant())  $f"
    }
    $lines -join "`n"
}

# The completion check's parser: the 'source:' key, or $null if unparseable
# (-> RELEASE_BODY_UNPARSEABLE, fail-closed; R22).
function Get-ReleaseBodySource {
    param([AllowEmptyString()][string]$Body)
    if (-not $Body) { return $null }
    $m = [regex]::Match($Body, '(?m)^source:\s*([0-9a-f]{40})\s*$')
    if ($m.Success) { $m.Groups[1].Value } else { $null }
}

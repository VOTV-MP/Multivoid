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
# doc). Reword only both together. Re-SHAPED at WP-2 commit 3 (UE4SS_ARC 8.4):
# a single "install folder" had no true value once there were two lanes, so the
# folder anchor is now explicitly the MANUAL lane's mod-folder destination (the
# managed lane's path belongs to r2modman's VFS and is never typed anywhere),
# and the delete-old anchor is the upgrade-from-standalone rule (pre-b144
# installs left two DLLs beside the exe; the mod REFUSES to start beside them).
$script:InstallModFolderAnchor = 'WindowsNoEditor\VotV\Binaries\Win64\Mods\Multivoid'
$script:InstallDeleteOldAnchor = 'delete the old `multivoid-*.dll` and `xinput1_3.dll`'
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
# TWO artifact eras, decided by what the sha map actually HOLDS (data, never a
# flag): the ZIP era (WP-2 commit 3 onward -- exactly one package zip) and the
# LEGACY two-DLL era (b122..b143 bodies are LIVE on GitHub; notes_regen rebuilds
# them from their own machine lines, and the Install block must describe THAT
# page's assets -- zip-era prose over two-DLL assets would lie about its own
# page). The legacy prose is FROZEN literal text on purpose: INSTALL.md no
# longer carries it, so it must never ride the live anchors or change again.
function New-ReleaseBody {
    param([Parameter(Mandatory)][string]$SourceSha,
          [Parameter(Mandatory)][hashtable]$Sha256ByFile,   # filename -> 64-hex
          [Parameter(Mandatory)][string]$NotesContent,      # the b<N>.md content (What's new)
          [switch]$Dev,
          [string[]]$ExtraLines = @())
    $zips   = @($Sha256ByFile.Keys | Where-Object { $_ -clike '*.zip' })
    $legacy = @($Sha256ByFile.Keys | Where-Object { $_ -clike 'multivoid-*.dll' })
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
    if ($zips.Count -eq 1 -and $Sha256ByFile.Count -eq 1) {
        $lines += "One file: ``$($zips[0])`` -- the same zip serves both install lanes."
        $lines += 'Mod manager (recommended): install from Thunderstore, or r2modman -> Settings -> Profile -> "Import local mod" with this file.'
        $lines += "Manual: unzip, then copy the CONTENTS of ``mod\`` into ``$($script:InstallModFolderAnchor)`` inside your game install (requires UE4SS -- see the full guide)."
        $lines += "Upgrading from a pre-b144 standalone install? First $($script:InstallDeleteOldAnchor) beside the game executable."
        $lines += "Full guide: $($script:InstallGuideUrl)"
    } elseif ($legacy.Count -eq 1 -and $Sha256ByFile.Count -eq 2 -and
              (@($Sha256ByFile.Keys) -ccontains 'xinput1_3.dll')) {
        $lines += "You need **both** files below: ``$($legacy[0])`` (the mod) + ``xinput1_3.dll`` (the loader)."
        $lines += 'Drop them into `WindowsNoEditor\VotV\Binaries\Win64` inside your game install.'
        $lines += 'Updating? Replace the mod DLL only -- and delete the old `multivoid-*.dll`.'
        $lines += "Full guide: $($script:InstallGuideUrl)"
    } else {
        throw ("New-ReleaseBody: sha map matches neither artifact era (zip era = exactly one " +
               "*.zip; legacy era = one multivoid-*.dll + xinput1_3.dll); keys: " +
               "$(@($Sha256ByFile.Keys | Sort-Object) -join ', ')")
    }
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

# --- Package identity + shape (WP-9; docs/UE4SS_ARC.md 7.2a / 7.3) ---------
# These five are the packaging half of this library. They live HERE and not in
# package.ps1 because publish.ps1 must re-run the identical tree predicate on the
# artifact it downloads back -- two copies of a fail-closed check is two checks that
# can disagree, which is the failure the one-parser rule already exists to prevent.

# kProtocolVersion in the WORKING TREE -- the sibling of Get-ProtoAtCommit, which
# reads it at a commit. Same UNREADABLE tri-state discipline as the game half: a
# parse miss THROWS rather than returning $null into a comparison.
function Get-ProtoFromWorktree {
    param([string]$RepoRoot = '.')
    $p = Join-Path $RepoRoot $script:ProtocolHeaderPath
    if (-not (Test-Path -LiteralPath $p)) { throw "UNREADABLE build number: $p not found" }
    foreach ($ln in (Get-Content -LiteralPath $p)) {
        $m = [regex]::Match($ln, '^\s*inline constexpr uint16_t kProtocolVersion\s*=\s*(\d+)\s*;')
        if ($m.Success) { return [int]$m.Groups[1].Value }
    }
    throw "UNREADABLE build number: kProtocolVersion not found in $p"
}

# 7.3's mapping, stated there so it cannot be misread: split the game target on '.',
# take fields 1 and 2, strip non-digits from each (so a hypothetical 0.9n still
# yields 0.9), and FAIL CLOSED if either is empty after stripping. The target's
# third field and letter suffix are deliberately unused -- the build number already
# disambiguates them, and the full Paper pair stays exact in the description.
function ConvertTo-PackageVersion {
    param([Parameter(Mandatory)][string]$GameTarget, [Parameter(Mandatory)][int]$Proto)
    $f = $GameTarget.Split('.')
    if ($f.Count -lt 2) { throw "UNREADABLE game target '$GameTarget': fewer than two dot-separated fields" }
    $major = ($f[0] -replace '\D', '')
    $minor = ($f[1] -replace '\D', '')
    if (-not $major) { throw "UNREADABLE game target '$GameTarget': major field has no digits" }
    if (-not $minor) { throw "UNREADABLE game target '$GameTarget': minor field has no digits" }
    if ($Proto -le 0) { throw "UNREADABLE build number '$Proto': must be a positive integer" }
    "$major.$minor.$Proto"
}

# The one constructor of the package zip's filename (<Team>-<Name>-<version>.zip,
# UE4SS_ARC 7.2a convention). package.ps1 writes the file; publish.ps1 must
# PREDICT the name to find and upload it -- two copies of the format string is
# how a writer and its reader drift apart.
function Get-PackageZipName {
    param([Parameter(Mandatory)][string]$Version)
    "Multivoid-Multivoid-$Version.zip"
}

# PNG IHDR width/height. Thunderstore requires icon.png to be EXACTLY 256x256 and the
# pre-flight checklist says to re-measure rather than trust the filename -- this is
# that measurement, not a name check.
function Get-PngDimensions {
    param([Parameter(Mandatory)][string]$Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 24) { throw "not a PNG (too short): $Path" }
    $sig = @(137, 80, 78, 71, 13, 10, 26, 10)
    for ($i = 0; $i -lt 8; $i++) {
        if ($b[$i] -ne $sig[$i]) { throw "not a PNG (bad signature): $Path" }
    }
    # IHDR width/height are big-endian uint32 at offsets 16 and 20.
    # MEASURED 2026-08-26: PowerShell -shl follows the LEFT operand WIDTH, so
    # [byte]1 -shl 8 is 0, not 256. Widen to [int] first or every dimension reads 0.
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    [pscustomobject]@{ Width = $w; Height = $h }
}

# manifest.json, GENERATED (7.3 HARD REQUIREMENT). Field rules are the wiki's, with
# our values recorded in docs/THUNDERSTORE.md section 3:
#   name           no spaces, [A-Za-z0-9_] only
#   version_number semver X.Y.Z, whole numbers, NO suffix
#   description    max 250 chars (it is also the gallery subtitle)
#   dependencies   {team}-{package}-{version}; the shimloader IS the UE4SS delivery
#   website_url    optional VALUE, but the KEY must exist -- use "" if unused
# 'author' is deliberately absent: it is not required and the namespace comes from
# the uploading Team, not the file (3 of 5 field packages omit it entirely).
function New-PackageManifest {
    param(
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$GameTarget,
        [Parameter(Mandatory)][int]$Build
    )
    # The Paper pair stays EXACT here -- this is the destination 7.3 names for the
    # information the X.Y mapping drops (the game target's letter suffix).
    $desc = "Co-op multiplayer for Voices of the Void. Host or join a session with friends. " +
            "Multivoid $GameTarget b$Build. Modifies no game files."
    if ($desc.Length -gt 250) { throw "manifest description is $($desc.Length) chars, max is 250" }
    $obj = [ordered]@{
        name           = 'Multivoid'
        version_number = $Version
        website_url    = 'https://multivoid.dev'
        description    = $desc
        dependencies   = @('Thunderstore-unreal_shimloader-1.1.7')
    }
    ($obj | ConvertTo-Json -Depth 4)
}

# The fail-closed tree check 7.4b requires: "an empty or mis-rooted zip is a silently
# broken release, and this project has shipped one silently-broken artifact before."
# Returns a list of violations (empty when the zip is well-formed).
#
# It asserts the two traps 7.2a measured off r2modman's own rule engine:
#   - NO wrapping folder. A top-level directory matching no route is RECURSED INTO
#     and its files are classified individually, so a wrapped zip scatters.
#   - the payload is at mod/dlls/, never at the root. A root-level dlls/ matches no
#     route and silently never loads (trap 1).
function Test-PackageZip {
    param(
        [Parameter(Mandatory)][string]$ZipPath,
        [string]$ExpectedPayloadSha256 = ''   # exact check when the caller knows the bytes
    )
    $bad = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $ZipPath)) { $bad.Add("zip not found: $ZipPath"); return $bad }
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ZipPath).Path)
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName.Replace([char]92, [char]47) })
        $required = @('manifest.json', 'icon.png', 'README.md', 'mod/enabled.txt', 'mod/dlls/main.dll')
        foreach ($r in $required) {
            if ($names -cnotcontains $r) { $bad.Add("missing required entry: $r") }
        }
        # Every top-level segment must be a route r2modman knows, or a root file.
        $routes = @('mod', 'pak', 'cfg', 'overlay')
        foreach ($n in $names) {
            if ($n.EndsWith([char]47)) { continue }     # directory entries carry no payload
            $seg = $n.Split([char]47)
            if ($seg.Count -eq 1) { continue }          # a root file is always fine
            if ($routes -cnotcontains $seg[0]) {
                $bad.Add("entry '$n' sits under top-level '$($seg[0])', which matches no r2modman route -- it would be recursed into and scattered")
            }
        }
        # THE PAYLOAD'S BYTES, not just its name (post-ship audit 2026-08-26, CRITICAL).
        # Every other check here is presence-by-name, so a zero-byte or garbage
        # main.dll satisfied all of them and still reported PACKAGE OK -- which is
        # the exact class 7.9 records this project having already shipped once
        # ("wrong bytes from a payload picked by mtime"). The header of this very
        # function calls an "empty zip" the thing it exists to refuse; until now
        # "empty" was asserted about the SHAPE and never measured about the payload.
        # No arbitrary size floor: a threshold is a guess, and a truncated download
        # still starts with MZ. So the always-on legs are non-empty + the PE magic,
        # and an EXACT sha256 is checked whenever the caller knows what it handed in.
        $pe = $zip.Entries | Where-Object { $_.FullName -ceq 'mod/dlls/main.dll' } | Select-Object -First 1
        if ($pe) {
            if ($pe.Length -eq 0) {
                $bad.Add('mod/dlls/main.dll is ZERO BYTES -- the package would install a mod that cannot load')
            } else {
                $ms = New-Object System.IO.MemoryStream
                $es = $pe.Open()
                try { $es.CopyTo($ms) } finally { $es.Dispose() }
                $bytes = $ms.ToArray(); $ms.Dispose()
                if ($bytes.Length -lt 2 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
                    $bad.Add("mod/dlls/main.dll does not begin with the PE magic 'MZ' -- it is not a DLL")
                }
                if ($ExpectedPayloadSha256) {
                    $sha = [System.Security.Cryptography.SHA256]::Create()
                    try { $got = ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
                    finally { $sha.Dispose() }
                    if ($got -cne $ExpectedPayloadSha256.ToLowerInvariant()) {
                        $bad.Add("mod/dlls/main.dll sha256 $got != expected $($ExpectedPayloadSha256.ToLowerInvariant()) -- the zip does not carry the payload it was given")
                    }
                }
            }
        }

        # The manifest must parse and carry a well-formed identity.
        $me = $zip.Entries | Where-Object { $_.FullName -ceq 'manifest.json' } | Select-Object -First 1
        if ($me) {
            $sr = New-Object System.IO.StreamReader($me.Open())
            try { $json = $sr.ReadToEnd() } finally { $sr.Dispose() }
            $m = $null
            try { $m = $json | ConvertFrom-Json } catch { $bad.Add('manifest.json does not parse as JSON') }
            if ($m) {
                # StrictMode is LIVE here (set at the top of this file, and dot-sourcing
                # runs in the caller's scope), so touching an ABSENT property throws
                # PropertyNotFoundException instead of reporting a violation -- measured
                # 2026-08-26. The .PSObject.Properties guard was already used for two of
                # the five fields; the other three were reached directly, so a manifest
                # merely MISSING a key crashed the gate rather than failing it, and the
                # next natural drill arm would have taken the whole drill down with it.
                $has = { param($k) $null -ne $m.PSObject.Properties[$k] }
                if (-not (& $has 'name')) { $bad.Add('manifest is missing the name KEY') }
                elseif ($m.name -notmatch '^[A-Za-z0-9_]{1,128}$') { $bad.Add("manifest name '$($m.name)' breaks the [A-Za-z0-9_] rule") }
                if (-not (& $has 'version_number')) { $bad.Add('manifest is missing the version_number KEY') }
                elseif ($m.version_number -notmatch '^\d+\.\d+\.\d+$') { $bad.Add("manifest version_number '$($m.version_number)' is not a suffix-free semver triple") }
                if (-not (& $has 'description')) { $bad.Add('manifest is missing the description KEY') }
                elseif ($m.description.Length -gt 250) { $bad.Add("manifest description is $($m.description.Length) chars, max 250") }
                if (-not (& $has 'website_url')) { $bad.Add('manifest is missing the website_url KEY (it may be empty but must exist)') }
                if (-not (& $has 'dependencies')) { $bad.Add('manifest is missing dependencies') }
            }
        }
    } finally { $zip.Dispose() }
    $bad
}

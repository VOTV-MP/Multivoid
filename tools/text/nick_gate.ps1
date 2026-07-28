# nick_gate.ps1 -- ARC D: police the OPERATION KIND, not a list of sites.
#
# WHY. The design's C2 was briefed as "replace the four truncators". A hand-listed
# site set is not a defence: the fifth resize() lands in a file nobody thought to
# list, and the failure is silent -- a mid-character cut produces exactly the
# ill-formed tail the receive boundary refuses whole, so a real player's name
# arrives as the placeholder and nothing logs an error. The project already paid
# for this lesson twice on this very lane
# ([[lesson-census-the-operation-kind-not-only-the-sites]]).
#
# So this greps for the VERB -- a raw byte/unit truncation applied to something
# nick-shaped -- anywhere outside the codec that owns it.
#
# FAIL-CLOSED ON AN EMPTY CENSUS, like tools/net/peerconn_gate.ps1 and
# tools/config/registry_gate.ps1: if the anchor scan finds nothing, the pattern
# has rotted and a zero-row green is not evidence.
[CmdletBinding()]
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src  = Join-Path $repo 'src\votv-coop'

# The ONE file allowed to cut bytes: it is the thing that knows where a character
# boundary is.
$codecRel = 'src\coop\text\utf8_codec.cpp'

function Get-Sources {
    Get-ChildItem -Path $src -Recurse -Include *.cpp, *.h |
        Where-Object { $_.FullName -notmatch '\\third_party\\' }
}

$files = @(Get-Sources)
if ($files.Count -eq 0) {
    Write-Host 'nick_gate: FAIL -- scanned 0 files (the source layout moved; a zero-row green is not evidence)'
    exit 1
}

# ANCHOR CENSUS: the identifiers this gate reasons about must exist. If none do,
# the vocabulary changed and every check below would pass vacuously.
$anchors = @('kNickMaxChars', 'kNickMaxBytes', 'CapUtf8Bytes', 'CapCodepoints')
$anchorHits = 0
foreach ($f in $files) {
    $t = Get-Content -Raw -LiteralPath $f.FullName
    foreach ($a in $anchors) { if ($t -match [regex]::Escape($a)) { $anchorHits++; break } }
}
if ($anchorHits -eq 0) {
    Write-Host 'nick_gate: FAIL -- anchor census empty (no file mentions the nick capacity vocabulary)'
    exit 1
}

# THE VERB. A raw truncation of a nick-shaped value: `<something>nick<something>`
# followed by .resize( or .substr(, in either case. Also catches the bare
# `nickUtf8.resize(200)` shape the four known truncators used.
$verb = '(?i)\bnick[A-Za-z0-9_]*\s*\.\s*(resize|substr)\s*\('
$violations = @()
foreach ($f in $files) {
    $rel = $f.FullName.Substring($repo.Length + 1)
    if ($rel -ieq $codecRel) { continue }
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $n++
        if ($line -match $verb) {
            $violations += [pscustomobject]@{ File = $rel; Line = $n; Text = $line.Trim() }
        }
    }
}

# POSITIVE CONTROL: the pattern must actually fire on a known-bad line, or a clean
# report means nothing. This is the control the four-truncator census never had.
$fixture = 'if (nickUtf8.size() > 200) nickUtf8.resize(200);'
if ($fixture -notmatch $verb) {
    Write-Host 'nick_gate: FAIL -- the detector does not match its own known-bad fixture'
    exit 1
}

if (-not $Quiet) {
    Write-Host "nick_gate: scanned $($files.Count) files, $anchorHits carry the capacity vocabulary, detector control PASS"
}
if ($violations.Count -gt 0) {
    Write-Host "nick_gate: FAIL -- $($violations.Count) raw nick truncation(s) outside $codecRel"
    foreach ($v in $violations) { Write-Host ("  {0}:{1}  {2}" -f $v.File, $v.Line, $v.Text) }
    Write-Host '  Use coop::text::CapUtf8Bytes (bytes) or coop::text::CapCodepoints (characters):'
    Write-Host '  a raw cut lands mid-character and the receive boundary then refuses the whole field.'
    exit 1
}
Write-Host 'nick_gate: PASS'
exit 0

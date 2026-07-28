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

# ---------------------------------------------------------------------------
# THE SECOND VERB: NARROWING. Added 2026-07-28 after this gate, in its
# truncation-only form, sat GREEN over four shipped narrow defects.
#
# Truncation and narrowing are different operations on the same lane, and a gate
# that polices one reads as a gate on the path -- exactly the trap
# [[lesson-census-the-operation-kind-not-only-the-sites]] names, which this file's
# own header cites while committing it. What shipped in v132:
#   * WideCharToMultiByte into a too-small buffer returns 0, so the name BLANKED
#     (roster board + ban record) past 12 Cyrillic / 8 hanzi / 6 emoji;
#   * `c < 127 ? c : '?'` SQUASHED the floating nameplate to '????????';
#   * an ASCII filter DROPPED the name from the inventory record.
# None is a resize() or a substr(), so none was visible to the verb above.
# ---------------------------------------------------------------------------

# (a) Encoding is the codec's. WideCharToMultiByte has no legitimate caller
#     outside it -- the codec hand-rolls UTF-8 precisely because that API is
#     undefined on the two inputs we care about (unpaired surrogate, C0), and its
#     too-small-buffer contract ("return 0") is indistinguishable from "empty".
$narrowApi = '\bWideCharToMultiByte\s*\('
$narrowApiFixture = 'int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out, 23, nullptr, nullptr);'
if ($narrowApiFixture -notmatch $narrowApi) {
    Write-Host 'nick_gate: FAIL -- the narrow-API detector does not match its own known-bad fixture'
    exit 1
}

# (b) The ASCII squash, scoped to a nick-shaped value so the event/save/SDK lanes
#     (which are legitimately ASCII) are not swept in. Matches both the '?'
#     substitution and the silent-drop filter form.
$squash = '(?i)\bnick[A-Za-z0-9_]*\b[^;]{0,80}<\s*12[78]\b'
$squashFixture = "for (wchar_t c : nick) s.push_back(c < 128 ? (char)c : '?');"
if ($squashFixture -notmatch $squash) {
    Write-Host 'nick_gate: FAIL -- the squash detector does not match its own known-bad fixture'
    exit 1
}

# (c) A nick buffer declared with a numeric literal. The width and the display
#     policy are then two owners of one axis, and they drifted for five buffers:
#     `char nick[24]` fit 23 ASCII but only 11 Cyrillic, while the policy said 20
#     CHARACTERS. Declare with coop::text::kNickBufBytes.
$litBuf = '(?i)\bchar\s+nick[A-Za-z0-9_]*\s*\[\s*\d+\s*\]'
$litBufFixture = 'char nick[24] = {};'
if ($litBufFixture -notmatch $litBuf) {
    Write-Host 'nick_gate: FAIL -- the literal-buffer detector does not match its own known-bad fixture'
    exit 1
}

foreach ($f in $files) {
    $rel = $f.FullName.Substring($repo.Length + 1)
    if ($rel -ieq $codecRel) { continue }
    $n = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $n++
        if ($line -match '^\s*//') { continue }   # prose may quote the retired forms
        if ($line -match $narrowApi) {
            $violations += [pscustomobject]@{ File = $rel; Line = $n; Text = $line.Trim() }
        } elseif ($line -match $squash) {
            $violations += [pscustomobject]@{ File = $rel; Line = $n; Text = $line.Trim() }
        } elseif ($line -match $litBuf) {
            $violations += [pscustomobject]@{ File = $rel; Line = $n; Text = $line.Trim() }
        }
    }
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

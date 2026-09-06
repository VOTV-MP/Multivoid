# notes_regen.ps1 -- regenerate a LIVE release's body from the notes authority
# file (tools/release/notes/b<N>.md), preserving the machine lines byte-exact.
# This IS the sanctioned correction path from notes/README.md (fix the file ->
# regenerate the body) and the retro path that first backfilled b126/b127.
#
# The body is rebuilt by the ONE writer (New-ReleaseBody) with the source sha +
# sha256 map PARSED from the live body's own machine lines (the API does not
# serve asset hashes; the machine lines are the binding of filename <-> hash).
# After a real edit it fetches the body BACK and asserts: every original
# machine line present verbatim, the completion parser resolves to the same
# sha, and the payload filename appears in the preserved sha256 lines.
#
#   notes_regen.ps1 -TagName v0.9.0n-b126-dev -DryRun   -> writes the would-be
#       body to the path printed, touches nothing public
#   notes_regen.ps1 -TagName v0.9.0n-b126-dev           -> gh release edit +
#       fetch-back verify (run ledger_lint.ps1 after)

param(
    [Parameter(Mandatory)][string]$TagName,
    [string]$Repo = 'VOTV-MP/Multivoid',
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$tag = ConvertFrom-ReleaseTag $TagName
if (-not $tag) { throw "tag '$TagName' fails the grammar" }

$rel = gh api "repos/$Repo/releases/tags/$TagName" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "no live release on $TagName" }
if ($rel.draft) { throw "release on $TagName is a draft -- this tool edits LIVE releases only" }
$oldBody = [string]$rel.body

# --- Parse the machine lines out of the live body (the authority for hashes) --
$srcMatches = [regex]::Matches($oldBody, $script:SourceLineRegex)
if ($srcMatches.Count -ne 1) { throw "live body carries $($srcMatches.Count) source:-grammar lines, expected exactly 1 -- reconcile by hand" }
$sourceSha = [regex]::Match($srcMatches[0].Value, '[0-9a-f]{40}').Value
$machineLines = @($srcMatches[0].Value.Trim())
$shaMap = @{}
foreach ($m in [regex]::Matches($oldBody, $script:Sha256LineRegex)) {
    $line = $m.Value.Trim()
    $machineLines += $line
    if ($line -cmatch '^sha256:\s*(?<h>[0-9a-f]{64})\s\s(?<f>\S+)$') { $shaMap[$Matches['f']] = $Matches['h'] }
}
# Era-aware floor (WP-2 commit 3): legacy bodies (b122..b143) carry the DLL
# pair; zip-era bodies carry exactly one zip. Zero parsed lines = unparseable.
if ($shaMap.Count -lt 1) { throw "parsed no sha256 lines from the live body -- reconcile by hand" }

# --- Notes authority ---------------------------------------------------------
$notesPath = Get-ReleaseNotesPath -N $tag.N
if (-not (Test-Path -LiteralPath $notesPath)) { throw "notes file missing: $notesPath -- write it first (it is the authority)" }
$notes = Get-Content -LiteralPath $notesPath -Raw
$violations = @(Test-ReleaseNotesFormat -Content $notes)
if ($violations.Count -gt 0) { throw "notes format violations: $($violations -join '; ')" }

# --- Rebuild via the ONE writer ---------------------------------------------
$newBody = New-ReleaseBody -SourceSha $sourceSha -Sha256ByFile $shaMap -NotesContent $notes -Dev:$tag.Dev

# Pre-flight asserts on the constructed body (same set the fetch-back re-runs).
foreach ($line in $machineLines) {
    if (-not $newBody.Contains($line)) { throw "constructed body lost machine line verbatim: '$line'" }
}
if ((Get-ReleaseBodySource $newBody) -cne $sourceSha) { throw 'constructed body: completion parser does not resolve the original sha' }
# The payload artifact is era-dependent: the package zip (WP-2 commit 3 onward)
# or the legacy versioned DLL (b122..b143 live bodies).
$payloadCand = @($shaMap.Keys | Where-Object { $_ -clike '*.zip' -or $_ -clike 'multivoid-*.dll' })
if ($payloadCand.Count -lt 1) { throw 'no payload artifact (a *.zip or multivoid-*.dll) among the preserved sha256 lines' }
$payloadName = $payloadCand[0]
if (-not (@($machineLines | Where-Object { $_.Contains($payloadName) }).Count)) { throw "payload name '$payloadName' not present in the preserved sha256 lines" }

$outPath = Join-Path ([System.IO.Path]::GetTempPath()) "multivoid-regen-body-$($tag.N).md"
Set-Content -LiteralPath $outPath -Value $newBody -Encoding utf8 -NoNewline
Write-Host "constructed body written: $outPath"

if ($DryRun) { Write-Host 'DRY RUN -- nothing public touched.'; exit 0 }

# --- The public act + fetch-back verification --------------------------------
gh release edit $TagName --repo $Repo --notes-file $outPath | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'gh release edit failed' }

$back = [string]((gh api "repos/$Repo/releases/tags/$TagName" | ConvertFrom-Json).body)
foreach ($line in $machineLines) {
    if (-not $back.Contains($line)) { throw "FETCH-BACK: machine line lost after edit: '$line'" }
}
if ((Get-ReleaseBodySource $back) -cne $sourceSha) { throw 'FETCH-BACK: completion parser broken after edit' }
$backNotes = Get-ReleaseBodyWhatsNew $back
if ($null -eq $backNotes) { throw 'FETCH-BACK: What''s-new section absent after edit' }
if (-not [string]::Equals((Get-NormalizedProse $backNotes), (Get-NormalizedProse $notes), [System.StringComparison]::Ordinal)) {
    throw 'FETCH-BACK: What''s-new section != notes file (NOTES_DRIFT would fail)'
}
Write-Host "regenerated + verified: $TagName body now carries notes/b$($tag.N).md; run ledger_lint.ps1 to confirm the lane is clean"

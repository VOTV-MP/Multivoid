# publish.ps1 -- the draft-first publish step (design D3), executed by
# release-core's publish job from the MAIN checkout after the judge said
# PUBLISH. Draft -> assets -> sha256 re-download verify -> flip (prerelease
# for -dev) -> read-back asserts (prerelease shape; releases/latest tri-state
# with labeled vacuity). Re-runs delete stale DRAFTS only; live releases are
# never workflow-deleted.

param(
    [Parameter(Mandatory)][string]$TagName,
    [Parameter(Mandatory)][string]$TagSha,
    [Parameter(Mandatory)][string]$ArtifactDir,   # downloaded build-core artifact (payload DLL + xinput1_3.dll)
    [string]$Repo = 'VOTV-MP/Multivoid',
    [string[]]$ExtraBodyLines = @()               # e.g. a 'DRILL' marker line (drill matrix, R23)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$tag = ConvertFrom-ReleaseTag $TagName
if (-not $tag) { throw "tag '$TagName' fails the grammar (the judge should have refused)" }

# --- Assets ---------------------------------------------------------------
$payload = @(Get-ChildItem $ArtifactDir -Filter 'multivoid-*.dll')
$proxy   = @(Get-ChildItem $ArtifactDir -Filter 'xinput1_3.dll')
if ($payload.Count -ne 1) { throw "expected exactly one multivoid-*.dll in $ArtifactDir, found $($payload.Count)" }
if ($proxy.Count -ne 1)   { throw "expected xinput1_3.dll in $ArtifactDir" }
$assets = @($payload[0], $proxy[0])
$shaMap = @{}
foreach ($a in $assets) { $shaMap[$a.Name] = (Get-FileHash -Algorithm SHA256 $a.FullName).Hash.ToLowerInvariant() }
Write-Host "assets: $($assets.Name -join ', ')"

# Filename sanity: the payload DLL must carry the tag's build number (loader
# ranks by the trailing number; a mismatched artifact is the wrong bytes).
if ($payload[0].Name -ne "multivoid-$($tag.Game)-$($tag.N).dll") {
    throw "payload '$($payload[0].Name)' != expected multivoid-$($tag.Game)-$($tag.N).dll"
}

# --- Stale drafts on this tag are a dead run's scratch: delete ------------
$existing = @(gh api "repos/$Repo/releases?per_page=100" --paginate | ConvertFrom-Json)
foreach ($rel in $existing) {
    if ($rel.tag_name -eq $TagName -and $rel.draft) {
        Write-Host "deleting stale draft release id=$($rel.id)"
        gh api -X DELETE "repos/$Repo/releases/$($rel.id)" | Out-Null
    }
    if ($rel.tag_name -eq $TagName -and -not $rel.draft) {
        throw "live release already on $TagName -- the judge should have said ALREADY_PUBLISHED; refusing"
    }
}

# --- Notes (the changelog authority; judge NOTES_OK already gated on this,
# these are the publish-side backstops) -------------------------------------
$notesPath = Get-ReleaseNotesPath -N $tag.N
if (-not (Test-Path -LiteralPath $notesPath)) { throw "notes file missing: $notesPath (the judge should have refused NOTES_OK)" }
$notes = Get-Content -LiteralPath $notesPath -Raw
$notesViolations = @(Test-ReleaseNotesFormat -Content $notes)
if ($notesViolations.Count -gt 0) { throw "notes format violations: $($notesViolations -join '; ')" }
if (-not (Test-Path -LiteralPath 'docs/INSTALL.md')) { throw 'docs/INSTALL.md missing on the main checkout -- the release body links it' }

# --- Draft-first ----------------------------------------------------------
$title = "Multivoid $($tag.Game) b$($tag.N)" + $(if ($tag.Dev) { '-dev' } else { '' })
$body = New-ReleaseBody -SourceSha $TagSha -Sha256ByFile $shaMap -NotesContent $notes -Dev:$tag.Dev -ExtraLines $ExtraBodyLines

# Backstop asserts on the FINAL body: the completion parser is first-match, so
# exactly ONE source:-grammar line may exist; sha256-grammar line count must
# equal the asset count (a notes/template line matching either grammar is a
# fail-closed refusal here, before anything goes public).
$srcMatches = [regex]::Matches($body, $script:SourceLineRegex)
if ($srcMatches.Count -ne 1) { throw "final body carries $($srcMatches.Count) source:-grammar lines, expected exactly 1" }
$shaMatches = [regex]::Matches($body, $script:Sha256LineRegex)
if ($shaMatches.Count -ne $assets.Count) { throw "final body carries $($shaMatches.Count) sha256:-grammar lines, expected $($assets.Count)" }
$bodyFile = Join-Path ([System.IO.Path]::GetTempPath()) "multivoid-relbody-$($tag.N).md"
Set-Content -LiteralPath $bodyFile -Value $body -Encoding utf8 -NoNewline

gh release create $TagName --repo $Repo --draft --title $title --notes-file $bodyFile @($assets.FullName) | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'draft creation failed' }

# --- SHA256 verify: re-download the uploaded assets and hash-compare ------
$dl = Join-Path ([System.IO.Path]::GetTempPath()) "multivoid-relverify-$($tag.N)"
if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
New-Item -ItemType Directory $dl | Out-Null
gh release download $TagName --repo $Repo --dir $dl | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'verify download failed' }
foreach ($a in $assets) {
    $back = (Get-FileHash -Algorithm SHA256 (Join-Path $dl $a.Name)).Hash.ToLowerInvariant()
    if ($back -ne $shaMap[$a.Name]) { throw "sha256 mismatch after upload for $($a.Name): local $($shaMap[$a.Name]) vs uploaded $back" }
    Write-Host "sha256 verified: $($a.Name) = $back"
}

# --- Flip -----------------------------------------------------------------
$flipArgs = @('release', 'edit', $TagName, '--repo', $Repo, '--draft=false')
$flipArgs += $(if ($tag.Dev) { '--prerelease' } else { '--prerelease=false' })
& gh @flipArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'flip (draft=false) failed' }

# --- Read-back asserts ----------------------------------------------------
$rel = gh api "repos/$Repo/releases/tags/$TagName" | ConvertFrom-Json
if ($rel.draft) { throw 'read-back: release still a draft after flip' }
if ([bool]$rel.prerelease -ne $tag.Dev) { throw "read-back: prerelease=$($rel.prerelease) but tag dev=$($tag.Dev)" }
Write-Host "read-back: prerelease == tag shape ($($tag.Dev)) ok"

# releases/latest tri-state with labeled vacuity (R15):
#   dev publish: latest must NOT be this tag (LATEST_404 = no stable yet, logged, never silent-green).
#   stable publish: latest MUST be this tag.
$latestTag = $null
$latestRaw = gh api "repos/$Repo/releases/latest" 2>$null
if ($LASTEXITCODE -eq 0 -and $latestRaw) { $latestTag = ($latestRaw | ConvertFrom-Json).tag_name }
if ($tag.Dev) {
    if ($null -eq $latestTag) { Write-Host 'read-back: LATEST_404 (no stable published yet) -- acceptable, labeled' }
    elseif ($latestTag -eq $TagName) { throw 'read-back: LATEST_IS_THIS -- a dev release surfaced as releases/latest' }
    else { Write-Host "read-back: LATEST_OK_DIFFERENT (latest = $latestTag)" }
} else {
    if ($latestTag -ne $TagName) { throw "read-back: stable published but releases/latest = '$latestTag'" }
    Write-Host 'read-back: releases/latest == this stable ok'
}

Write-Host "published: $title ($TagName @ $TagSha)"
Write-Host 'RITUAL REMINDER: verify the release page, then append the published row to tools/release/LEDGER.tsv (closes state(N) API-free).'
if (-not $tag.Dev) {
    Write-Host 'STABLE: update the master env constants, then run tools/release/verify_latest.ps1:'
    Write-Host "  COOP_LATEST_PROTO=$($tag.N)  COOP_LATEST_MOD=`"$($tag.Game) b$($tag.N)`""
}

# ledger_lint.ps1 -- the ledger drift detector (design D3 LINT). Runs in every
# CI build (ADVISORY there, R28 -- continue-on-error at the workflow layer) and
# in every release judge pass (ENFORCING).
#
# FAIL rows: ledger parse errors; fold grammar faults (ambiguous mint, ...);
#   burn x live-release; a live release whose N has no matching consume row (sha);
#   a live release on a TERMINAL N.
# WARN rows: aged lone consume ("annotate: publish/burn/retract?"); a v-shaped
#   platform tag that fails the grammar (junk, cannot carry an N).
#
# The GitHub API here is a DRIFT DETECTOR only -- the invariant rides
# ledger-recorded history (the deletable-platform-objects lesson). -SkipApi
# turns the API cross-checks into labeled SKIPs (local offline runs).

param(
    [string]$LedgerPath = (Join-Path $PSScriptRoot 'LEDGER.tsv'),
    [string]$Repo = 'VOTV-MP/Multivoid',
    [int]$LoneConsumeWarnDays = 7,
    [switch]$SkipApi
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$failCount = 0; $warnCount = 0
function Fail([string]$msg) { Write-Host "LINT FAIL: $msg"; $script:failCount++ }
function Warn([string]$msg) { Write-Host "LINT WARN: $msg"; $script:warnCount++ }

$ledger = Read-Ledger -Path $LedgerPath
foreach ($e in $ledger.Errors) { Fail "parse: $e" }
$rows = $ledger.Rows

$allNs = @($rows | Select-Object -ExpandProperty N -Unique)
$states = @{}
foreach ($n in $allNs) {
    $st = Get-LedgerState -Rows $rows -N $n
    $states[$n] = $st
    foreach ($f in $st.Faults) { Fail "fold: $f" }
    if ($st.State -eq 'EXPECTED' -and $st.ConsumeDate) {
        $age = (Get-Date) - [datetime]::ParseExact($st.ConsumeDate, 'yyyy-MM-dd', $null)
        if ($age.TotalDays -gt $LoneConsumeWarnDays) {
            Warn "N=$n consumed $($st.ConsumeDate) and never closed -- annotate (publish/burn/retract?)"
        }
    }
}

# --- Public-doc staleness/consistency gates (local, both lanes) -----------
# INSTALL_STALENESS: docs/INSTALL.md + README.md carry NO per-build data (no
# 40/64-hex, no literal multivoid-<target>-<digits>.dll -- placeholders like
# multivoid-<game>-<build>.dll pass) and any multivoid-<x.y.z?>- filename
# context names the CURRENT game target (parsed by the one PS-side parser,
# Get-GameTargetFromCMake, which throws UNREADABLE on parser-miss).
# INSTALL_CONSISTENT: the release-body template's anchor phrases appear
# verbatim (ordinal) in docs/INSTALL.md -- the machine diff between the two
# surfaces that share the install prose.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$gameTarget = Get-GameTargetFromCMake -CMakePath (Join-Path $repoRoot $script:CMakeListsPath)
$installPath = Join-Path $repoRoot 'docs/INSTALL.md'
if (-not (Test-Path -LiteralPath $installPath)) {
    Fail 'INSTALL: docs/INSTALL.md missing (release bodies link it)'
} else {
    $installDoc = Get-Content -LiteralPath $installPath -Raw
    if (-not $installDoc.Contains($script:InstallModFolderAnchor)) { Fail "INSTALL_CONSISTENT: manual-lane mod-folder anchor '$($script:InstallModFolderAnchor)' not found verbatim in docs/INSTALL.md" }
    if (-not $installDoc.Contains($script:InstallDeleteOldAnchor)) { Fail "INSTALL_CONSISTENT: upgrade-from-standalone anchor '$($script:InstallDeleteOldAnchor)' not found verbatim in docs/INSTALL.md" }
    if (-not $installDoc.Contains($gameTarget)) { Fail "INSTALL_STALENESS: docs/INSTALL.md does not name the current game target '$gameTarget' (retarget without doc update?)" }
}
foreach ($docRel in @('docs/INSTALL.md', 'README.md')) {
    $p = Join-Path $repoRoot $docRel
    if (-not (Test-Path -LiteralPath $p)) { continue }   # INSTALL absence already failed above; README always exists
    $doc = Get-Content -LiteralPath $p -Raw
    if ([regex]::IsMatch($doc, '\b[0-9a-f]{64}\b')) { Fail "INSTALL_STALENESS: $docRel carries a 64-hex literal (per-release data lives on the release page)" }
    if ([regex]::IsMatch($doc, '\b[0-9a-f]{40}\b')) { Fail "INSTALL_STALENESS: $docRel carries a 40-hex literal (per-release data lives on the release page)" }
    if ([regex]::IsMatch($doc, "multivoid-$([regex]::Escape($gameTarget))-\d+\.dll")) {
        Fail "INSTALL_STALENESS: $docRel carries a literal build filename (use the multivoid-<game>-<build>.dll placeholder)"
    }
    foreach ($m in [regex]::Matches($doc, 'multivoid-(\d+\.\d+\.\d+[a-z]?)-')) {
        if ($m.Groups[1].Value -cne $gameTarget) {
            Fail "INSTALL_STALENESS: $docRel names game target '$($m.Groups[1].Value)' in a filename context; current target is '$gameTarget'"
        }
    }
}

# --- API cross-checks (drift detection) ----------------------------------
if ($SkipApi) {
    Write-Host 'LINT SKIP: API cross-checks (offline run)'
} else {
    $releases = @()
    try {
        $releases = @(gh api "repos/$Repo/releases?per_page=100" --paginate 2>$null | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0) { throw 'gh api failed' }
    } catch {
        Warn "API unreachable (gh api releases failed) -- drift checks not run this pass"
        $releases = $null
    }
    if ($null -ne $releases) {
        foreach ($rel in $releases) {
            if ($rel.draft) { continue }   # drafts are a run's own scratch, never drift
            $tag = ConvertFrom-ReleaseTag $rel.tag_name
            if (-not $tag) { Warn "live release on non-grammar tag '$($rel.tag_name)' (cannot carry an N)"; continue }
            $st = if ($states.ContainsKey($tag.N)) { $states[$tag.N] } else { Get-LedgerState -Rows $rows -N $tag.N }
            switch ($st.State) {
                'NONE'      { Fail "live release '$($rel.tag_name)' but N=$($tag.N) has NO consume row (unrecorded mint)" }
                'TERMINAL'  { Fail "live release '$($rel.tag_name)' but N=$($tag.N) is terminal ($($st.TerminalClass)) -- reconcile (delete the release?)" }
                default {
                    if ($st.TagName -ne $rel.tag_name) {
                        Fail "live release '$($rel.tag_name)' but the ledger row for N=$($tag.N) names '$($st.TagName)'"
                    } else {
                        $bodySha = Get-ReleaseBodySource $rel.body
                        if ($null -eq $bodySha) { Fail "live release '$($rel.tag_name)' body has no parseable 'source:' key (RELEASE_BODY_UNPARSEABLE)" }
                        elseif ($bodySha -ne $st.SourceSha) { Fail "live release '$($rel.tag_name)' body source $bodySha != ledger sha $($st.SourceSha)" }
                        # NOTES_DRIFT: the git-tracked notes file is the authority;
                        # the live body is a publish-time copy. Labeled tri-state:
                        # section absent / notes file missing / content mismatch --
                        # never a silent pass. This is also the mechanical
                        # enforcement of notes write-once-after-publish.
                        # NOTES_DRIFT is a DETECTOR (WARN), not a refusal.
                        # MEASURED 2026-07-26: BOTH the paginated list endpoint
                        # AND releases/tags/<tag> intermittently serve a STALE
                        # body -- a mismatch reported by the confirm-read below
                        # cleared on 5/5 immediate re-runs with the notes file
                        # untouched. A single pass therefore CANNOT distinguish a
                        # cached read from real drift, so this check must never
                        # refuse a build; the write-once invariant it guards is a
                        # process invariant on already-published prose, not a
                        # publish precondition (judge NOTES_OK + the publish
                        # backstops read LOCAL files and stay FAIL-hard).
                        # The confirm-read stays: it cuts the noise, and a
                        # mismatch surviving both endpoints is worth reading.
                        $notesPath = Get-ReleaseNotesPath -N $tag.N
                        $whatsNew = Get-ReleaseBodyWhatsNew $rel.body
                        if (-not (Test-Path -LiteralPath $notesPath)) {
                            Fail "NOTES_DRIFT: live release '$($rel.tag_name)' but tools/release/notes/b$($tag.N).md is missing"
                        } else {
                            $fileNorm = Get-NormalizedProse (Get-Content -LiteralPath $notesPath -Raw)
                            $agrees = ($null -ne $whatsNew) -and
                                      [string]::Equals($fileNorm, (Get-NormalizedProse $whatsNew), [System.StringComparison]::Ordinal)
                            if (-not $agrees) {
                                # authoritative re-read of THIS release only
                                $confirmBody = $null
                                try {
                                    $confirmRaw = gh api "repos/$Repo/releases/tags/$($rel.tag_name)" 2>$null
                                    if ($LASTEXITCODE -eq 0 -and $confirmRaw) { $confirmBody = ($confirmRaw | ConvertFrom-Json).body }
                                } catch { $confirmBody = $null }
                                if ($null -eq $confirmBody) {
                                    Warn "NOTES_DRIFT: '$($rel.tag_name)' mismatched on the list endpoint and the confirm-read was UNREACHABLE -- not judged this pass (labeled)"
                                } else {
                                    $confirmSection = Get-ReleaseBodyWhatsNew $confirmBody
                                    if ($null -eq $confirmSection) {
                                        Warn "NOTES_DRIFT: live release '$($rel.tag_name)' body has no '## What's new' section (NOTES_SECTION_ABSENT, confirmed) -- regenerate it with notes_regen.ps1, or re-run if the read was cached"
                                    } elseif (-not [string]::Equals($fileNorm, (Get-NormalizedProse $confirmSection), [System.StringComparison]::Ordinal)) {
                                        Warn "NOTES_DRIFT: live release '$($rel.tag_name)' What's-new section != notes/b$($tag.N).md (confirmed on the per-tag endpoint) -- RE-RUN first (both endpoints cache); if it persists, fix the file AND regenerate the body"
                                    } else {
                                        Write-Host "LINT NOTE: '$($rel.tag_name)' list-endpoint body was stale; confirm-read agrees with notes/b$($tag.N).md"
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

Write-Host "ledger_lint: $failCount FAIL, $warnCount WARN ($($rows.Count) rows)"
if ($failCount -gt 0) { exit 1 }
exit 0

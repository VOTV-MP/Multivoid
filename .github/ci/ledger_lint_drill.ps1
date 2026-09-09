# Drill for ledger_lint's README_COUNTS check -- that one row, not the whole
# lint: the ledger fold grammar and the INSTALL rows are undrilled.
#
# README_COUNTS asserts the README's wire-lane counts against protocol.h. It
# went RED unnoticed when a prose sweep reworded the sentence it reads, which is
# the fault this drill exists to catch: a check nobody has seen fail is a check
# nobody can trust. Each case copies the files the row reads into a sandbox,
# mutates one of them, and asserts the row is RED -- and, for the three GREEN
# cases, that it stays quiet when it should.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sandbox = Join-Path ([IO.Path]::GetTempPath()) 'ledger_lint_drill'

$files = @(
    '.github/ci/ledger_lint.ps1',
    '.github/ci/ledger_lib.ps1',
    '.github/ci/LEDGER.tsv',
    'README.md',
    'docs/install.md',
    'src/votv-coop/CMakeLists.txt',
    'src/votv-coop/include/coop/net/protocol.h'
)

function Reset-Sandbox {
    if (Test-Path $sandbox) { Remove-Item -Recurse -Force $sandbox }
    foreach ($f in $files) {
        $dst = Join-Path $sandbox $f
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item (Join-Path $repo $f) $dst
    }
}

function Invoke-Gate {
    # -SkipApi: the drill is offline, and README_COUNTS reads only the worktree.
    $out = pwsh -NoProfile -File (Join-Path $sandbox '.github/ci/ledger_lint.ps1') -SkipApi 2>&1
    return ($out | Out-String)
}

function Edit-File([string]$rel, [string]$from, [string]$to) {
    $p = Join-Path $sandbox $rel
    $s = [IO.File]::ReadAllText($p)
    if (-not $s.Contains($from)) { throw "mutant anchor missing in ${rel}: $from" }
    [IO.File]::WriteAllText($p, $s.Replace($from, $to))
}

$results = @()
function Check([string]$name, [bool]$wantRed, [string]$out) {
    $red = $out -match 'README_COUNTS'
    $ok = ($red -eq $wantRed)
    $want = if ($wantRed) { 'RED' } else { 'GREEN' }
    $got = if ($red) { 'RED' } else { 'GREEN' }
    $script:results += [pscustomobject]@{ Name = $name; Pass = $ok }
    $line = ($out -split "`n" | Select-String 'README_COUNTS' | Select-Object -First 1)
    Write-Host ("[{0}] {1,-34} want {2,-5} got {3,-5} {4}" -f `
        $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $want, $got, $line)
}

# The tree as it stands: the row must be silent.
Reset-Sandbox
Check 'baseline (tree as it stands)' $false (Invoke-Gate)

# Negative control -- prose the row has no business reading.
Reset-Sandbox
Edit-File 'README.md' 'Each machine''s engine' 'Every machine''s engine'
Check 'negative control (unrelated edit)' $false (Invoke-Gate)

# The reliable-kind claim drifts from the enum.
Reset-Sandbox
Edit-File 'README.md' '122 message kinds' '121 message kinds'
Check 'M1 kinds claim off by one' $true (Invoke-Gate)

# The stream claim drifts from the enum.
Reset-Sandbox
Edit-File 'README.md' '12 unreliable pose and state streams' '13 unreliable pose and state streams'
Check 'M2 stream claim off by one' $true (Invoke-Gate)

# A new MsgType lands and the README says nothing: the decomposition stops
# balancing, so an added lane cannot pass as one of the existing ones.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' '    VoiceFrame = 64,' `
    "    VoiceFrame = 64,`n`n    // drill-only lane.`n    DrillLane = 65,"
Check 'M3 new MsgType, README silent' $true (Invoke-Gate)

# A non-stream member is renamed: the classification is checked, not assumed.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' '    VoiceFrame = 64,' '    VoiceStream = 64,'
Check 'M4 non-stream member renamed' $true (Invoke-Gate)

# A new ReliableKind lands and the README says nothing.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' 'enum class ReliableKind : uint8_t {' `
    "enum class ReliableKind : uint8_t {`n    // drill-only kind.`n    DrillKind = 254,"
Check 'M5 new ReliableKind, README silent' $true (Invoke-Gate)

# The sentence is reworded away -- the fault that opened this debt.
Reset-Sandbox
Edit-File 'README.md' 'Transport is GameNetworkingSockets: 12 unreliable pose and state streams, a voice stream beside' `
    'Transport is GameNetworkingSockets, carrying pose streams and a reliable channel beside'
Check 'M6 sentence reworded away' $true (Invoke-Gate)

# The count stated twice: two claims that can drift apart, of which the row
# would only ever read the first.
Reset-Sandbox
Edit-File 'README.md' 'performs.' 'performs. The wire carries 122 message kinds.'
Check 'M7 kinds claim stated twice' $true (Invoke-Gate)

# An enumerator written WITHOUT a value is still an enumerator: C++ increments it
# implicitly. Counting it is what keeps the next case honest, so this one is
# GREEN -- the parser must accept the shape, not merely survive it.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' '    VoiceFrame = 64,' '    VoiceFrame,'
Check 'M8 implicit value still counted' $false (Invoke-Gate)

# ...so a new lane written that way cannot slip past the balance. The first
# version of this parser demanded `= <n>` and would have passed this silently.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' '    VoiceFrame = 64,' `
    "    VoiceFrame = 64,`n`n    // drill-only lane, no explicit value.`n    DrillLane,"
Check 'M9 new implicit MsgType, README silent' $true (Invoke-Gate)

# A row the parser cannot read is reported, never skipped: dropping one quietly
# is indistinguishable from the enumerator not being there.
Reset-Sandbox
Edit-File 'src/votv-coop/include/coop/net/protocol.h' '    VoiceFrame = 64,' `
    "    VoiceFrame = 64, DrillPair = 65,"
Check 'M10 unreadable enum row is reported' $true (Invoke-Gate)

Remove-Item -Recurse -Force $sandbox
$failed = @($results | Where-Object { -not $_.Pass })
Write-Host ''
Write-Host ("ledger_lint_drill: {0} of {1} checks passed" -f ($results.Count - $failed.Count), $results.Count)
if ($failed.Count -gt 0) { exit 1 }
exit 0

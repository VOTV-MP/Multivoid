# tag_regex_selftest.ps1 -- the release-lane grammar fixtures (design D3 + the
# 2026-07-26 release-body/notes/install gates): MUST-MATCH rows with expected
# parse fields + MUST-REFUSE near-twins, for the tag grammar AND the notes
# format lint AND the install-doc staleness patterns AND the body writer's
# machine-key invariants. Runs in every release judge pass and locally.
# Exit 0 = all rows hold; exit 1 = a grammar/gate drifted.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$fail = 0

$mustMatch = @(
    @{ Tag = 'v0.9.0n-b122';     Game = '0.9.0n'; N = 122; Dev = $false },
    @{ Tag = 'v0.9.0n-b123-dev'; Game = '0.9.0n'; N = 123; Dev = $true },
    @{ Tag = 'v1.0.0-b200';      Game = '1.0.0';  N = 200; Dev = $false }
)
foreach ($c in $mustMatch) {
    $p = ConvertFrom-ReleaseTag $c.Tag
    if (-not $p) { Write-Host "FAIL must-match refused: $($c.Tag)"; $fail++; continue }
    if ($p.Game -ne $c.Game -or $p.N -ne $c.N -or $p.Dev -ne $c.Dev) {
        Write-Host "FAIL must-match parsed wrong: $($c.Tag) -> game=$($p.Game) n=$($p.N) dev=$($p.Dev)"; $fail++
    } else {
        Write-Host "ok   match  $($c.Tag) -> game=$($p.Game) n=$($p.N) dev=$($p.Dev)"
    }
}

$mustRefuse = @(
    'v0.9.0n-b12-devx',   # trailing junk after -dev
    'v0.9.0nb12',         # missing -b separator
    'b12dev',             # no v<game> prefix
    'b122',               # bare number
    'v0.9.0n-b012',       # leading zero
    'v0.9.0n-b0',         # zero build
    'v0.9.0n-b12-DEV',    # case matters
    'v0.9.0n-b12 '        # trailing whitespace
)
foreach ($t in $mustRefuse) {
    $p = ConvertFrom-ReleaseTag $t
    if ($p) { Write-Host "FAIL must-refuse matched: '$t'"; $fail++ }
    else    { Write-Host "ok   refuse '$t'" }
}

# --- Notes-format fixtures (judge NOTES_OK / publish backstop) -------------
function Assert-Case([bool]$Cond, [string]$Label) {
    if ($Cond) { Write-Host "ok   $Label" } else { Write-Host "FAIL $Label"; $script:fail++ }
}

Assert-Case (@(Test-ReleaseNotesFormat -Content "- a change`n- another change`n").Count -eq 0) 'notes: plain bullets pass'
Assert-Case (@(Test-ReleaseNotesFormat -Content '').Count -gt 0) 'notes: empty refused'
Assert-Case (@(Test-ReleaseNotesFormat -Content "## What's new`n- x").Count -gt 0) 'notes: leading heading refused (template owns the heading)'
Assert-Case (@(Test-ReleaseNotesFormat -Content "- fine`nsource: 0123456789abcdef0123456789abcdef01234567").Count -gt 0) 'notes: source:-grammar line refused (first-match shadowing)'
Assert-Case (@(Test-ReleaseNotesFormat -Content ("- fine`nsha256: " + ('a' * 64) + '  x.dll')).Count -gt 0) 'notes: sha256:-grammar line refused'

# --- Body-writer machine-key invariants: BOTH artifact eras ------------------
# The writer's era switch is data-driven (what the sha map holds), so both eras
# get fixtures: the zip era is what publish emits from WP-2 commit 3 onward; the
# legacy two-DLL era is what notes_regen rebuilds for the LIVE b122..b143 bodies.
$fixtureSha = '0123456789abcdef0123456789abcdef01234567'
$fixtureMap = @{ 'Pelmentor-Multivoid-0.9.999.zip' = ('a' * 64) }
$fixtureBody = New-ReleaseBody -SourceSha $fixtureSha -Sha256ByFile $fixtureMap -NotesContent '- a change' -Dev
Assert-Case ((Get-ReleaseBodySource $fixtureBody) -eq $fixtureSha) 'body: completion parser finds the one source: key'
Assert-Case ([regex]::Matches($fixtureBody, $script:SourceLineRegex).Count -eq 1) 'body: exactly one source:-grammar line'
Assert-Case ([regex]::Matches($fixtureBody, $script:Sha256LineRegex).Count -eq 1) 'body: sha256-grammar line count == asset count (one zip)'
Assert-Case ((Get-NormalizedProse (Get-ReleaseBodyWhatsNew $fixtureBody)) -ceq '- a change') 'body: What''s-new section round-trips ordinal-exact'
Assert-Case ($fixtureBody.Contains($script:InstallModFolderAnchor)) 'body: install block carries the manual-lane mod-folder anchor'
Assert-Case ($fixtureBody.Contains($script:InstallDeleteOldAnchor)) 'body: install block carries the upgrade-from-standalone anchor'
Assert-Case ($null -eq (Get-ReleaseBodyWhatsNew "Development build`nsource: $fixtureSha")) 'body: legacy no-section body -> labeled ABSENT (null), not empty'
$legacyMap = @{ 'multivoid-0.9.0n-999.dll' = ('a' * 64); 'xinput1_3.dll' = ('b' * 64) }
$legacyBody = New-ReleaseBody -SourceSha $fixtureSha -Sha256ByFile $legacyMap -NotesContent '- a change' -Dev
Assert-Case ([regex]::Matches($legacyBody, $script:Sha256LineRegex).Count -eq 2) 'body(legacy): sha256-grammar line count == 2 (DLL pair)'
Assert-Case ($legacyBody.Contains('You need **both** files below')) 'body(legacy): two-DLL install prose preserved (frozen literals)'
Assert-Case ($legacyBody.Contains('WindowsNoEditor\VotV\Binaries\Win64')) 'body(legacy): old folder path survives as a frozen literal'
$eraMissThrew = $false
try { New-ReleaseBody -SourceSha $fixtureSha -Sha256ByFile @{ 'weird.txt' = ('c' * 64) } -NotesContent '- x' | Out-Null }
catch { $eraMissThrew = $true }
Assert-Case $eraMissThrew 'body: a sha map matching neither era is refused (fail-closed, no default prose)'

# --- Install-staleness pattern fixtures (both outcomes through the REAL
# patterns; the game-target parser-miss tri-state) ---------------------------
$t = Get-GameTargetFromCMake   # the real code authority -- also proves the parser matches the live CMakeLists
Assert-Case ($t -cmatch '^\d+\.\d+\.\d+[a-z]?$') "target parser: live CMakeLists yields a well-formed target ('$t')"
$missTmp = Join-Path ([System.IO.Path]::GetTempPath()) 'multivoid-selftest-cmake-miss.txt'
Set-Content -LiteralPath $missTmp -Value 'set(SOMETHING_ELSE "x")' -Encoding utf8
$missThrew = $false
try { Get-GameTargetFromCMake -CMakePath $missTmp | Out-Null } catch { $missThrew = $_.Exception.Message.Contains('UNREADABLE') }
Assert-Case $missThrew 'target parser: parser-miss throws labeled UNREADABLE (never $null into a compare)'

$banLiteral = "multivoid-$([regex]::Escape($t))-\d+\.dll"
Assert-Case ([regex]::IsMatch("multivoid-$t-128.dll", $banLiteral)) 'staleness: literal build filename is banned'
Assert-Case (-not [regex]::IsMatch("multivoid-$t-<N>.dll", $banLiteral)) 'staleness: <N> placeholder passes'
Assert-Case (-not [regex]::IsMatch("multivoid-$t-<build>.dll", $banLiteral)) 'staleness: <build> placeholder passes'
Assert-Case (-not [regex]::IsMatch(("multivoid-$t-128.dll").ToUpperInvariant(), $banLiteral)) 'staleness: ban is case-sensitive by construction (uppercase twin does not match, and real filenames are lowercase)'
Assert-Case ([regex]::IsMatch('see commit 0123456789abcdef0123456789abcdef01234567 for', '\b[0-9a-f]{40}\b')) 'staleness: 40-hex known-positive (a commit citation WOULD fail -- the docs must not cite raw shas)'
Assert-Case (-not [regex]::IsMatch('multivoid-0.9.0n-<build>.dll and xinput1_3.dll', '\b[0-9a-f]{40}\b')) 'staleness: normal install prose carries no hex false-positive'

if ($fail -gt 0) { Write-Host "tag_regex_selftest: $fail row(s) FAILED"; exit 1 }
Write-Host 'tag_regex_selftest: all rows hold'
exit 0

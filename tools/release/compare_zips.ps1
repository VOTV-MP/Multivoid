# compare_zips.ps1 -- does the PUBLISHED package carry the same tree as the one we tested?
#
# WHY A WHOLE-FILE HASH IS THE WRONG INSTRUMENT HERE. The zip on the release page carries the CI
# CACHELESS REBUILD of the tagged source; a zip tested locally carries a locally compiled
# main.dll. Same source, different compiler run -- so the two files can never be byte-equal, and
# `Get-FileHash` on the zips answers "are these the same file" (no, always) instead of the
# question actually being asked: "did the pipeline package the same TREE from the same source?"
#
# TWO DIFFERENCE CLASSES ARE EXPECTED AND NEITHER IS A DEFECT (both MEASURED 2026-09-01 on the
# b150 release, which is where this script's expected-diff list comes from):
#
#   1. `mod/dlls/main.dll` differs in bytes. It is the CI rebuild. That is the designed lane.
#   2. TEXT files differ in bytes while being identical as text -- git checks these out LF in
#      this worktree and CRLF on the GitHub runner, so LICENSE / README.md /
#      THIRD-PARTY-NOTICES.md each grow by exactly their line count (21 / 69 / 1502 on b150).
#      The first version of this script did not know that and called a perfectly healthy release
#      FAIL. Same LF-vs-CRLF asymmetry that made the release fingerprint look stale the same
#      morning -- see memory/lesson-a-file-hash-gate-can-only-be-minted-where-it-is-checked.md.
#
# So text entries are compared with newlines NORMALIZED, and only a real text change is a
# finding. Binary entries are compared byte-for-byte, with main.dll the one allowed exception.
param(
    [Parameter(Mandatory)][string]$Approved,   # the zip that was hands-on tested
    [Parameter(Mandatory)][string]$Published,  # the zip downloaded from the release page
    # main.dll is expected to differ (CI rebuild). Pass an empty array to demand byte-equality of
    # everything -- e.g. when comparing two zips that came from ONE build.
    [string[]]$AllowBinaryDiff = @('mod/dlls/main.dll')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Entries compared as TEXT. Everything else is compared as bytes -- an extension list would be a
# guess, and a package's manifest is small enough to name its text members outright.
$script:TextEntries = @(
    'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.md', 'manifest.json', 'mod/enabled.txt'
)

function Get-Sha256([byte[]]$bytes) {
    [BitConverter]::ToString(
        ([Security.Cryptography.SHA256]::Create()).ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
}

function Read-ZipEntries([string]$path) {
    $z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $path))
    try {
        $out = @{}
        foreach ($e in $z.Entries) {
            if ($e.FullName.EndsWith('/')) { continue }
            $ms = New-Object IO.MemoryStream
            $e.Open().CopyTo($ms)
            $bytes = $ms.ToArray()
            $rec = @{ len = $bytes.Length; sha = Get-Sha256 $bytes; textSha = $null }
            if ($script:TextEntries -contains $e.FullName) {
                $norm = ([Text.Encoding]::UTF8.GetString($bytes)) -replace "`r`n", "`n"
                $rec.textSha = Get-Sha256 ([Text.Encoding]::UTF8.GetBytes($norm))
            }
            $out[$e.FullName] = $rec
        }
        return $out
    } finally { $z.Dispose() }
}

$a = Read-ZipEntries $Approved
$b = Read-ZipEntries $Published
Write-Host "approved  = $Approved"
Write-Host "published = $Published"
Write-Host ''

$identical = 0; $textOnly = @(); $binDiff = @(); $realDiff = @(); $onlyA = @(); $onlyB = @()
foreach ($n in ($a.Keys + $b.Keys | Sort-Object -Unique)) {
    if (-not $a.ContainsKey($n)) { $onlyB += $n; Write-Host "  ONLY-PUBLISHED $n" -ForegroundColor Red; continue }
    if (-not $b.ContainsKey($n)) { $onlyA += $n; Write-Host "  ONLY-APPROVED  $n" -ForegroundColor Red; continue }
    $ea = $a[$n]; $eb = $b[$n]
    if ($ea.sha -eq $eb.sha) {
        $identical++
        Write-Host ("  SAME      {0,-40} {1}" -f $n, $ea.sha.Substring(0, 16))
    } elseif ($null -ne $ea.textSha -and $ea.textSha -eq $eb.textSha) {
        $textOnly += $n
        Write-Host ("  EOL-ONLY  {0,-40} text identical, +{1} B (LF here, CRLF on the runner)" -f `
            $n, ($eb.len - $ea.len)) -ForegroundColor DarkGray
    } elseif ($AllowBinaryDiff -contains $n) {
        $binDiff += $n
        Write-Host ("  REBUILT   {0,-40} {1} -> {2}  ({3} -> {4} B)" -f `
            $n, $ea.sha.Substring(0, 16), $eb.sha.Substring(0, 16), $ea.len, $eb.len) -ForegroundColor Cyan
    } else {
        $realDiff += $n
        Write-Host ("  DIFF      {0,-40} {1} -> {2}" -f `
            $n, $ea.sha.Substring(0, 16), $eb.sha.Substring(0, 16)) -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host ("identical: {0}   eol-only: {1}   rebuilt: {2}   unexplained: {3}   only-one-side: {4}" -f `
    $identical, $textOnly.Count, $binDiff.Count, $realDiff.Count, ($onlyA.Count + $onlyB.Count))

if ($onlyA.Count -or $onlyB.Count) {
    Write-Host 'VERDICT: FAIL -- the two zips do not carry the same tree.' -ForegroundColor Red
    exit 1
}
if ($realDiff.Count) {
    Write-Host ("VERDICT: FAIL -- content differs where it should not: {0}" -f ($realDiff -join ', ')) -ForegroundColor Red
    exit 1
}
if ($binDiff.Count -eq 0 -and $textOnly.Count -eq 0) {
    Write-Host 'VERDICT: BYTE-IDENTICAL -- both zips came from one build.' -ForegroundColor Green
} else {
    Write-Host 'VERDICT: PASS -- same tree; only the CI-rebuilt payload and line endings differ.' -ForegroundColor Green
}
exit 0

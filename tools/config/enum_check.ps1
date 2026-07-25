# tools/config/enum_check.ps1 -- the T2 enum-completeness instrument
# (design research/findings/tooling/votv-ini-config-registry-DESIGN-2026-07-24.md §5).
#
# Proves the config_registry row table enumerates EVERY ini key the tree reads or
# writes:
#   1. tree-parse all read/write-API call sites; literal key args -> the KEY SET;
#   2. a NON-literal key arg must match one of the known composed-key producers
#      (fail-closed otherwise);
#   3. diff the key set against the registry rows (both directions must be empty;
#      the composed ui.font.<role> family is carried by the registry BY REFERENCE,
#      so the fonts producer maps to the role list, not to literal rows).
#
# Must-FAIL controls (a run that reports zero must first prove it can report
# non-zero):  -OmitKey <key>   simulates a missing registry row;
#             -InjectNonLiteral simulates an unknown composed-key producer.
#
# Retires when arc 3's const-Row& ratchet makes unregistered keys uncompilable.
#
# NOTE: every comparison here is case-SENSITIVE on purpose
# (lesson_powershell_defaults_are_case_insensitive_everywhere).

param(
    [string[]]$OmitKey = @(),
    [switch]$InjectNonLiteral
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$srcDirs = @("$root\src\votv-coop\src", "$root\src\votv-coop\include")
$registryCpp = "$root\src\votv-coop\src\coop\config\config_registry.cpp"

# ---- 1. registry rows -------------------------------------------------------
$registryKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($line in Get-Content $registryCpp) {
    if ($line -cmatch '^\s*(?:Row\{|F\(|S\(|I\(|Fl\(|E\()"(?<k>[^"]+)"') {
        [void]$registryKeys.Add($Matches['k'])
    }
}
foreach ($k in $OmitKey) { [void]$registryKeys.Remove($k) }
Write-Host "registry rows: $($registryKeys.Count)"

# ---- 2. call-site extraction ------------------------------------------------
# Key-arg position per API: 1 = first arg, 2 = second arg.
$apis = @(
    @{ n = 'ReadIniValue';  pos = 1 },
    @{ n = 'IsIniKeyTrue';  pos = 1 },
    @{ n = 'WriteIniValue'; pos = 1 },
    @{ n = 'ResolveFlag';   pos = 1 },
    @{ n = 'ResolveInt';    pos = 1 },
    @{ n = 'ResolveFloat';  pos = 1 },
    @{ n = 'ResolveEnum';   pos = 1 },
    @{ n = 'EnvOrIniBool';  pos = 2 },   # dies with the arc-2 conversions; kept for the transition
    @{ n = 'DeviceCombo';   pos = 2 },   # voice_panel combo: literal key at each call site
    @{ n = 'LookupTriState'; pos = 1 }   # config.cpp-internal flag core ("enabled" lives there)
)

# Known composed-key producers: a non-literal key arg is legal ONLY here.
#   fonts.cpp RoleIniKey(..)   -> the registry's kFontRoleKeys by reference
#   voice_panel.cpp DeviceCombo helper body forwards its literal-at-call-site arg
#   voice_chat.cpp EnvOrIniBool helper body forwards its literal-at-call-site arg
$producerOk = @(
    @{ file = 'fonts.cpp';       argPattern = 'RoleIniKey\(' },
    @{ file = 'config.cpp';      argPattern = '^key$' },       # public-API forwarders into the core

    @{ file = 'voice_panel.cpp'; argPattern = '^iniKey$' },
    @{ file = 'voice_chat.cpp';  argPattern = '^iniKey$' }
)

$extracted = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$badSites = @()
$siteCount = 0

$files = Get-ChildItem -Recurse -Path $srcDirs -Include *.cpp, *.h -File
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    foreach ($api in $apis) {
        # Immediate '(' on purpose: `Name (x)` inside prose/comments must not match
        # (quiescence_drain.cpp carries exactly that comment shape).
        $rx = [regex]("(?<![A-Za-z0-9_])" + $api.n + "\(")
        foreach ($m in $rx.Matches($text)) {
            # Slice from the '(' and split top-level args (enough for arg 1-2).
            $tail = $text.Substring($m.Index + $m.Length, [Math]::Min(220, $text.Length - $m.Index - $m.Length))
            # Skip declarations/definitions: first token is a type keyword.
            if ($tail -cmatch '^\s*(const\s|std::|bool\s|int\s|char\s|void\s|\))') { continue }
            $args_ = @(); $depth = 0; $cur = ''
            foreach ($ch in $tail.ToCharArray()) {
                if ($ch -eq '(') { $depth++ }
                elseif ($ch -eq ')') { if ($depth -eq 0) { $args_ += $cur; break }; $depth-- }
                if ($ch -eq ',' -and $depth -eq 0) { $args_ += $cur; $cur = ''; continue }
                $cur += $ch
            }
            if ($args_.Count -lt $api.pos) { continue }
            $arg = $args_[$api.pos - 1].Trim()
            $siteCount++
            if ($arg -cmatch '^"(?<k>[^"]*)"') { [void]$extracted.Add($Matches['k']); continue }
            # `WriteIniValue('%s')` etc INSIDE log-format strings: a mention, not a call.
            if ($arg -cmatch "^'") { continue }
            # non-literal: must match a known producer
            $ok = $false
            foreach ($p in $producerOk) {
                if ($f.Name -ceq $p.file -and $arg -cmatch $p.argPattern) { $ok = $true; break }
            }
            if (-not $ok) { $badSites += "$($f.Name): $($api.n)($arg ...)" }
        }
    }
}
if ($InjectNonLiteral) { $badSites += 'INJECTED.cpp: ReadIniValue(mysteryKey ...)' }

Write-Host "call sites scanned: $siteCount; literal keys extracted: $($extracted.Count)"

# ---- 3. diff ----------------------------------------------------------------
$fail = $false
if ($badSites.Count -gt 0) {
    Write-Host "FAIL: non-literal key args outside the producer whitelist:" -ForegroundColor Red
    $badSites | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $fail = $true
}
$notInRegistry = @($extracted | Where-Object { -not $registryKeys.Contains($_) } | Sort-Object)
$notInTree     = @($registryKeys | Where-Object { -not $extracted.Contains($_) } | Sort-Object)
if ($notInRegistry.Count -gt 0) {
    Write-Host "FAIL: keys read/written in the tree but MISSING from the registry:" -ForegroundColor Red
    $notInRegistry | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $fail = $true
}
if ($notInTree.Count -gt 0) {
    Write-Host "FAIL: registry rows with NO call site in the tree (stale row?):" -ForegroundColor Red
    $notInTree | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $fail = $true
}
if ($fail) { Write-Host 'enum_check: FAIL'; exit 1 }
Write-Host "enum_check: PASS (extracted == registry, $($extracted.Count) keys; producers whitelisted)"
exit 0

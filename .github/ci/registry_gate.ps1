# tools/config/registry_gate.ps1 -- the STANDING config-registry CI gate
# (ini rework arc 3 C5; design research/findings/tooling/
# votv-ini-arc3-impl-DESIGN-2026-07-25.md). Replaces the retired
# enum_check.ps1's non-inherited jobs -- the inheritor map lives in that design
# (forward literal-keys=>rows direction is now the COMPILER: handles are
# registry-minted, a string key does not build; recorded compile-proof
# C2664/C2665).
#
# Jobs (each with a fixture-injected MUST-FIRE control run every invocation --
# a green run first proves every detector can go red):
#   1. DEAD ROWS -- every row ident in config_registry_rows.inc must be
#      referenced at least once outside the registry's own three files.
#      Census token = the QUALIFIED TAIL `rows::<ident>` (case-SENSITIVE,
#      Ordinal): bare idents collide with locals (`save`, `enabled`, `port`);
#      the tail survives any OUTER-namespace alias, and aliasing/using the
#      rows namespace itself is FORBIDDEN (job 3), which is what makes this
#      census exact. CFG_FONTROLE rows are referenced by INDEX through the one
#      FontRoleRow(door) -- the family counts as alive iff that door has a
#      reference outside the registry TU.
#      Known imprecision (accepted): a `rows::<ident>` inside a comment counts
#      as a reference; it can only make a dead row look alive, never fail a
#      live one, and the forbidden-alias job keeps code references canonical.
#   2. WRITE-ONLY ROWS -- a row whose every reference is the first arg of
#      WriteIniValue( must be in the committed allowlist below (each entry
#      carries its review reason). SYMMETRIC REAPING: an allowlist entry with
#      no matching row FAILS, and one whose row gained a reader FAILS -- the
#      table can only shrink.
#   3. FORBIDDEN ALIAS VOCABULARY for the rows namespace -- using-directive,
#      using-declaration, namespace-alias (three patterns; any would blind the
#      job-1 census).
#   4. RATCHET SURFACE -- the public coop/config/config.h must not re-grow a
#      string-keyed read/write declaration (the standing robot twin of the
#      one-shot compile-proof).
#
# Case-sensitivity: every match is -cmatch / Ordinal on purpose
# (lesson_powershell_defaults_are_case_insensitive_everywhere).
# Exit codes are EXPLICIT (lesson_gha_pwsh_step_exits_with_last_child_code).

param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }

$incPath  = Join-Path $Root 'src\votv-coop\include\coop\config\config_registry_rows.inc'
$cfgHdr   = Join-Path $Root 'src\votv-coop\include\coop\config\config.h'
$srcDirs  = @((Join-Path $Root 'src\votv-coop\src'), (Join-Path $Root 'src\votv-coop\include'))
# The registry's own three files self-reference every ident (decl/def/table)
# and must not satisfy the census.
$selfFiles = @('config_registry.h', 'config_registry.cpp', 'config_registry_rows.inc')

$violations = New-Object System.Collections.Generic.List[string]
$controlFailures = New-Object System.Collections.Generic.List[string]

# ---- WRITE-ONLY allowlist (job 2). Review reason required per entry. -------
# player_skin: an Identity row; the product WRITE rides the typed door (the
# local_body skin picker) while the READ is the mint machinery's internal string
# read inside the config TU by design (the IdentityRow handle is write-only on
# purpose -- config_registry.h). player_guid was the other entry and is GONE with
# its row (b144): the durable identity is a keypair in multivoid_identity.key, so
# nothing mints a guid into the ini any more. The gate caught this leftover
# itself -- an allowlist entry naming no row is exactly what its reap arm is for.
$writeOnlyAllow = @{
    'player_skin' = 'identity mint + skin picker: read internally by the config TU machinery'
}

# ---- parse the row list -----------------------------------------------------
if (-not (Test-Path $incPath)) { Write-Host "registry_gate: FAIL -- missing $incPath"; exit 1 }
$rowIdents = New-Object System.Collections.Generic.List[string]
$fontRoleIdents = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content $incPath) {
    if ($line -cmatch '^\s*CFG_(?<kind>FLAG|INT|FLOAT|ENUM|STRING_GATED|STRING|IDENTITY|FONTROLE)\(\s*(?<ident>[A-Za-z0-9_]+)\s*,') {
        if ($Matches['kind'] -ceq 'FONTROLE') { $fontRoleIdents.Add($Matches['ident']) }
        else { $rowIdents.Add($Matches['ident']) }
    }
}
if ($rowIdents.Count -eq 0) { Write-Host 'registry_gate: FAIL -- parsed 0 rows from the .inc'; exit 1 }
Write-Host "registry_gate: rows=$($rowIdents.Count) fontRoleRows=$($fontRoleIdents.Count)"

# ---- census helpers (pure functions over passed-in text; the fixture
# controls feed them synthetic corpora through the SAME code path) ------------
$refRx      = [regex]'rows::(?<n>[A-Za-z0-9_]+)'
# A reference is a WRITE ref iff the qualified name is the first argument of
# WriteIniValue( -- i.e. the text immediately before the match is the open
# paren plus namespace qualification only.
$writeCtxRx = [regex]'WriteIniValue\(\s*(::)?[A-Za-z0-9_:]*$'

function Get-RowRefs([string[]]$texts) {
    # -> hashtable ident -> @{ total = n; write = n }
    $refs = @{}
    foreach ($t in $texts) {
        foreach ($m in $refRx.Matches($t)) {
            $n = $m.Groups['n'].Value
            if (-not $refs.ContainsKey($n)) { $refs[$n] = @{ total = 0; write = 0 } }
            $refs[$n].total++
            $preStart = [Math]::Max(0, $m.Index - 96)
            $pre = $t.Substring($preStart, $m.Index - $preStart)
            if ($pre -cmatch $writeCtxRx) { $refs[$n].write++ }
        }
    }
    return $refs
}

$aliasRx = @(
    @{ name = 'using-directive';   rx = [regex]'using\s+namespace\s+[A-Za-z0-9_:\s]*\brows\s*;' },
    @{ name = 'using-declaration'; rx = [regex]'using\s+[A-Za-z0-9_:]*\brows::[A-Za-z0-9_]+\s*;' },
    @{ name = 'namespace-alias';   rx = [regex]'namespace\s+[A-Za-z0-9_]+\s*=\s*[A-Za-z0-9_:]*\brows\s*;' }
)

$ratchetRx = [regex]'(?m)^\s*(bool|int|long|float|std::string)\s+(IsIniKeyTrue|ReadIniValue|WriteIniValue)\s*\(\s*const\s+char\s*\*'

# ---- fixture MUST-FIRE controls (every run) ---------------------------------
# 1: dead-row detector fires on an unreferenced fixture ident.
$fixRefs = Get-RowRefs @('void f() { (void)coop::config_registry::rows::zz_fix_alive; }')
if ($fixRefs.ContainsKey('zz_fix_dead')) { $controlFailures.Add('dead-row control: fixture ident spuriously referenced') }
if (-not $fixRefs.ContainsKey('zz_fix_alive')) { $controlFailures.Add('dead-row control: live fixture reference NOT counted') }
# 2: write-only classifier fires on a write-only fixture corpus.
$fixWo = Get-RowRefs @('coop::config::WriteIniValue(coop::config_registry::rows::zz_fix_wo, "1");')
if (-not ($fixWo.ContainsKey('zz_fix_wo') -and $fixWo['zz_fix_wo'].write -eq $fixWo['zz_fix_wo'].total)) {
    $controlFailures.Add('write-only control: fixture write reference not classified as write')
}
# ...and a READ ref must NOT classify as write (the reaper direction).
$fixRd = Get-RowRefs @('bool v = coop::config::ResolveFlag(coop::config_registry::rows::zz_fix_rd);')
if (-not ($fixRd.ContainsKey('zz_fix_rd') -and $fixRd['zz_fix_rd'].write -eq 0)) {
    $controlFailures.Add('write-only control: fixture read reference misclassified as write')
}
# 3: the three alias patterns each fire on their fixture line.
$aliasFixtures = @(
    'using namespace coop::config_registry::rows;',
    'using coop::config_registry::rows::save;',
    'namespace r = coop::config_registry::rows;'
)
for ($i = 0; $i -lt 3; $i++) {
    if (-not ($aliasFixtures[$i] -cmatch $aliasRx[$i].rx)) {
        $controlFailures.Add("alias control: pattern '$($aliasRx[$i].name)' did not fire on its fixture")
    }
}
# 4: ratchet-surface pattern fires on a fixture string-keyed declaration.
if (-not ('bool IsIniKeyTrue(const char* key);' -cmatch $ratchetRx)) {
    $controlFailures.Add('ratchet control: string-keyed decl fixture did not fire')
}
if (-not ('bool WriteIniValue(const char* key, const char* value);' -cmatch $ratchetRx)) {
    $controlFailures.Add('ratchet control: string-keyed write decl fixture did not fire')
}
# 5: allowlist reaping fires on a ghost entry (checked against the REAL rows below).

if ($controlFailures.Count -gt 0) {
    Write-Host 'registry_gate: CONTROL FAILURE (a detector cannot fire -- the gate is blind):' -ForegroundColor Red
    $controlFailures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host 'registry_gate: FAIL'
    exit 1
}
Write-Host 'registry_gate: fixture controls fired (dead/write-only/alias x3/ratchet)'

# ---- real scan --------------------------------------------------------------
$texts = @()
$files = Get-ChildItem -Recurse -Path $srcDirs -Include *.cpp, *.h, *.inc -File |
    Where-Object { $selfFiles -notcontains $_.Name }
foreach ($f in $files) { $texts += , ([System.IO.File]::ReadAllText($f.FullName)) }
$refs = Get-RowRefs $texts

# Job 1: dead rows.
foreach ($ident in $rowIdents) {
    if (-not $refs.ContainsKey($ident)) { $violations.Add("dead row: rows::$ident has no reference outside the registry TU") }
}
# Font-role family: alive iff the indexed door is used outside the registry.
$fontDoorRefs = 0
foreach ($t in $texts) { $fontDoorRefs += ([regex]::Matches($t, 'FontRoleRow\(')).Count }
if ($fontRoleIdents.Count -gt 0 -and $fontDoorRefs -eq 0) {
    $violations.Add('dead rows: the CFG_FONTROLE family has no FontRoleRow( reference outside the registry TU')
}

# Job 2: write-only rows + symmetric reaping.
foreach ($ident in $rowIdents) {
    if (-not $refs.ContainsKey($ident)) { continue }  # already a dead-row violation
    $r = $refs[$ident]
    $isWriteOnly = ($r.write -gt 0 -and $r.write -eq $r.total)
    if ($isWriteOnly -and -not $writeOnlyAllow.ContainsKey($ident)) {
        $violations.Add("write-only row: rows::$ident is only ever written -- add a reader or an allowlist entry with a review reason")
    }
    if (-not $isWriteOnly -and $writeOnlyAllow.ContainsKey($ident)) {
        $violations.Add("allowlist reap: rows::$ident gained a reader -- remove its write-only allowlist entry")
    }
}
foreach ($ident in @($writeOnlyAllow.Keys)) {
    if (($rowIdents -cnotcontains $ident) -and ($fontRoleIdents -cnotcontains $ident)) {
        $violations.Add("allowlist reap: entry '$ident' matches no registry row")
    }
}

# Job 3: forbidden alias vocabulary.
foreach ($f in $files) {
    $t = [System.IO.File]::ReadAllText($f.FullName)
    foreach ($a in $aliasRx) {
        if ($t -cmatch $a.rx) { $violations.Add("forbidden alias ($($a.name)) for the rows namespace in $($f.Name)") }
    }
}

# Job 4: ratchet surface on the public header.
$hdrText = [System.IO.File]::ReadAllText($cfgHdr)
if ($hdrText -cmatch $ratchetRx) {
    $violations.Add('ratchet surface: coop/config/config.h declares a string-keyed read/write API again')
}

# ---- verdict ----------------------------------------------------------------
if ($violations.Count -gt 0) {
    Write-Host 'registry_gate: FAIL' -ForegroundColor Red
    $violations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host "registry_gate: PASS (rows=$($rowIdents.Count)+$($fontRoleIdents.Count) fontRole; write-only allowlist=$($writeOnlyAllow.Count); alias/ratchet clean)"
exit 0

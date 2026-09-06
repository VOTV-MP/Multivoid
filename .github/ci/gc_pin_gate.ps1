# tools/gc/gc_pin_gate.ps1 -- a GC pin must be OWNED, never a hand-written flag pair.
#
# THE RULE. `reflection::AddToRoot` / `RemoveFromRoot` are the slot primitives. Subsystems
# hold an `ue_wrap::GcPin` (ue_wrap/core/gc_pin.h), which roots on construction and un-roots
# from its destructor. Nothing outside gc_pin.cpp calls the primitives directly.
#
# WHY PROSE WAS NOT ENOUGH -- this is not a hypothetical. reflection.h's own RemoveFromRoot
# comment already said the release "pairs with AddToRoot on EVERY teardown ... a
# destroyed-but-still-rooted object leaks its GUObjectArray slot forever", and
# subsystems.cpp's DisconnectAll comment already claimed "RetireProxy un-roots + destroys ...
# (structural no-leak)". Both were true statements about intent and both shipped beside code
# that did the opposite: the un-root sat inside `if (liveActor)`, and at a world teardown
# every mirror reports not-alive. `[V]` 2026-09-01: 871 spawned trash proxies, 871 root-set
# actors still reaching the departed UWorld through their Outer chain, that world therefore
# never collected, and the next in-process map load adopted the corpse and died on its null
# WorldSettings. A second module (native_pile_mirror) had a pin with no release path at all.
#
# HONEST BILLING. A text scan over `src/`, not a compile-time constraint:
#   * It matches CALLS -- `AddToRoot(` / `RemoveFromRoot(` -- not prose. reflection.h and
#     gc_pin.h are full of the names in comments and neither is a violation.
#   * It does not see a call made through a function pointer or a macro alias.
#   * third_party/ is excluded.
#
# ALLOWLIST -- by FILE, and only two, both of which are the mechanism rather than a user:
#   * src/ue_wrap/core/reflection.cpp  -- defines the primitives.
#   * src/ue_wrap/core/gc_pin.cpp      -- the single owner that calls them.
#
# Modes:
#   -CrossCheck  list every hit with context (used to verify a census)
#   -Drill       inject a violation into a scratch copy and require the gate to FIRE, then
#                run the real check. A gate never shown failing is not evidence of anything.
#   (default)    CI gate: exit 1 if any non-allowlisted call survives.

param(
    [switch]$CrossCheck,
    [switch]$Drill,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$srcRoot = Join-Path $root "src"

$Forbidden = @('AddToRoot', 'RemoveFromRoot')
# Allowlisted by PATH SUFFIX, not by basename: a basename match would silently exempt a
# future `src/anything/gc_pin.cpp`, which is the file a violator would most plausibly be
# called. Matched case-insensitively with either separator.
$AllowedSuffixes = @('ue_wrap\core\reflection.cpp', 'ue_wrap\core\gc_pin.cpp')

function Test-Allowed {
    param([string]$FullName)
    foreach ($suffix in $AllowedSuffixes) {
        $norm = $FullName -replace '/', '\'
        if ($norm.EndsWith($suffix, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Get-Violations {
    param([string]$Root)

    $hits = @()
    $files = Get-ChildItem -Path $Root -Recurse -Include *.cpp, *.h -File |
             Where-Object { $_.FullName -notmatch '[\/]third_party[\/]' }

    foreach ($f in $files) {
        if (Test-Allowed $f.FullName) { continue }
        $lines = Get-Content -LiteralPath $f.FullName
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            foreach ($sym in $Forbidden) {
                if ($line -notmatch [regex]::Escape($sym) + '\s*\(') { continue }
                # A line that is wholly a comment is prose, not a call.
                if ($line -match '^\s*(//|\*|/\*)') { continue }
                # A DECLARATION in the primitives' own header is not a call either. Match the
                # shape `bool AddToRoot(void* obj);` rather than allowlisting the file, so a
                # real call added to reflection.h would still fire.
                if ($line -match '^\s*bool\s+(AddToRoot|RemoveFromRoot)\s*\(void\*\s*\w+\)\s*;') { continue }

                $rel = if ($f.FullName.StartsWith($Root)) {
                    $f.FullName.Substring($Root.Length).TrimStart([char]92, [char]47)
                } else { $f.FullName }
                $hits += [PSCustomObject]@{ File = $rel; Line = $i + 1; Sym = $sym; Text = $line.Trim() }
            }
        }
    }
    return , $hits
}

if ($Drill) {
    # RED control: a subsystem taking the pin by hand, exactly the shape that leaked.
    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("gcpingate_drill_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    try {
        $inject = @'
void SomeFutureMirror(void* actor) {
    R::AddToRoot(actor);
}
void SomeFutureTeardown(void* actor) {
    if (IsLive(actor)) R::RemoveFromRoot(actor);
}
'@
        Set-Content -LiteralPath (Join-Path $scratch "injected.cpp") -Value $inject
        $red = Get-Violations -Root $scratch
        if ($red.Count -lt 2) {
            Write-Host "DRILL FAIL: injected 2 violations, the scanner found $($red.Count) -- the gate is BLIND."
            exit 2
        }
        if (-not $Quiet) {
            Write-Host "DRILL PASS: the scanner caught the injected violations:"
            $red | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
        }

        # INVERSE control: the header's declarations and the owner's own calls must NOT fire,
        # or the gate is red on a legitimate tree and would simply be switched off.
        Remove-Item -LiteralPath (Join-Path $scratch "injected.cpp") -Force
        $ok = @'
// AddToRoot(x) in a comment is prose, not a call.
bool AddToRoot(void* obj);
bool RemoveFromRoot(void* obj);
'@
        Set-Content -LiteralPath (Join-Path $scratch "reflection.h") -Value $ok
        $ownerOk = @'
bool GcPin::Pin(void* obj) { return R::AddToRoot(obj); }
void GcPin::Release() { R::RemoveFromRoot(obj_); }
'@
        $ownerDir = Join-Path $scratch "ue_wrap\core"
        New-Item -ItemType Directory -Path $ownerDir -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $ownerDir "gc_pin.cpp") -Value $ownerOk
        # And the NEGATIVE half of the path rule: the same body under a DIFFERENT directory
        # must still fire, or the allowlist is a basename rule wearing a path's clothes.
        Set-Content -LiteralPath (Join-Path $scratch "gc_pin.cpp") -Value $ownerOk
        $green = Get-Violations -Root $scratch
        $misplaced = @($green | Where-Object { $_.File -notmatch '[\\/]' })
        $legit = @($green | Where-Object { $_.File -match '[\\/]' })
        if ($legit.Count -ne 0) {
            Write-Host "DRILL FAIL: a legitimate shape FIRED ($($legit.Count) hit) -- the gate is unusable."
            $legit | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
            exit 2
        }
        if ($misplaced.Count -lt 2) {
            Write-Host "DRILL FAIL: a gc_pin.cpp OUTSIDE ue_wrap/core did NOT fire ($($misplaced.Count) hit) -- the allowlist is matching on basename."
            exit 2
        }
        if (-not $Quiet) {
            Write-Host "DRILL PASS: declarations and the real owner do not fire; a same-named file elsewhere DOES."
        }
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$violations = Get-Violations -Root $srcRoot

if ($CrossCheck) {
    Write-Host "gc_pin gate cross-check -- every AddToRoot/RemoveFromRoot call under src/:"
    if ($violations.Count -eq 0) { Write-Host "    (none outside $($AllowedFiles -join ', '))" }
    $violations | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
}

if ($violations.Count -gt 0) {
    Write-Host ""
    Write-Host "GC PIN GATE: FAIL -- $($violations.Count) direct call(s) to the root-set primitives:"
    $violations | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
    Write-Host ""
    Write-Host "Hold an ue_wrap::GcPin (ue_wrap/core/gc_pin.h) instead. It releases from its"
    Write-Host "destructor, so no teardown path can forget the un-root or condition it on a"
    Write-Host "liveness test -- which is how a whole UWorld was leaked on 2026-09-01."
    exit 1
}

if (-not $Quiet) {
    Write-Host "GC PIN GATE: PASS -- every GC pin in src/ is owned by an ue_wrap::GcPin."
}
exit 0

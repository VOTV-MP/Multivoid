# tools/hooks/minhook_free_gate.ps1 -- nothing in this mod may FREE a MinHook trampoline.
#
# THE RULE, and why prose was not enough. `ue_wrap/core/hook.h` ("Retirement") states it:
# lift a patch, never remove it. Removing writes a linked-list pointer over the trampoline's
# first eight bytes -- `[V]` minhook/src/buffer.c:43-50 (MEMORY_SLOT UNIONs the link with the
# bytes) + :282, reached from :702 MH_RemoveHook -- while a thread may still be about to
# return through them. MH_Uninitialize then VirtualFrees every block on top of that.
#
# That rule was ALREADY written down, in the same header, and it did not hold: hook.h:45-48
# said Disable was "the ONLY safe retirement for a detour other threads may be entering
# concurrently" while hook.h:56, five lines later, called a remove-and-uninitialize helper
# "Safe to call once at shutdown". Both shipped for four months. A 2026-05-27 audit even
# looked straight at the use-after-free and cleared it, because the variable was named
# `g_originalPE` and the auditor believed the name.
#
# So this is the tripwire for the half a gate CAN hold: a call re-entering the tree. It
# cannot catch a lying comment (the rename to `*Trampoline` is what addresses that half),
# and it should not pretend to.
#
# HONEST BILLING. This is a text scan over `src/`, not a compile-time constraint:
#   * It matches CALLS -- `MH_RemoveHook(` / `MH_Uninitialize(` -- not prose, because a
#     census of this exact rule was run by grepping prose and missed `hook.cpp`'s third
#     MH_RemoveHook entirely (2026-08-26).
#   * It does not see a call made through a function pointer or a macro alias.
#   * third_party/ is excluded: MinHook's own implementation obviously calls them.
#
# ALLOWLIST -- exactly one line, and it is not a convenience. `hook.cpp`'s Install() calls
# MH_RemoveHook when MH_EnableHook has just FAILED. The target was therefore never patched,
# so no thread can be executing in the trampoline or holding a pointer into it, and the
# alternative is leaking the slot. It is identified by FILE + SURROUNDING FUNCTION rather
# than by line number, because this project has re-cited moved line numbers three times.
#
# Modes:
#   -CrossCheck  list every hit with context (used to verify the census)
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

$Forbidden = @('MH_RemoveHook', 'MH_Uninitialize')

function Get-Violations {
    param([string]$Root)

    $hits = @()
    $files = Get-ChildItem -Path $Root -Recurse -Include *.cpp, *.h -File |
             Where-Object { $_.FullName -notmatch '[\\/]third_party[\\/]' }

    foreach ($f in $files) {
        $lines = Get-Content -LiteralPath $f.FullName
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            foreach ($sym in $Forbidden) {
                # A CALL: the symbol immediately followed by '('. Comments mentioning the
                # symbol without calling it are not violations -- this file is full of them.
                if ($line -notmatch [regex]::Escape($sym) + '\s*\(') { continue }
                # Skip a line that is wholly a comment.
                if ($line -match '^\s*(//|\*|/\*)') { continue }

                # The allowlist, resolved by CONTEXT not by line number: the enable-failure
                # path inside hook.cpp's Install(). Walk back to the nearest function header.
                $allowed = $false
                if ($f.Name -eq 'hook.cpp') {
                    for ($j = $i; $j -ge 0 -and $j -gt $i - 60; $j--) {
                        if ($lines[$j] -match '^\s*bool\s+Install\s*\(') {
                            # Confirm it really is the failure branch, not a stray call.
                            for ($k = $i; $k -ge $j; $k--) {
                                if ($lines[$k] -match 'MH_EnableHook\s*\(') { $allowed = $true; break }
                            }
                            break
                        }
                        if ($lines[$j] -match '^\s*(bool|void|int)\s+\w+\s*\(') { break }
                    }
                }
                if ($allowed) { continue }

                # Relative to the root being SCANNED, not to the repo -- the drill scans a
                # scratch dir, and subtracting the repo path there produced a mangled prefix.
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
    # RED control: copy the tree's hook.cpp to a scratch dir, inject a bare call, and require
    # the scanner to see it. If this passes clean the gate is blind and every GREEN it has
    # ever printed is worthless.
    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("mhgate_drill_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    try {
        $inject = @'
void SomeFutureTeardown(void* target) {
    MH_RemoveHook(target);
    MH_Uninitialize();
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

        # And the inverse control: the allowlisted shape must NOT fire, or the allowlist is
        # decorative and the gate would be red forever on a legitimate tree.
        $ok = @'
bool Install(void* target, void* detour, void** trampoline, bool followJmpImmune) {
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    return true;
}
'@
        Set-Content -LiteralPath (Join-Path $scratch "hook.cpp") -Value $ok
        Remove-Item -LiteralPath (Join-Path $scratch "injected.cpp") -Force
        $green = Get-Violations -Root $scratch
        if ($green.Count -ne 0) {
            Write-Host "DRILL FAIL: the allowlisted enable-failure shape FIRED ($($green.Count) hit) -- the allowlist does not work."
            $green | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
            exit 2
        }
        if (-not $Quiet) { Write-Host "DRILL PASS: the allowlisted enable-failure path does NOT fire." }
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$violations = Get-Violations -Root $srcRoot

if ($CrossCheck) {
    Write-Host "MinHook free-call census over src/ (excluding third_party):"
    if ($violations.Count -eq 0) { Write-Host "    (none outside the allowlist)" }
    $violations | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
    exit 0
}

if ($violations.Count -gt 0) {
    Write-Host "FAIL: $($violations.Count) MinHook free-call(s) outside the allowlist."
    $violations | ForEach-Object { Write-Host ("    {0}:{1}  {2}" -f $_.File, $_.Line, $_.Text) }
    Write-Host ""
    Write-Host "  Removing a hook or uninitializing MinHook FREES the trampoline, and the free"
    Write-Host "  writes a linked-list pointer over its first eight bytes -- over the stolen"
    Write-Host "  prologue a thread may be about to return through. Use hook::Disable: the patch"
    Write-Host "  is lifted, the slot stays intact. See ue_wrap/core/hook.h, 'Retirement'."
    exit 1
}

if (-not $Quiet) {
    Write-Host "PASS: no MinHook free-calls in src/ outside the one allowlisted enable-failure path."
}
exit 0

# tools/reflection/islive_gate.ps1 -- the IsLive cached-pointer discipline gate (D3).
#
# Design of record: research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md
# section 5. The ratified rule (OPUS_48_DISCIPLINE.md:59): a UObject pointer cached ACROSS
# game-thread tasks must never be probed with bare IsLive (it dereferences the possibly-freed
# object and, under a co-resident VEH crash reporter, the absorbed AV surfaces as a user-visible
# "crash"); it must go through CachedObjRef / IsLiveByIndex (array-slot reads only).
#
# HONEST BILLING (design round 7): this gate mechanically re-derives the STATIC subset only --
# a bare `IsLive(<ident>)` whose <ident> is a file-scope/static pointer variable in the same
# file. Member-field caches (element structs, ActiveDrive rows) are invisible to it; so is a
# probe through a local alias (`void* mp = g_localPawn; IsLive(mp)` -- input_owner.cpp:155);
# and a FRESH same-task refill line on a static (`g_x = Find...; IsLive(g_x)`) flags even
# though bare is legal there -- conversion to CachedObjRef removes those lines anyway. Those
# classes are covered by the census (design Appendix A), review, and the runtime attribution
# tripwire. The TYPE is the invariant; this script is a tripwire, not a proof.
#
# Modes:
#   -CrossCheck   list every hit (file:line, ident) -- used to verify the census pre-conversion
#   (default)     CI gate: exit 1 if any hit outside the allowlist survives
#
# Allowlist: harness/autotest (dev-only scenario code, census-excluded by the same rule) and
# reflection.cpp itself (it DEFINES the primitives).

param(
    [switch]$CrossCheck
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$srcRoots = @(
    (Join-Path $root "src\votv-coop\src"),
    (Join-Path $root "src\votv-coop\include")
)

# A "static cached pointer" declaration: file-scope or function-static, pointer type, with the
# codebase's cache naming (g_* / s<Upper>*). Examples matched:
#   void* g_netLocal = nullptr;          static void* g_gsCdo = nullptr;
#   static UClass* sActorCls;            uint8_t* g_camMgr{};
# (?-i: ...) forces case sensitivity -- PowerShell regexes default to case-insensitive,
# which made s[A-Z] match plain locals like `spawned`/`slot`/`save` (caught 2026-08-22).
$declRe = '^\s*(?:static\s+)?(?:[\w:<>,\s]+?)\*+\s*((?-i:g_\w+|s[A-Z]\w+))\s*(?:=|;|\{)'

# A bare-IsLive probe of an identifier (optionally through R:: / reflection:: and optionally
# a leading ! or cast). IsLiveByIndex is the CORRECT form and must not match.
$callRe = '(?<!ByIndex\s{0,8})\bIsLive\s*\(\s*(?:\([\w:\s*]+\)\s*)?([A-Za-z_]\w*)\s*[),]'

$hits = @()
foreach ($sr in $srcRoots) {
    $files = Get-ChildItem -Path $sr -Recurse -Include *.cpp, *.h -File
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($root.Path.Length + 1)
        if ($rel -match 'harness\\autotest\\') { continue }
        if ($rel -match 'ue_wrap\\core\\reflection\.(cpp|h)$') { continue }
        $lines = Get-Content -LiteralPath $f.FullName
        # Pass 1: collect this file's static cached-pointer names.
        $statics = @{}
        foreach ($ln in $lines) {
            if ($ln -match $declRe) { $statics[$Matches[1]] = $true }
        }
        if ($statics.Count -eq 0) { continue }
        # Pass 2: bare IsLive calls on those names. Strip // comments first --
        # prose like "a bare R::IsLive(g_lastHeldProp) would deref freed memory"
        # (local_streams.cpp:54) is documentation, not a probe (caught 2026-08-22).
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $ln = $lines[$i] -replace '//.*$', ''
            if ($ln -notmatch '\bIsLive\s*\(') { continue }
            if ($ln -match '\bIsLiveByIndex\s*\(') { continue }
            foreach ($m in [regex]::Matches($ln, '\bIsLive\s*\(\s*(?:\([\w:\s*]+\)\s*)?([A-Za-z_]\w*)')) {
                $ident = $m.Groups[1].Value
                if ($statics.ContainsKey($ident)) {
                    $hits += [pscustomobject]@{ File = $rel; Line = ($i + 1); Ident = $ident }
                }
            }
        }
    }
}

if ($CrossCheck) {
    Write-Host "islive_gate CROSS-CHECK: $($hits.Count) bare-IsLive-on-static hit(s)"
    $hits | ForEach-Object { Write-Host ("  {0}:{1}  {2}" -f $_.File, $_.Line, $_.Ident) }
    exit 0
}

if ($hits.Count -gt 0) {
    Write-Host "islive_gate FAIL: $($hits.Count) bare IsLive probe(s) of a static cached pointer."
    Write-Host "The rule (OPUS_48_DISCIPLINE.md:59): cached-across-ticks pointers use CachedObjRef"
    Write-Host "(ue_wrap/core/cached_obj_ref.h) or IsLiveByIndex with the captured index."
    $hits | ForEach-Object { Write-Host ("  {0}:{1}  {2}" -f $_.File, $_.Line, $_.Ident) }
    exit 1
}
Write-Host "islive_gate PASS: no bare IsLive on a static cached pointer."
exit 0

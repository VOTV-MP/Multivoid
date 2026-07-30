# atlas_regime_gate.ps1 -- police the ATLAS REGIME, positively.
#
# WHY. On 2026-07-30 both RHI backends stopped clearing
# ImGuiBackendFlags_RendererHasTextures, which turns ImGui's lazy atlas on. That
# single deletion changes what the build can DRAW, what a nickname FOLDS to, and
# how much work a remote peer's chat can force -- and nothing about it is visible
# in a compile. The obvious guard is `grep -c '&= ~...' -eq 0`, and it is worthless:
# it goes GREEN the moment a backend file is renamed, the clear moves into a
# helper, or a third RHI arrives with no gate entry at all.
#
# So this CENSUSES POSITIVELY. It names the files it expects to find, counts what
# it finds in each, classifies by OPERATION KIND, and FAILS ON AN EMPTY OR
# UNEXPECTED CENSUS -- the shape tools/net/peerconn_gate.ps1,
# tools/config/registry_gate.ps1 and tools/text/nick_gate.ps1 already use.
#
# It is NOT tripwires.ps1's business: that script's own header says "ADVISORY,
# always exit 0". This is a refuse-to-build gate, in the family build-core.yml
# already runs from a clean clone.
#
# -Drill injects each violation into a temp copy of the tree's text and requires
# the corresponding check to go RED. A gate never shown failing is decoration
# ([[lesson-an-instrument-never-shown-failing-passes-by-construction]]).
[CmdletBinding()]
param([switch]$Quiet, [switch]$Drill)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# Every file this gate reasons about, and what it must be true of. A missing file
# is a FAILURE, never a skip: "the check did not run" and "the check passed" must
# never produce the same exit code.
$expect = @(
    @{ rel = 'src\votv-coop\src\ui\overlay_backend_dx11.cpp'; role = 'RHI backend' }
    @{ rel = 'src\votv-coop\src\ui\overlay_backend_dx12.cpp'; role = 'RHI backend' }
    @{ rel = 'src\votv-coop\src\ui\atlas_watch.cpp';          role = 'the watch' }
    @{ rel = 'src\votv-coop\src\ui\fonts.cpp';                role = 'the loader' }
    @{ rel = 'src\votv-coop\include\coop\text\exclude_ranges.inc'; role = 'the exclude table' }
)
$designRel = 'research\findings\tooling\votv-imgui-192-upgrade-DESIGN-2026-07-30.md'

# Code, not prose. A comment that QUOTES the line it explains must not be read as
# that line -- see the operation-kind census below for the measurement that made
# this necessary.
function Strip-Comments([string]$t) {
    if (-not $t) { return $t }
    $t = [regex]::Replace($t, '/\*.*?\*/', '', 'Singleline')
    return [regex]::Replace($t, '(?m)//.*$', '')
}

function Read-Or-Fail([string]$rel) {
    $p = Join-Path $repo $rel
    if (-not (Test-Path $p)) { return $null }
    return (Get-Content -Raw -LiteralPath $p)
}

# $mutate: a hashtable rel -> scriptblock that corrupts that file's text, used by
# -Drill only. Production runs pass $null and read the tree as it is.
function Invoke-Gate([hashtable]$mutate) {
    $fails = @()
    $text = @{}
    foreach ($e in $expect) {
        $t = Read-Or-Fail $e.rel
        if ($null -eq $t) {
            $fails += "MISSING $($e.rel) ($($e.role)) -- the layout moved; a zero census is not evidence"
            continue
        }
        if ($mutate -and $mutate.ContainsKey($e.rel)) { $t = & $mutate[$e.rel] $t }
        $text[$e.rel] = $t
    }
    if ($fails.Count -gt 0) { return $fails }

    # --- 1. the regime, by operation kind ------------------------------------
    # A backend may READ the flag; it may not CLEAR it. Two refinements, both
    # found by this gate's own drill rather than by review:
    #
    #   COMMENTS ARE STRIPPED FIRST. Classifying on the assignment form is not
    #   enough when the comment explaining the DELETION quotes the deleted line
    #   verbatim -- which is exactly what overlay_backend_dx11.cpp does, and it
    #   made the first run of this gate report clears=1 on a tree with none. A
    #   gate that reads prose as code fails on a well-documented deletion and
    #   passes on a clear hidden inside a #if 0.
    $backends = @($expect | Where-Object { $_.role -eq 'RHI backend' })
    $clears = 0
    foreach ($b in $backends) {
        $t = Strip-Comments $text[$b.rel]
        $c = ([regex]::Matches($t, '&=\s*~\s*ImGuiBackendFlags_RendererHasTextures')).Count
        $clears += $c
        if (-not $Quiet) {
            Write-Host ("  {0,-46} clears={1}" -f (Split-Path -Leaf $b.rel), $c)
        }
    }
    if ($clears -gt 0) {
        # The pre-flip state is not automatically wrong -- it was deliberate for
        # one build -- but it may not ship unexamined. Either our own DX12 texture
        # servicing exists, or the design doc carries a dated verdict line saying
        # the upload cost was measured and accepted.
        #   THE ESCAPE HATCH IS ANCHORED AND DATED. An unanchored match on the
        #   token is satisfied by the design doc's own PROSE describing this gate
        #   -- measured: the first version of this check passed on a tree with a
        #   live clear, because the doc mentions the token in a sentence. The
        #   line must BEGIN with it and carry an ISO date, so writing the escape
        #   is a deliberate act with a day attached.
        $svc = Test-Path (Join-Path $repo 'src\votv-coop\src\ui\overlay_backend_dx12_textures.cpp')
        $design = Read-Or-Fail $designRel
        $verdict = $design -and ($design -match '(?m)^MEASURED-UPLOAD-VERDICT:\s*\d{4}-\d{2}-\d{2}')
        if (-not ($svc -or $verdict)) {
            $fails += ("$clears backend site(s) CLEAR RendererHasTextures, and neither our own " +
                       "DX12 servicing TU nor a dated 'MEASURED-UPLOAD-VERDICT:' line in " +
                       "$designRel exists. One binary must not ship two drawable repertoires " +
                       "chosen by the player's GPU API.")
        }
    }

    # --- 2. the flip is WATCHED ----------------------------------------------
    # The regime assertion is what stops every check in atlas_watch.cpp from
    # passing by construction under an eager atlas.
    $w = $text['src\votv-coop\src\ui\atlas_watch.cpp']
    if ($w -notmatch 'BackendFlags\s*&\s*ImGuiBackendFlags_RendererHasTextures') {
        $fails += "atlas_watch.cpp no longer asserts the regime -- its checks would pass by construction"
    }
    foreach ($needle in 'InRepertoire', 'InExcludeSet', 'IsGlyphInFont') {
        if ($w -notmatch [regex]::Escape($needle)) {
            $fails += "atlas_watch.cpp no longer references $needle -- an instrument was removed"
        }
    }

    # --- 3. the atlas ceiling is EXPLICIT ------------------------------------
    # ImGui defaults TexMax to 8192. Inheriting it raises the worst DX12 upload
    # behind an unbounded fence wait from 16.8 MB to 268 MB.
    $f = $text['src\votv-coop\src\ui\fonts.cpp']
    if ($f -notmatch 'TexMaxWidth\s*=\s*2048' -or $f -notmatch 'TexMaxHeight\s*=\s*2048') {
        $fails += "fonts.cpp does not pin TexMaxWidth/TexMaxHeight to 2048 (the 8192 default would be inherited)"
    }
    if ($f -notmatch 'GlyphExcludeRanges') {
        $fails += "fonts.cpp no longer sets GlyphExcludeRanges -- every codepoint in every face would bake"
    }

    # --- 4. the exclude table cannot be a no-op ------------------------------
    # ImGui walks GlyphExcludeRanges as a ZERO-TERMINATED array. A table whose
    # first value is 0 excludes NOTHING, both of ImGui's own asserts pass, and
    # NDEBUG strips them -- so nothing downstream can see it.
    $inc = $text['src\votv-coop\include\coop\text\exclude_ranges.inc']
    $rows = [regex]::Matches($inc, '\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*\}')
    if ($rows.Count -eq 0) {
        $fails += "exclude_ranges.inc parsed 0 ranges -- the emission format moved and this check rotted"
    } elseif ([Convert]::ToInt64($rows[0].Groups[1].Value, 16) -eq 0) {
        $fails += ("exclude_ranges.inc begins at U+0000. ImGui reads the list as zero-terminated, " +
                   "so the ENTIRE exclude set becomes a no-op and every codepoint any face " +
                   "carries bakes -- silently, with fold != bake.")
    }
    if (-not $Quiet) { Write-Host ("  {0,-46} ranges={1}" -f 'exclude_ranges.inc', $rows.Count) }

    return $fails
}

if ($Drill) {
    # Each drill re-introduces a REAL defect -- three of these are states the tree
    # was actually in during this work -- and the gate must go RED for it.
    $drills = @(
        @{ name = 'flag cleared again'
           mut  = @{ 'src\votv-coop\src\ui\overlay_backend_dx11.cpp' = {
                        param($t) $t + "`nvoid x(){ ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures; }`n" } }
           want = 'CLEAR RendererHasTextures' }
        @{ name = 'regime assert gone'
           mut  = @{ 'src\votv-coop\src\ui\atlas_watch.cpp' = {
                        param($t) $t -replace 'BackendFlags & ImGuiBackendFlags_RendererHasTextures', 'false' } }
           want = 'no longer asserts the regime' }
        @{ name = 'TexMax inherited'
           mut  = @{ 'src\votv-coop\src\ui\fonts.cpp' = {
                        param($t) $t -replace 'TexMaxWidth\s*=\s*2048', 'TexMaxWidth  = 8192' } }
           want = 'does not pin TexMaxWidth' }
        @{ name = 'exclude list a no-op'
           mut  = @{ 'src\votv-coop\include\coop\text\exclude_ranges.inc' = {
                        param($t) $t -replace '\{ 0x00001,', '{ 0x00000,' } }
           want = 'begins at U\+0000' }
        @{ name = 'exclude format rotted'
           mut  = @{ 'src\votv-coop\include\coop\text\exclude_ranges.inc' = {
                        param($t) '// nothing parseable here' } }
           want = 'parsed 0 ranges' }
    )
    $bad = 0
    foreach ($d in $drills) {
        $r = Invoke-Gate $d.mut
        $red = @($r | Where-Object { $_ -match $d.want }).Count -gt 0
        Write-Host ("  drill {0,-24} {1}" -f $d.name, ($(if ($red) { 'RED (gate fired)' } else { 'GREEN -- GATE DID NOT FIRE' })))
        if (-not $red) { $bad++; $r | ForEach-Object { Write-Host "      got: $_" } }
    }
    $base = Invoke-Gate $null
    Write-Host ("  drill {0,-24} {1}" -f 'baseline', ($(if ($base.Count -eq 0) { 'GREEN' } else { 'RED -- THE REAL TREE FAILS' })))
    if ($base.Count -ne 0) { $bad++; $base | ForEach-Object { Write-Host "      $_" } }
    Write-Host ("atlas_regime_gate drill: {0} ({1} problem(s))" -f $(if ($bad -eq 0) { 'PASS' } else { 'FAIL' }), $bad)
    exit $(if ($bad -eq 0) { 0 } else { 1 })
}

$fails = Invoke-Gate $null
if ($fails.Count -gt 0) {
    Write-Host 'atlas_regime_gate: FAIL'
    $fails | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host 'atlas_regime_gate: PASS -- lazy atlas on both RHIs, watched, ceiling pinned, exclude table live'
exit 0

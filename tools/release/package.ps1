# package.ps1 -- assemble the ONE distributable zip (docs/UE4SS_ARC.md 7.2a shape).
#
# WHY ONE ZIP. 7.4c (USER 2026-08-25): "Я хочу чтобы в zip лежал сам мод с нужной
# иерархией под r2modman ... Кто захочет установить не для r2modman, а вручную для
# ue4ss без shimloader то сам разберется и закинет куда надо файлы из нашего
# собранного zip release." Both lanes take the SAME file: r2modman routes it by its
# top-level folder names, and a hand-installer copies mod\ into the game's Mods\.
#
# THE MANIFEST IS GENERATED, NEVER HAND-EDITED (7.3, HARD REQUIREMENT). A typed
# version string that rots unbumped is the exact failure that got mod semver deleted
# on 2026-07-19; a hand-kept version_number would recreate it one layer out. Both
# halves are read from the tree through the ONE parser each already has in
# ledger_lib.ps1, and a parse miss FAILS CLOSED rather than defaulting.
#
# THE ZIP IS VERIFIED AFTER IT IS WRITTEN, not merely assembled (7.4b: "it must FAIL
# CLOSED if the zip does not contain the expected tree -- an empty or mis-rooted zip
# is a silently broken release"). Test-PackageZip lives in ledger_lib.ps1 so publish
# can re-run the identical predicate on the artifact it downloaded back.
#
# THE PAK IS OWED, NOT DROPPED -- and -Pak is why this script works in both worlds.
# 7.7c item 1: "scientists.pak is the BASE pak and ships INSIDE the mod package", and
# 7.9's USER DIRECTION 2026-08-25 ("zip релиза будет содержать мод и пак") chose manual
# assembly BECAUSE the pak ships. What the user said 2026-08-26 -- "скин пака пока не
# делали, это долг" -- is that the ARTIFACT does not exist yet (THUNDERSTORE.md:33: the
# skin_registry LogicMods walk is a hard blocker), not that it is excluded. So today the
# zip is pak-less and CI can assemble it unaided; the day the pak exists the SAME script
# takes -Pak and the assembly may move to a human without the tree forking.
# A pak-less package is a fully working mod: client_model.cpp:52-53 falls back to the
# game's own kerfur skin, redistributing nothing.

param(
    [string]$BuildDir   = 'build/votv-coop/Release',
    [string]$PayloadDll = '',                       # default: the one payload in BuildDir
    [string]$OutDir     = 'build/package',
    [string[]]$Pak      = @(),                      # optional: files routed to pak\ (7.7c)
    [switch]$KeepStage                              # leave the staged tree for inspection
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ledger_lib.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# --- Identity: both halves from the tree, through the one parser each -------
$gameTarget = Get-GameTargetFromCMake -CMakePath (Join-Path $repoRoot $script:CMakeListsPath)
$proto      = Get-ProtoFromWorktree  -RepoRoot $repoRoot
$version    = ConvertTo-PackageVersion -GameTarget $gameTarget -Proto $proto
Write-Host "identity: game=$gameTarget build=$proto -> version_number=$version"

# --- Payload ---------------------------------------------------------------
# THE ONE SEAM THAT MUST NOT GO MANUAL (7.9): which DLL bytes go in the zip. For a
# release that is the tagged cacheless rebuild's artifact, handed in via -PayloadDll.
# The BuildDir default exists for the local-import control, which tests LAYOUT and is
# explicitly exempt from the CI-bytes rule (THUNDERSTORE.md checklist item 5).
if (-not $PayloadDll) {
    $bd = Join-Path $repoRoot $BuildDir
    $cand = @(Get-ChildItem (Join-Path $bd 'main.dll') -ErrorAction SilentlyContinue)
    if ($cand.Count -ne 1) {
        throw ("expected exactly one main.dll in $bd, found $($cand.Count) -- build first: " +
               'cmake --build build/votv-coop --config Release')
    }
    $PayloadDll = $cand[0].FullName
}
if (-not (Test-Path -LiteralPath $PayloadDll)) { throw "payload not found: $PayloadDll" }

# --- Icon: re-MEASURE it, never trust the filename (THUNDERSTORE.md item 3) --
$iconPath = Join-Path $repoRoot 'assets/branding/icon.png'
if (-not (Test-Path -LiteralPath $iconPath)) { throw "icon missing: $iconPath" }
$iconDim = Get-PngDimensions -Path $iconPath
if ($iconDim.Width -ne 256 -or $iconDim.Height -ne 256) {
    throw ("icon.png must be exactly 256x256 (Thunderstore requirement); measured " +
           "$($iconDim.Width)x$($iconDim.Height). Re-run the downscale in assets/branding/README.md.")
}

$readmePath = Join-Path $repoRoot 'README.md'
if (-not (Test-Path -LiteralPath $readmePath)) { throw "README.md missing at the repo root" }

# --- Stage the 7.2a tree ---------------------------------------------------
# The zip ROOT holds manifest/icon/README with NO wrapping folder, and the mod's
# files sit under mod\ -- a root-level dlls\ matches no route and silently never
# loads (7.2a trap 1). enabled.txt is what UE4SS reads to START an enumerated mod.
$outAbs = Join-Path $repoRoot $OutDir
$stage  = Join-Path $outAbs 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'mod/dlls') -Force | Out-Null

Set-Content -LiteralPath (Join-Path $stage 'manifest.json') `
            -Value (New-PackageManifest -Version $version -GameTarget $gameTarget -Build $proto) `
            -Encoding utf8 -NoNewline
Copy-Item $iconPath   (Join-Path $stage 'icon.png')
Copy-Item $readmePath (Join-Path $stage 'README.md')
Copy-Item $PayloadDll (Join-Path $stage 'mod/dlls/main.dll')
# enabled.txt: UE4SS reads the FILE'S PRESENCE, and the field packages ship it with
# the literal "true" inside. Match them rather than shipping an empty file.
Set-Content -LiteralPath (Join-Path $stage 'mod/enabled.txt') -Value 'true' -Encoding ascii -NoNewline

# --- Optional pak route (7.2a: pak\ -> shimloader\pak\<pkg>\) -----------------
if ($Pak.Count -gt 0) {
    New-Item -ItemType Directory -Path (Join-Path $stage 'pak') -Force | Out-Null
    foreach ($f in $Pak) {
        if (-not (Test-Path -LiteralPath $f)) { throw "pak input not found: $f" }
        Copy-Item $f (Join-Path $stage 'pak')
    }
    Write-Host "pak: staged $($Pak.Count) file(s)"
} else {
    Write-Host 'pak: none (the skin pak is OWED, not dropped -- UE4SS_ARC 7.7c / THUNDERSTORE.md:33)'
}

# --- Zip -------------------------------------------------------------------
# Named <Team>-<Name>-<version>.zip after the convention every field package follows
# (7.2a). Thunderstore itself keys on the Team + manifest, not the filename
# (THUNDERSTORE.md section 3), but the GitHub release asset is read by humans and the
# Paper pair has to stay legible there -- it is the identity's zip-name destination
# (7.3a item 2).
$zipName = "Multivoid-Multivoid-$version.zip"
$zipPath = Join-Path $outAbs $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal

# --- Verify the WRITTEN zip, not the staged tree ---------------------------
# The payload sha is handed in so the check is EXACT: a presence-by-name gate cannot
# tell a real DLL from a truncated one, and 7.9 records this project shipping wrong
# bytes once already (post-ship audit 2026-08-26, CRITICAL).
$payloadSha = (Get-FileHash -Algorithm SHA256 $PayloadDll).Hash.ToLowerInvariant()
$violations = @(Test-PackageZip -ZipPath $zipPath -ExpectedPayloadSha256 $payloadSha)
if ($violations.Count -gt 0) {
    throw ("package zip FAILED its own tree check:`n  " + ($violations -join "`n  "))
}

if (-not $KeepStage) { Remove-Item $stage -Recurse -Force }

$sha = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLowerInvariant()
Write-Host ""
Write-Host "PACKAGE OK  $zipName" -ForegroundColor Green
Write-Host "  path   : $zipPath"
Write-Host "  sha256 : $sha"
Write-Host "  payload: $PayloadDll"
Write-Host "  tree   : manifest.json, icon.png, README.md, mod/enabled.txt, mod/dlls/main.dll"
if ($Pak.Count -gt 0) { Write-Host "           + pak/ ($($Pak.Count) file(s))" }

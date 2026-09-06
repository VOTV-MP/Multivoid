# package_drill.ps1 -- RED/GREEN controls for Test-PackageZip.
#
# A gate that has never been shown FAILING is not evidence of anything. This builds
# deliberately-broken zips for each trap docs/UE4SS_ARC.md 7.2a measured off
# r2modman's own rule engine, and REQUIRES the check to catch every one -- plus a
# GREEN arm requiring it to stay silent on a correct package.
#
# The two traps are not hypothetical: 7.2a read them out of
# InstallRulePluginInstaller.ts and r2modman's own test spec. A top-level directory
# matching no route is RECURSED INTO and its files classified individually, so a
# wrapped zip scatters; and a root-level dlls/ matches no route at all, which is the
# silent "mod does nothing" a player would report and nobody could reproduce.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here 'ledger_lib.ps1')

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('mv-pkg-drill-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

$fails = 0
function Arm {
    param([string]$Name, [scriptblock]$Build, [bool]$ExpectViolation, [string]$Because, [string]$Sha = '')
    $stage = Join-Path $tmp $Name
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    & $Build $stage
    $zip = Join-Path $tmp "$Name.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    $v = @(Test-PackageZip -ZipPath $zip -ExpectedPayloadSha256 $Sha)
    $caught = $v.Count -gt 0
    if ($caught -eq $ExpectViolation) {
        $verdict = if ($ExpectViolation) { "RED  caught" } else { "GREEN clean" }
        Write-Host ("  PASS  {0,-22} {1}" -f $Name, $verdict) -ForegroundColor Green
        if ($ExpectViolation) { $v | ForEach-Object { Write-Host "          -> $_" -ForegroundColor DarkGray } }
    } else {
        $script:fails++
        Write-Host ("  FAIL  {0,-22} expected violation={1}, got {2} -- {3}" -f $Name, $ExpectViolation, $v.Count, $Because) -ForegroundColor Red
        $v | ForEach-Object { Write-Host "          -> $_" -ForegroundColor DarkGray }
    }
}

function Seed {
    param([string]$Root, [string]$ManifestVersion = '0.9.143', [string]$ManifestName = 'Multivoid')
    New-Item -ItemType Directory -Path (Join-Path $Root 'mod/dlls') -Force | Out-Null
    Set-Content (Join-Path $Root 'mod/enabled.txt') 'true' -NoNewline
    Set-Content (Join-Path $Root 'mod/dlls/main.dll') 'MZfake' -NoNewline
    Set-Content (Join-Path $Root 'README.md') '# Multivoid' -NoNewline
    # a real 256x256 PNG is not needed here -- Test-PackageZip checks the TREE and the
    # manifest; the icon dimension check is package.ps1's, measured at stage time.
    Set-Content (Join-Path $Root 'icon.png') 'x' -NoNewline
    $m = [ordered]@{
        name = $ManifestName; version_number = $ManifestVersion
        website_url = 'https://multivoid.dev'; description = 'd'
        dependencies = @('Thunderstore-unreal_shimloader-1.1.7')
    }
    Set-Content (Join-Path $Root 'manifest.json') ($m | ConvertTo-Json -Depth 4) -NoNewline
}

Write-Host "package_drill: Test-PackageZip controls" -ForegroundColor Cyan

Arm 'green-correct' { param($r) Seed $r } $false 'a correct package must produce ZERO violations'

Arm 'red-wrapped' { param($r)
    $inner = Join-Path $r 'Multivoid'
    New-Item -ItemType Directory -Path $inner -Force | Out-Null
    Seed $inner
} $true 'a wrapping folder makes every route unreachable -- 7.2a: an unmatched top-level dir is RECURSED INTO'

# NARROWED after the 2026-08-26 audit: this used to delete the whole mod/ tree, which
# conflated trap 1 with "enabled.txt is missing" and gave the arm blast radius its own
# name did not describe. Only the payload moves now.
Arm 'red-root-dlls' { param($r)
    Seed $r
    Remove-Item (Join-Path $r 'mod/dlls') -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $r 'dlls') -Force | Out-Null
    Set-Content (Join-Path $r 'dlls/main.dll') 'MZfake' -NoNewline
} $true '7.2a trap 1: a root-level dlls/ matches no route and silently never loads'

Arm 'red-no-manifest' { param($r) Seed $r; Remove-Item (Join-Path $r 'manifest.json') } $true 'no manifest = not a package'

Arm 'red-no-icon' { param($r) Seed $r; Remove-Item (Join-Path $r 'icon.png') } $true 'Thunderstore requires icon.png at the root'

Arm 'red-bad-version' { param($r) Seed $r -ManifestVersion '0.9.143-dev' } $true 'version_number must be a suffix-free semver triple'

Arm 'red-bad-name' { param($r) Seed $r -ManifestName 'Multi void' } $true 'name allows only [A-Za-z0-9_]'

Arm 'red-payload-at-mod-root' { param($r)
    Seed $r
    Remove-Item (Join-Path $r 'mod/dlls') -Recurse -Force
    Set-Content (Join-Path $r 'mod/main.dll') 'MZfake' -NoNewline
} $true 'UE4SS loads dlls/main.dll -- a payload at mod/ root is never enumerated'

# ISOLATES THE ROUTING CHECK. Every other RED arm is over-determined: the audit traced
# that red-wrapped and red-root-dlls each trip the required-entry check AND the routing
# check, so deleting the routing loop entirely would leave both still reporting "caught"
# and the regression would be invisible. This is the one shape where the routing check
# fires ALONE -- a complete, correct tree plus one stray file under an unknown folder.
Arm 'red-stray-toplevel' { param($r)
    Seed $r
    New-Item -ItemType Directory -Path (Join-Path $r 'junk') -Force | Out-Null
    Set-Content (Join-Path $r 'junk/extra.txt') 'x' -NoNewline
} $true 'a top-level folder matching no route is recursed into and its files scattered'

# THE PAYLOAD'S BYTES. Presence-by-name passed all of these before 2026-08-26.
Arm 'red-empty-dll' { param($r)
    Seed $r
    Set-Content (Join-Path $r 'mod/dlls/main.dll') '' -NoNewline
} $true 'a zero-byte main.dll installs a mod that cannot load'

Arm 'red-not-a-dll' { param($r)
    Seed $r
    Set-Content (Join-Path $r 'mod/dlls/main.dll') 'not a PE at all' -NoNewline
} $true 'content without the MZ magic is not a DLL, whatever the filename says'

Arm 'red-wrong-payload-sha' { param($r) Seed $r } $true 'an exact sha mismatch means the zip does not carry the payload it was handed' -Sha 'deadbeef'

# ISOLATES THE StrictMode-SAFE PROPERTY ACCESS. Before the fix this arm did not report
# FAIL -- it threw PropertyNotFoundException out of Test-PackageZip and took every
# arm after it down with the script.
Arm 'red-manifest-missing-key' { param($r)
    Seed $r
    $m = [ordered]@{ name = 'Multivoid'; version_number = '0.9.143'; website_url = ''; dependencies = @() }
    Set-Content (Join-Path $r 'manifest.json') ($m | ConvertTo-Json -Depth 4) -NoNewline
} $true 'a manifest merely MISSING description must FAIL the gate, never crash it'

Arm 'red-miscased-entry' { param($r)
    Seed $r
    Remove-Item (Join-Path $r 'mod/dlls/main.dll') -Force
    Set-Content (Join-Path $r 'mod/dlls/Main.DLL') 'MZfake' -NoNewline
} $true 'THUNDERSTORE.md:39 -- root files are case-SENSITIVE; PS -notcontains is not'

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
if ($fails -gt 0) { Write-Host "package_drill: $fails ARM(S) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "package_drill: ALL PASS" -ForegroundColor Green

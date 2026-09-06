# fingerprint.ps1 -- the "proven runnable" toolchain gate (design D3, R23).
# The committed fingerprint (tools/release/fingerprint.json, read from MAIN
# HEAD) records the runner MSVC toolset + Windows SDK + the sha256 of
# build-core.yml whose CI bytes were last proven runnable by the cacheless
# smoke. A release run refuses on any mismatch ("re-smoke + re-commit").
# A MISSING fingerprint file is a refusal too (fail-closed -- this is what
# enforces the section-5 build ordering; no seed is ever committed, R19).
#
#   fingerprint.ps1 dump   -> print the live facts as JSON (the human commits
#                             this from the CACHELESS smoked run's artifact)
#   fingerprint.ps1 check  -> compare live facts vs the committed file; exit 1
#                             on mismatch/missing
#
# image_version is recorded as EVIDENCE only, never compared: an image roll
# that keeps the same toolset+SDK is not a build-path change.

param(
    [Parameter(Mandatory)][ValidateSet('dump', 'check')][string]$Mode,
    [string]$FingerprintPath = (Join-Path $PSScriptRoot 'fingerprint.json'),
    [string]$BuildCorePath = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '.github/workflows/build-core.yml')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ToolchainFacts {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found -- not a VS machine?' }
    $inst = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $inst) { $inst = & $vswhere -latest -property installationPath }
    $inst = "$inst".Trim()
    if (-not $inst) { throw 'no Visual Studio instance found' }
    $toolset = (Get-Content (Join-Path $inst 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
    $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
    $sdk = @(Get-ChildItem $sdkRoot -Directory | Where-Object Name -match '^\d+\.' |
             Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
    if (-not $sdk) { throw "no Windows SDK found under $sdkRoot" }
    if (-not (Test-Path $BuildCorePath)) { throw "build-core.yml not found at $BuildCorePath" }
    $coreHash = (Get-FileHash -Algorithm SHA256 $BuildCorePath).Hash.ToLowerInvariant()
    [ordered]@{
        msvc_toolset      = $toolset
        windows_sdk       = $sdk
        build_core_sha256 = $coreHash
        image_version     = "$env:ImageVersion"   # evidence only, never compared
    }
}

$live = Get-ToolchainFacts

if ($Mode -eq 'dump') {
    ($live | ConvertTo-Json) | Write-Output
    exit 0
}

# check
if (-not (Test-Path $FingerprintPath)) {
    Write-Host "FINGERPRINT: FAIL -- no fingerprint committed at $FingerprintPath."
    Write-Host 'Run the cacheless CI-bytes smoke and commit its dump (design section 5 step 4).'
    exit 1
}
$committed = Get-Content $FingerprintPath -Raw | ConvertFrom-Json
$fail = 0
foreach ($k in 'msvc_toolset', 'windows_sdk', 'build_core_sha256') {
    $want = $committed.$k; $have = $live[$k]
    if ($want -ne $have) {
        Write-Host "FINGERPRINT: FAIL -- $k committed '$want' vs live '$have'"
        $fail++
    } else {
        Write-Host "FINGERPRINT: ok -- $k = $have"
    }
}
Write-Host "FINGERPRINT: image_version live '$($live['image_version'])' (evidence; committed '$($committed.image_version)')"
if ($fail -gt 0) {
    Write-Host 'FINGERPRINT: build path changed since the last proven-runnable smoke -- re-smoke + re-commit the fingerprint.'
    exit 1
}
Write-Host 'FINGERPRINT: PASS'
exit 0

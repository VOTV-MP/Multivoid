<#
.SYNOPSIS
    Deploy a UE4SS Lua probe from tools/probes/ into the game and enable it.

.DESCRIPTION
    Copies tools/probes/<Name>/ into the game's UE4SS Mods/ folder and adds
    "<Name> : 1" to Mods/mods.txt (before the built-in Keybinds entry, which
    must stay last). The game dir is gitignored, so the repo copy under
    tools/probes/ is the source of truth; this script re-deploys it.

.EXAMPLE
    ./tools/deploy-probe.ps1 -Name coopSpawnProbe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Name,
    [string]$Win64Dir = "$PSScriptRoot\..\Game_0.9.0n_HOST\WindowsNoEditor\VotV\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'

$srcMod  = Join-Path $PSScriptRoot "probes\$Name"
if (-not (Test-Path (Join-Path $srcMod 'Scripts\main.lua'))) {
    throw "Probe '$Name' not found at $srcMod\Scripts\main.lua"
}
$modsDir = (Resolve-Path $Win64Dir).Path + "\Mods"
if (-not (Test-Path $modsDir)) { throw "UE4SS Mods/ not found at $modsDir (run install-ue4ss.ps1 first)." }

$destMod = Join-Path $modsDir $Name
New-Item -ItemType Directory -Force -Path (Join-Path $destMod 'Scripts') | Out-Null
Copy-Item (Join-Path $srcMod 'Scripts\main.lua') (Join-Path $destMod 'Scripts\main.lua') -Force
Write-Host "Deployed $Name -> $destMod" -ForegroundColor Green

$modsTxt = Join-Path $modsDir 'mods.txt'
$lines = Get-Content $modsTxt
if ($lines -match [regex]::Escape($Name)) {
    Write-Host "$Name already present in mods.txt" -ForegroundColor Yellow
} else {
    $out = New-Object System.Collections.Generic.List[string]
    $inserted = $false
    foreach ($l in $lines) {
        if (-not $inserted -and $l -match '^\s*;\s*Built-in keybinds') {
            $out.Add("$Name : 1"); $out.Add(""); $inserted = $true
        }
        $out.Add($l)
    }
    if (-not $inserted) { $out.Add("$Name : 1") }
    Set-Content -Path $modsTxt -Value $out
    Write-Host "Enabled $Name in mods.txt" -ForegroundColor Green
}

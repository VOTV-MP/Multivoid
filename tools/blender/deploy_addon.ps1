# Deploy VotvIO into the local Blender 5.1 extensions dir (user_default).
# Usage: pwsh tools/blender/deploy_addon.ps1
$src = Join-Path $PSScriptRoot "votvio"
$dst = Join-Path $env:APPDATA "Blender Foundation\Blender\5.1\extensions\user_default\votvio"

if (-not (Test-Path (Join-Path $src "blender_manifest.toml"))) {
    Write-Error "source addon not found at $src"; exit 1
}
New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
robocopy $src $dst /MIR /XD "__pycache__" /XF "*.pyc" /NFL /NDL /NJH | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
$files = (Get-ChildItem $dst -Recurse -File | Measure-Object).Count
Write-Host "VotvIO deployed -> $dst ($files files)"
Write-Host "Enable it in Blender: Edit > Preferences > Add-ons > search 'VotvIO'"
exit 0

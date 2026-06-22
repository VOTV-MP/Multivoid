<#  Capture the CLIENT (and host) while the client's pile-showcase hold is active.
    Polls the CLIENT log for the "facing the pile" marker, then shoots both windows
    twice (mid-hold) so the client view frames a mirrored pile. #>
param([int]$TimeoutS = 220)
$ErrorActionPreference = 'Continue'
$root      = Split-Path $PSScriptRoot -Parent
$clientLog = Join-Path $root 'Game_0.9.0n_copy/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log'
$shotDir   = Join-Path $root 'tools/shots'
New-Item -ItemType Directory -Force $shotDir | Out-Null

function Get-PeerPid([string]$t) {
    Get-Process -Name 'VotV-Win64-Shipping' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$t*" } | Select-Object -First 1 -ExpandProperty Id
}
function Capture([string]$t, [string]$tag) {
    $procId = Get-PeerPid $t
    if (-not $procId) { Write-Host "  [$tag] no '$t' pid"; return }
    $out = Join-Path $shotDir "$tag.png"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'capture-window.ps1') `
        -ProcessId $procId -OutPath $out 2>&1 | Select-String 'OK:|ERR:|black' | ForEach-Object { Write-Host "  [$tag] $_" }
}

Write-Host "=== capture-showcase (timeout ${TimeoutS}s) -- waiting for CLIENT 'facing the pile' ==="
$t0 = Get-Date; $armed = $false
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutS) {
    if ((Test-Path $clientLog) -and (Select-String -Path $clientLog -Pattern 'CLIENT showcase -- teleported' -Quiet)) { $armed = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $armed) { Write-Host "client never reached the showcase pose within ${TimeoutS}s"; exit 1 }

Start-Sleep -Seconds 4
Write-Host "[showcase] hold active -- shooting"
Capture 'Client' 'showcase-client-a'; Capture 'Host' 'showcase-host-a'
Start-Sleep -Seconds 18
Capture 'Client' 'showcase-client-b'; Capture 'Host' 'showcase-host-b'
Write-Host "=== done ==="
Get-ChildItem $shotDir -Filter 'showcase-*.png' | Sort-Object LastWriteTime | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host

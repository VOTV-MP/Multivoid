<#
.SYNOPSIS
  Capture host+client VotV windows at the chipPile scenario's key phases (joined / carry /
  landed), so the pile sync can be eyeballed on a phone without a human at the PC.

.DESCRIPTION
  Polls the HOST votv-coop.log for the autotest_chippile phase markers and shoots BOTH
  windows (by window title -> PID) at each. Writes PNGs to tools/shots/. Run alongside a
  `mp.py smoke --duration 300 VOTVCOOP_RUN_CHIPPILE_TEST=1` background smoke.
#>
param([int]$TimeoutS = 280)
$ErrorActionPreference = 'Continue'
$root    = Split-Path $PSScriptRoot -Parent
$hostLog = Join-Path $root 'Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log'
$shotDir = Join-Path $root 'tools/shots'
New-Item -ItemType Directory -Force $shotDir | Out-Null

function Get-PeerPid([string]$titleSub) {
    Get-Process -Name 'VotV-Win64-Shipping' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$titleSub*" } |
        Select-Object -First 1 -ExpandProperty Id
}
function Capture([string]$titleSub, [string]$tag) {
    $procId = Get-PeerPid $titleSub
    if (-not $procId) { Write-Host "  [$tag] no '$titleSub' window/pid -- skipped"; return }
    $out = Join-Path $shotDir "$tag.png"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'capture-window.ps1') `
        -ProcessId $procId -OutPath $out 2>&1 | Select-String -Pattern 'OK:|ERR:|black' | ForEach-Object { Write-Host "  [$tag] $_" }
    if (Test-Path $out) { Write-Host "  [$tag] -> $out ($((Get-Item $out).Length) bytes)" }
}
function Wait-Marker([string]$rx, [int]$capS) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $capS) {
        if ((Test-Path $hostLog) -and (Select-String -Path $hostLog -Pattern $rx -Quiet)) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

Write-Host "=== capture-pile-shots (timeout ${TimeoutS}s) ==="

# 1. JOINED -- the host puppet is live on the client (both in-world).
if (Wait-Marker 'client puppet LIVE after' $TimeoutS) {
    Start-Sleep -Seconds 3
    Write-Host "[phase] JOINED"
    Capture 'Host'   'host-1-joined'
    Capture 'Client' 'client-1-joined'
}
# 2. CARRY -- the host is mid-walk with the clump in hand.
if (Wait-Marker 'moving carry step 40' $TimeoutS) {
    Write-Host "[phase] CARRY"
    Capture 'Host'   'host-2-carry'
    Capture 'Client' 'client-2-carry'
}
# 3. LANDED -- the re-pile committed; the pile sits at its settled spot.
if (Wait-Marker 'HOST LAND COMMIT' $TimeoutS) {
    Start-Sleep -Seconds 2
    Write-Host "[phase] LANDED"
    Capture 'Host'   'host-3-landed'
    Capture 'Client' 'client-3-landed'
}
Write-Host "=== done; shots in $shotDir ==="
Get-ChildItem $shotDir -Filter '*.png' | Sort-Object LastWriteTime | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host

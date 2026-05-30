# capture_window.ps1 -- capture a process's main window to a PNG.
#
# Uses PrintWindow(PW_RENDERFULLCONTENT) so it captures the rendered (DirectX)
# content of a WINDOWED UE4 game even if the window is partially occluded --
# the same path Windows uses for Alt-Tab thumbnails. Falls back to a screen-
# region BitBlt if PrintWindow yields an all-black frame (rare on some DX12
# swapchains). Used by tools/mp.py npctest for agent-run NPC-spawn verification.
#
# Usage: powershell -File capture_window.ps1 -ProcId <pid> -Out <path.png>

param(
    [Parameter(Mandatory = $true)][int]$ProcId,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = "Stop"

Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class WinCap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

$proc = Get-Process -Id $ProcId -ErrorAction Stop
$h = $proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Error "PID $ProcId has no main window"; exit 1 }

$r = New-Object WinCap+RECT
[void][WinCap]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L
$ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Error "bad window rect ${w}x${ht}"; exit 1 }

function Save-Bitmap($bmp) {
    $dir = Split-Path -Parent $Out
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Is-Black($bmp) {
    # Sample a 10x10 grid; if every sample is near-black, treat as a failed capture.
    for ($x = 0; $x -lt 10; $x++) {
        for ($y = 0; $y -lt 10; $y++) {
            $px = $bmp.GetPixel([int]($bmp.Width * $x / 10), [int]($bmp.Height * $y / 10))
            if ($px.R -gt 12 -or $px.G -gt 12 -or $px.B -gt 12) { return $false }
        }
    }
    return $true
}

# Attempt 1: PrintWindow(PW_RENDERFULLCONTENT = 2).
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][WinCap]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

if (-not (Is-Black $bmp)) {
    Save-Bitmap $bmp
    Write-Output "OK PrintWindow ${w}x${ht} -> $Out"
    exit 0
}

# Attempt 2 (fallback): bring to front + BitBlt the screen region.
[void][WinCap]::ShowWindow($h, 9)            # SW_RESTORE
[void][WinCap]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400
$bmp2 = New-Object System.Drawing.Bitmap $w, $ht
$g2 = [System.Drawing.Graphics]::FromImage($bmp2)
$g2.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$g2.Dispose()
Save-Bitmap $bmp2
Write-Output "OK BitBlt-fallback ${w}x${ht} -> $Out"
exit 0

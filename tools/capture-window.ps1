<#
.SYNOPSIS
    Capture a process's game window to a PNG (for autonomous testing).

.DESCRIPTION
    Finds the target process's top-level windows, picks the real game window
    (excluding the UE4SS debug GUI), and captures it. Tries PrintWindow
    (PW_RENDERFULLCONTENT) first; if that yields a near-black frame (common
    for some GPU swapchains), falls back to foreground + screen BitBlt.

    Run under Windows PowerShell 5.1 (has System.Drawing):
      powershell.exe -ExecutionPolicy Bypass -File tools/capture-window.ps1 ...

.PARAMETER ProcessName  e.g. VotV-Win64-Shipping
.PARAMETER OutPath      PNG path to write
.PARAMETER ExcludeTitle Substring of window titles to skip (default UE4SS GUI)
#>
param(
    [string]$ProcessName = "VotV-Win64-Shipping",
    [int]$ProcessId = 0,   # capture THIS pid (needed when two instances share the name, e.g. the LAN test)
    [string]$OutPath = "$env:TEMP\votv_cap.png",
    [string]$ExcludeTitle = "UE4SS"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Windows(uint targetPid) {
    var list = new List<IntPtr>();
    EnumWindows((h,p) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == targetPid && IsWindowVisible(h)) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }
  public static string Title(IntPtr h){ var sb=new StringBuilder(256); GetWindowText(h,sb,256); return sb.ToString(); }
}
"@
# Resolve the target PID: explicit -ProcessId wins (two same-named instances);
# else the first process matching -ProcessName.
if ($ProcessId -gt 0) {
    $targetPid = $ProcessId
} else {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { Write-Output "ERR: process '$ProcessName' not running"; exit 1 }
    $targetPid = $proc.Id
}

# Pick the game window: visible, has a client area, title doesn't match exclude.
$cand = $null
foreach ($h in [W]::Windows([uint32]$targetPid)) {
    $t = [W]::Title($h)
    if ($t -and $t -like "*$ExcludeTitle*") { continue }
    $r = New-Object W+RECT
    [void][W]::GetClientRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    Write-Output ("window hwnd={0} '{1}' client={2}x{3}" -f $h, $t, $w, $ht)
    if ($w -ge 320 -and $ht -ge 240) { $cand = $h; $cw = $w; $ch = $ht }
}
if (-not $cand) { Write-Output "ERR: no suitable game window found"; exit 2 }

function Save-Print($h,$w,$ht,$path) {
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][W]::PrintWindow($h, $hdc, 2)  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    # crude darkness check
    $s=0; for($x=0;$x -lt $w;$x+=64){ for($y=0;$y -lt $ht;$y+=64){ $c=$bmp.GetPixel($x,$y); $s+=$c.R+$c.G+$c.B } }
    $bmp.Dispose(); return $s
}
function Save-BitBlt($h,$path) {
    [void][W]::ShowWindow($h, 9); [void][W]::SetForegroundWindow($h); Start-Sleep -Milliseconds 600
    $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
    $w = $r.R-$r.L; $ht=$r.B-$r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w,$ht))
    $g.Dispose(); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

$bright = Save-Print $cand $cw $ch $OutPath
if ($bright -lt 200) {
    Write-Output "PrintWindow looked black (sum=$bright); trying foreground BitBlt"
    Save-BitBlt $cand $OutPath
}
Write-Output ("OK: wrote {0} ({1} bytes)" -f $OutPath, (Get-Item $OutPath).Length)

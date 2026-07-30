"""Cursor probe: root the "server browser is up, no cursor" bug (REPRO 2026-07-28).

The REPRO doc reproduced the symptom and falsified the filed hypothesis, then named
three remaining candidates and one decisive read. This runs that read: the DLL's
VOTVCOOP_CURSOR_PROBE=1 block logs, immediately before ImGui::Render(), every term of
the guard at imgui.cpp:6229-6230 plus RenderMouseCursor's own early-outs.

One peer, solo, browser forced open (VOTVCOOP_BROWSER_OPEN=1). The FOCUS CONTROL is
mandatory and is why the first 2026-07-28 capture was discarded: ImGui's Win32 backend
only updates io.MousePos while the game window is foreground, so an unattended run shows
no cursor whether or not the bug exists. We SetForegroundWindow + SetCursorPos into the
panel, assert the foreground window is ours, and only then read.

Run: python tools/cursor_probe.py
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import mp

OUT = mp.ROOT / "research" / "cursor_probe"
WATCH_S = 60


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    mp.kill_all()
    mp.deploy_all()
    log_path = mp.HOST_DIR / "multivoid.log"

    pid = mp.launch_peer(
        "host", mp.DEFAULT_PORT, "Probe", peer=None, res_x=1280, res_y=720,
        monitor=1, center=True, set_net_role=False,
        extra_env={"VOTVCOOP_BROWSER_OPEN": "1", "VOTVCOOP_CURSOR_PROBE": "1"})
    mp.log(f"launched pid={pid}; waiting for the overlay bring-up + browser")

    if not mp._wait_for_log(log_path, "server browser starts visible", WATCH_S, "PROBE"):
        mp.log("FAIL: browser never opened")
        mp.tail_log(log_path, 40, "PROBE")
        mp.kill_all()
        sys.exit(1)
    # The env-open runs at hook-install time, long before DXGI Present exists. The
    # overlay only draws (and CaptureActive() only matters) once BringUp has run.
    if not mp._wait_for_log(log_path, "bring-up OK", 180, "PROBE"):
        mp.log("FAIL: the overlay never brought up")
        mp.tail_log(log_path, 40, "PROBE")
        mp.kill_all()
        sys.exit(1)
    mp.log("overlay live; letting the game settle on the main menu")
    time.sleep(25)

    hwnd = mp._peer_hwnd(pid)
    if not hwnd:
        mp.log("FAIL: no window for the peer")
        mp.kill_all()
        sys.exit(1)

    # Focus control + put the OS cursor inside the client area, then hold it there.
    rc, out, err = mp.run_ps(f"""
$sig = @'
using System;
using System.Runtime.InteropServices;
public class W {{
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern int GetCursorInfo(ref CURSORINFO ci);
  [StructLayout(LayoutKind.Sequential)] public struct RECT {{ public int l,t,r,b; }}
  [StructLayout(LayoutKind.Sequential)] public struct POINT {{ public int x,y; }}
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO {{ public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }}
}}
'@
Add-Type -TypeDefinition $sig
$h = [IntPtr]{hwnd}
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$fg = [W]::GetForegroundWindow()
Write-Output ("foreground-is-game: " + ($fg -eq $h))
$r = New-Object W+RECT
[W]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object W+POINT
$p.x = [int]($r.r / 2); $p.y = [int]($r.b / 2)
[W]::ClientToScreen($h, [ref]$p) | Out-Null
[W]::SetCursorPos($p.x, $p.y) | Out-Null
Write-Output ("cursor-set-to: " + $p.x + "," + $p.y)
Start-Sleep -Milliseconds 500
$ci = New-Object W+CURSORINFO
$ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
[W]::GetCursorInfo([ref]$ci) | Out-Null
Write-Output ("cursorinfo-flags: " + $ci.flags + " at " + $ci.pt.x + "," + $ci.pt.y)
# Jiggle: a real mouse produces a stream of WM_MOUSEMOVE. One teleport may not be
# enough to make the backend leave its "tracked area" path, and the counters in the
# DLL tell us whether the messages arrive at all.
for ($i = 0; $i -lt 60; $i++) {{
  $dx = 40 * [Math]::Sin($i / 4.0); $dy = 30 * [Math]::Cos($i / 4.0)
  [W]::SetCursorPos([int]($p.x + $dx), [int]($p.y + $dy)) | Out-Null
  Start-Sleep -Milliseconds 150
}}
[W]::GetCursorInfo([ref]$ci) | Out-Null
Write-Output ("cursorinfo-flags-after-jiggle: " + $ci.flags + " at " + $ci.pt.x + "," + $ci.pt.y)
$fg2 = [W]::GetForegroundWindow()
Write-Output ("foreground-still-game: " + ($fg2 -eq $h))
""")
    mp.log(out.strip())
    if err.strip():
        mp.log("PS stderr: " + err.strip())

    shot = OUT / "probe_run.png"
    mp._capture_window(pid, shot)
    mp.log(f"captured {shot}")

    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = [l for l in text.splitlines() if "cursor_probe:" in l]
    mp.log(f"--- cursor_probe lines: {len(lines)} ---")
    for l in lines[-8:]:
        mp.log(l)
    mp.kill_all()
    if not lines:
        mp.log("FAIL: the probe never logged -- CaptureActive() was false, or the env didn't take")
        sys.exit(1)


if __name__ == "__main__":
    main()

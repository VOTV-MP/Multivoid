"""Input-ownership probe: the batched M1-M5 run for the keys/cursor arc.

One peer, host role (so s_1234 loads and `mainPlayer` exists), F1 menu forced open so
the overlay renders AND `CaptureActive()` is true -- which is the exact state in which
our SetCursorPos suppression is live. That single state answers:

  M1  reflected `UWidget::HasKeyboardFocus()` over the three engine editable-widget
      classes -- with both controls: RED is every sample while nothing is focused,
      GREEN is the one-shot round trip that sets Slate focus through the engine's own
      `SetKeyboardFocus` and re-reads.
  M1b `mainPlayer.activeInterface` + `HasFocusedDescendants()` logged beside it.
  M2  the cursor freeze WITHOUT the probe writing the pointer itself (the round-4
      confound); pass --write to arm the positive control instead.
  M3  this process's DPI awareness, against the cross-process position disagreement.
  M4  every key message and what the WndProc did with it.
  M5  ControlRotation yaw over time -- does the camera spin while capture suppresses
      the game's 120 Hz recentre.

Run: python tools/input_probe.py [--write]
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import mp

WATCH_S = 240


def main() -> None:
    want_write = "--write" in sys.argv
    mp.kill_all()
    mp.deploy_all()
    log_path = mp.HOST_DIR / "multivoid.log"

    env = {
        "VOTVCOOP_MENU_OPEN": "1",        # a surface up => the overlay renders + capture is ON
        "VOTVCOOP_CURSOR_PROBE": "1",
        "VOTVCOOP_INPUT_PROBE": "1",
        "VOTVCOOP_INPUT_PROBE_FOCUS": "1",
    }
    if want_write:
        env["VOTVCOOP_CURSOR_PROBE_WRITE"] = "1"

    pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Probe", peer=None, res_x=1280, res_y=720,
                         monitor=1, center=True, extra_env=env)
    mp.log(f"launched pid={pid} (write-test {'ARMED' if want_write else 'OFF'})")

    if not mp._wait_for_log(log_path, "bring-up OK", 180, "PROBE"):
        mp.log("FAIL: overlay never brought up")
        mp.tail_log(log_path, 40, "PROBE")
        mp.kill_all()
        sys.exit(1)

    hwnd = mp._peer_hwnd(pid)
    if hwnd:
        mp.run_ps(f"""
$sig = @'
using System;
using System.Runtime.InteropServices;
public class W {{
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}}
'@
Add-Type -TypeDefinition $sig
[W]::SetForegroundWindow([IntPtr]{hwnd}) | Out-Null
Start-Sleep -Milliseconds 500
Write-Output ("foreground-is-game: " + ([W]::GetForegroundWindow() -eq [IntPtr]{hwnd}))
""")

    # Give the world time, then create REAL widget instances to scan: TAB opens
    # ui_playerInventory (VOTV binds inventory=Tab), whose textbox_search is one of the
    # 73 measured fields. Then F1 closes our menu so a sample is taken with NO surface of
    # ours up -- the state in which the game owns text. Then T, the reported key.
    mp.log("waiting 100s for the world, then driving keys")
    time.sleep(100)
    # ORDER MATTERS: run 2 pressed TAB first and the log showed it "SWALLOWED by
    # CaptureActive" -- the forced-open F1 menu ate it, so no game UI ever opened and the
    # focus round trip had nothing real to target. Close our menu FIRST.
    # FOREGROUND IS PART OF THE PREDICATE, so every PowerShell helper that steals focus
    # invalidates the test: VOTV auto-pauses on focus loss (pause=1 in the first attempt's
    # log) and MayTakeKey()'s foreground term then answers everything, making all three
    # controls read the same. Re-assert foreground immediately before each press.
    def fg():
        mp.run_ps(f"""
$s = @'
using System; using System.Runtime.InteropServices;
public class F {{ [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }}
'@
Add-Type -TypeDefinition $s
[F]::SetForegroundWindow([IntPtr]{hwnd}) | Out-Null
""")
        time.sleep(1.0)

    fg(); mp._press_vk(pid, 0x70, "F1 (close our menu)")
    time.sleep(2)
    # TWO STATES, ONE CONTRAST. A gate that closed issue #5 by never taking the key would
    # pass state A and silently delete chat, so state B must come out DIFFERENT.
    mp.log("STATE A: game UI focused -- T must reach the GAME")
    fg(); mp._press_vk(pid, 0x09, "TAB (open inventory)")
    time.sleep(5)
    fg(); mp._press_vk(pid, 0x54, "T (state A: inventory focused)")
    time.sleep(3)
    mp.log("STATE B: no game UI -- T must be SWALLOWED by the T-chat hotkey")
    fg(); mp._press_vk(pid, 0x09, "TAB (close inventory)")
    time.sleep(5)
    fg(); mp._press_vk(pid, 0x54, "T (state B: playing)")
    time.sleep(3)

    mp.log("waiting for the world + probe samples (up to 240s)")
    deadline = time.time() + WATCH_S
    seen = 0
    while time.time() < deadline:
        time.sleep(10)
        text = log_path.read_text(encoding="utf-8", errors="replace")
        seen = text.count("input_probe: editable")
        if "FOCUS ROUND-TRIP" in text and seen >= 8:
            break
        if not any(p["PID"] == pid for p in mp.list_votv()):
            mp.log("PEER DIED")
            break

    text = log_path.read_text(encoding="utf-8", errors="replace")
    mp.kill_all()

    def show(label: str, needle: str, last: int) -> list[str]:
        rows = [l for l in text.splitlines() if needle in l]
        mp.log(f"--- {label}: {len(rows)} lines ---")
        for l in rows[-last:]:
            mp.log(l.strip())
        return rows

    show("M1/M1b/M5 input_probe", "input_probe:", 10)
    show("M1 GREEN control", "FOCUS ROUND-TRIP", 3)
    show("M2/M3 cursor_probe", "cursor_probe:", 3)
    keys = show("M4 key_probe", "key_probe:", 12)

    # M5 verdict: does yaw drift monotonically while capture is on?
    yaws = [float(m) for m in re.findall(r"yaw=(-?\d+\.\d+)", text)]
    if len(yaws) >= 4:
        span = max(yaws) - min(yaws)
        mp.log(f"M5: {len(yaws)} yaw samples, min={min(yaws):.2f} max={max(yaws):.2f} "
               f"span={span:.2f} -> {'CAMERA MOVED' if span > 1.0 else 'camera steady'}")
    else:
        mp.log(f"M5: only {len(yaws)} yaw samples -- inconclusive")
    if not keys:
        mp.log("M4: no key messages seen (nothing typed at the window) -- expected for an "
               "unattended run; the RED is that the probe is wired, not that keys arrived")


if __name__ == "__main__":
    main()

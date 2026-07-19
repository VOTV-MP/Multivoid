"""Native-launch -> MENU (not gameplay) regression probe.

User bug (2026-06-06): launching VotV.exe NATIVELY (double-click / Steam) booted
straight into gameplay, because a leftover scenario.txt="play" in the game dir
aliased the native launch into the autotest "play" scenario. The fix: the harness
scenario is driven ONLY by the process-scoped VOTVCOOP_SCENARIO env var (set by the
test launchers); a native launch has no such env -> ReadScenario() returns "menu" ->
the harness boots to VOTV's own main menu and does NOT auto-load a save.

This probe launches VotV.exe with a SCRUBBED env (every VOTVCOOP_* var removed) =
exactly what a native double-click sees, then reads the log:

  PASS iff:  scenario='menu'  AND  "MENU mode (native launch)" present
             AND none of the gameplay markers ("target STORY save" / "PLAY READY" /
             "COOP SESSION START" / "skip-to-gameplay") appear.

The game sits at the OMEGA content-warning (menu mode never auto-proceeds -- that is
a TEST-only helper), which is the correct native behaviour; we only need the log to
prove the harness took the menu branch, not the gameplay branch.

Run: python tools/menu_boot_probe.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

import mp

WATCH_S = 35
LOG = mp.HOST_DIR / "multivoid.log"
GAMEPLAY_MARKERS = [
    "target STORY save",
    "==== PLAY READY ====",
    "COOP SESSION START",
    "skip-to-gameplay",
    "FRESH New Game",
]


def main() -> None:
    mp.kill_all()
    mp.deploy_all()

    # Scrub EVERY VOTVCOOP_* var so the child sees exactly what a native launch sees.
    env = {k: v for k, v in os.environ.items() if not k.startswith("VOTVCOOP_")}
    scrubbed = sorted(k for k in os.environ if k.startswith("VOTVCOOP_"))
    mp.log(f"native-launch sim: removed {len(scrubbed)} VOTVCOOP_* env vars: {scrubbed}")

    try:
        LOG.unlink()
    except FileNotFoundError:
        pass

    exe = str(mp.HOST_DIR / mp.VOTV_EXE)
    mp.log(f"launching NATIVE (no VOTVCOOP_* env): {exe}")
    proc = subprocess.Popen(
        [exe, "-windowed", "-ResX=1280", "-ResY=720"],
        cwd=str(mp.HOST_DIR), env=env,
        creationflags=mp.DETACHED_PROCESS | mp.CREATE_NEW_PROCESS_GROUP,
    )

    scenario_menu = menu_mode = False
    gameplay_hit: list[str] = []
    t0 = time.time()
    try:
        while time.time() - t0 < WATCH_S:
            time.sleep(3)
            txt = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
            scenario_menu = "timeline start, scenario='menu'" in txt
            menu_mode = "MENU mode (native launch)" in txt
            gameplay_hit = [m for m in GAMEPLAY_MARKERS if m in txt]
            t = int(time.time() - t0)
            mp.log(f"  t={t}s scenario_menu={scenario_menu} menu_mode={menu_mode} "
                   f"gameplay_markers={gameplay_hit}")
            if menu_mode and scenario_menu:
                break
    finally:
        mp.tail_log(LOG, 20, "NATIVE")
        mp.kill_all()

    ok = scenario_menu and menu_mode and not gameplay_hit
    mp.log("--- VERDICT ---")
    mp.log(f"  scenario == 'menu'                 : {scenario_menu}")
    mp.log(f"  'MENU mode (native launch)' logged : {menu_mode}")
    mp.log(f"  NO gameplay markers                : {not gameplay_hit}  {gameplay_hit}")
    mp.log(f"{'PASS' if ok else 'FAIL'}: a native launch (no test env) "
           + ("boots to the MENU and does NOT auto-load gameplay."
              if ok else "did NOT cleanly enter menu mode (see markers/log)."))
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()

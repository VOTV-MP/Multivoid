"""Verify ue_wrap::save_browser against the running VOTV build (native menu state).

Before layering the ImGui Host-Game save picker on top, prove the NATIVE save
enumeration works: launch VotV.exe in MENU mode (no scenario env) with the dev probe
VOTVCOOP_TEST_SAVE_ENUM=1, which a few seconds after boot drives
ue_wrap::save_browser::EnumerateSaves (which drives VOTV's own loadSlots) and logs the
harvested save list + metadata.

Pass VOTVCOOP_TEST_SAVE_CREATE=<name> (via --create) to ALSO create a story save then
re-enumerate (writes <s_name>.sav -- a real disk side effect; only with --create).

PASS = the log shows `save_probe[enum1]: EnumerateSaves ok=1` (the native harvest
resolved). The per-save `save_browser: [i] '<slot>' ...` lines show what was found.

Run: python tools/save_enum_probe.py          (enumerate only)
     python tools/save_enum_probe.py --create coopProbe   (also create+persist)
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

import mp

WATCH_S = 40
LOG = mp.HOST_DIR / "multivoid.log"


def main() -> None:
    create = ""
    if "--create" in sys.argv:
        i = sys.argv.index("--create")
        if i + 1 < len(sys.argv):
            create = sys.argv[i + 1]

    mp.kill_all()
    mp.deploy_all()

    env = {k: v for k, v in os.environ.items() if not k.startswith("VOTVCOOP_")}
    env["VOTVCOOP_TEST_SAVE_ENUM"] = "1"   # arm the save_probe
    if create:
        env["VOTVCOOP_TEST_SAVE_CREATE"] = create
        mp.log(f"create test ON: will CreateNamedSave('{create}') -> s_{create}.sav")
    # NO VOTVCOOP_SCENARIO -> menu mode (the native picker context).

    try:
        LOG.unlink()
    except FileNotFoundError:
        pass

    exe = str(mp.HOST_DIR / mp.VOTV_EXE)
    mp.log(f"launching MENU mode + save probe: {exe}")
    proc = subprocess.Popen(
        [exe, "-windowed", "-ResX=1280", "-ResY=720"],
        cwd=str(mp.HOST_DIR), env=env,
        creationflags=mp.DETACHED_PROCESS | mp.CREATE_NEW_PROCESS_GROUP,
    )

    enum_ok = False
    t0 = time.time()
    try:
        while time.time() - t0 < WATCH_S:
            time.sleep(3)
            txt = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
            done = "save_probe: DONE" in txt
            enum_ok = "save_probe[enum1]: EnumerateSaves ok=1" in txt
            t = int(time.time() - t0)
            mp.log(f"  t={t}s enum_ok={enum_ok} done={done}")
            if done:
                break
    finally:
        # Dump every save_browser / save_probe line (the actual evidence).
        txt = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
        mp.log("--- save_browser / save_probe log lines ---")
        for line in txt.splitlines():
            if "save_browser:" in line or "save_probe" in line:
                mp.log("  " + line)
        mp.kill_all()

    code = 0 if enum_ok else 2
    mp.log("--- VERDICT ---")
    mp.log(f"{'PASS' if enum_ok else 'FAIL'}: native EnumerateSaves "
           + ("resolved (drove VOTV loadSlots, harvested the save list)."
              if enum_ok else "did NOT resolve (see save_browser lines / offsets above)."))
    sys.exit(code)


if __name__ == "__main__":
    main()

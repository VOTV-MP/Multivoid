"""Connect-FAILURE probe -- regressions C + D (2026-06-06).

Reproduces the user's Method-2 hands-on bug: a NATIVE menu-mode launch where the
client tries to Direct-connect to a DEAD address (nothing listening on 127.0.0.1:7777).
Before the fix the loading screen hung on "Connecting..." forever AND net_pump kept
pumping the full gameplay tick at the menu every frame -> lag + RAM balloon.

This launches ONE client in MENU mode (set_scenario=None -> no VOTVCOOP_SCENARIO) with NO
env net role (set_net_role=False), and the imgui test hook VOTVCOOP_TEST_CONNECT_DIRECT
pointed at a dead local port. It is the exact browser "Direct connect" path (ConnectDirect
-> BeginConnect -> harness TakePendingStart -> StartCoopSession -> GNS connect -> fail).

PASS requires, in the client log:
  * "DIRECT connect queued"                 (ConnectDirect fired -> BeginConnect raised)
  * "COOP SESSION START (client / lan-direct)"  (the session started -> GNS dialing)
  * "join FAILED"                           (net_pump connect-fail detector -> jp::Fail)
  * "join aborted (cancelled or failed)"    (harness drained the abort -> Stop)
  * "Reset -- loading screen hidden"        (the cover came down)
AND the process stays ALIVE (no OOM/crash) AND RSS stays bounded (no balloon).

Run: python tools/connect_fail_probe.py
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import mp

DEAD_ADDR = "127.0.0.1:7777"   # nothing listens here -> GNS connect fails after its timeout
WATCH_S = 45                   # cover the ~10s GNS connect timeout + the abort + idle settle
# A menu-mode client sits ~2-4 GB. A balloon (the bug) climbs past this and keeps growing.
RSS_BALLOON_MB = 7000


def grep(path: Path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def main() -> None:
    mp.kill_all()
    mp.deploy_all()
    client_log = mp.CLIENT_DIR / "votv-coop.log"
    code = 2
    rss_samples: list[float] = []
    try:
        mp.log(f"--- CLIENT (MENU mode, NO env role; ConnectDirect -> DEAD {DEAD_ADDR}) ---")
        client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer=None,
                                    res_x=1280, res_y=720, monitor=1, center=True,
                                    set_net_role=False, set_scenario=None,
                                    extra_env={"VOTVCOOP_TEST_CONNECT_DIRECT": DEAD_ADDR})

        mp.log(f"--- watching {WATCH_S}s for the connect-fail -> abort sequence + RSS ---")
        t0 = time.time()
        died = False
        while time.time() - t0 < WATCH_S:
            time.sleep(4)
            procs = [p for p in mp.list_votv() if p["PID"] == client_pid]
            alive = bool(procs)
            rss = procs[0]["RSS_MB"] if procs else -1.0
            if rss > 0:
                rss_samples.append(rss)
            t = int(time.time() - t0)
            queued = grep(client_log, "DIRECT connect queued") > 0
            started = grep(client_log, "COOP SESSION START (client / lan-direct)") > 0
            failed = grep(client_log, "join FAILED") > 0
            aborted = grep(client_log, "join aborted") > 0
            hidden = grep(client_log, "Reset -- loading screen hidden") > 0
            mp.log(f"  t={t}s alive={alive} RSS={rss}MB | queued={queued} started={started} "
                   f"failed={failed} aborted={aborted} hidden={hidden}")
            if not alive:
                mp.log("  CLIENT PROCESS DIED (crash/OOM) -- stopping watch")
                died = True
                break
            if rss > RSS_BALLOON_MB:
                mp.log(f"  RSS BALLOON: {rss}MB > {RSS_BALLOON_MB}MB threshold -- the D bug")
                break

        queued = grep(client_log, "DIRECT connect queued") > 0
        started = grep(client_log, "COOP SESSION START (client / lan-direct)") > 0
        failed = grep(client_log, "join FAILED") > 0
        aborted = grep(client_log, "join aborted") > 0
        hidden = grep(client_log, "Reset -- loading screen hidden") > 0
        max_rss = max(rss_samples) if rss_samples else -1.0
        last_rss = rss_samples[-1] if rss_samples else -1.0
        rss_ok = (max_rss > 0) and (max_rss <= RSS_BALLOON_MB) and not died

        ok = queued and started and failed and aborted and hidden and rss_ok
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  ConnectDirect queued (BeginConnect)   : {queued}")
        mp.log(f"  session started (client/lan-direct)   : {started}")
        mp.log(f"  connect-fail detector fired (Fail)    : {failed}")
        mp.log(f"  harness drained abort (Stop)          : {aborted}")
        mp.log(f"  loading screen hidden (Reset)         : {hidden}")
        mp.log(f"  RSS bounded (max={max_rss}MB last={last_rss}MB cap={RSS_BALLOON_MB}MB, "
               f"no crash): {rss_ok}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: a dead-address Direct connect "
               f"{'cleanly failed -> aborted -> loading hidden, RSS stable (regressions C+D fixed)' if ok else 'did NOT complete the fail->abort->hide sequence or ballooned (see markers)'}.")
    finally:
        mp.tail_log(client_log, 40, "CLIENT")
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

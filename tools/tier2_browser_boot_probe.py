"""Tier-2 browser-boot probe: a browser-initiated session boots g_session + connects.

The harness RunPlayLoop polls session_manager::TakePendingStart and boots g_session when
a browser action (Host/Join/Direct) queues a Config -- the Tier-2 mechanism. This proves
it END-TO-END WITHOUT a click: the client boots plain "play" with NO env net role, and the
imgui test hook VOTVCOOP_TEST_CONNECT_DIRECT queues a LanDirect connect to a REAL env-path
host. If the client connects, the browser->g_session boot path works.

PASS = the client log shows the ConnectDirect queued + "harness: browser-initiated coop
session" + "COOP SESSION START (client / lan-direct)" + "host assigned us peer slot"
(connected to the host via the browser-boot path), and the host shows "host accepted
client at slot".

Run: python tools/tier2_browser_boot_probe.py
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import mp

PORT = mp.DEFAULT_PORT
WATCH_S = 80


def grep(path: Path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def main() -> None:
    mp.kill_all()
    mp.deploy_all()
    host_log = mp.HOST_DIR / "votv-coop.log"
    client_log = mp.CLIENT_DIR / "votv-coop.log"
    code = 2
    try:
        mp.log("--- HOST (env path, LanDirect) ---")
        host_pid = mp.launch_peer("host", PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True)
        mp.log("waiting up to 90s for the host to bind UDP...")
        bound = False
        for i in range(90):
            time.sleep(1)
            if mp.host_owns_udp(host_pid, PORT):
                bound = True
                mp.log(f"host bound UDP {PORT} after {i + 1}s")
                break
            if not any(p["PID"] == host_pid for p in mp.list_votv()):
                mp.log("HOST DIED before binding UDP")
                mp.tail_log(host_log, 30, "HOST")
                sys.exit(1)
        if not bound:
            mp.log("FAIL: host did not bind UDP")
            mp.kill_all()
            sys.exit(1)

        mp.log("--- CLIENT (NO env role; browser ConnectDirect via the imgui test hook) ---")
        client_pid = mp.launch_peer("client", PORT, "Client", peer=None,
                                    res_x=1280, res_y=720, monitor=2, tile_index=0,
                                    set_net_role=False,
                                    extra_env={"VOTVCOOP_TEST_CONNECT_DIRECT": f"127.0.0.1:{PORT}"})

        mp.log(f"--- watching {WATCH_S}s for the browser-boot connect ---")
        queued = booted = connected = accepted = False
        t0 = time.time()
        while time.time() - t0 < WATCH_S:
            time.sleep(3)
            alive = any(p["PID"] == client_pid for p in mp.list_votv())
            queued = grep(client_log, "queued a browser-path session start") > 0
            booted = grep(client_log, "browser-initiated coop session") > 0
            connected = grep(client_log, "host assigned us peer slot") > 0
            accepted = grep(host_log, "host accepted client at slot") > 0
            t = int(time.time() - t0)
            mp.log(f"  t={t}s alive={alive} queued={queued} booted={booted} "
                   f"connected={connected} accepted={accepted}")
            if not alive:
                mp.log("  client process died -- stopping watch")
                break
            if connected and accepted:
                break

        ok = queued and booted and connected and accepted
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  ConnectDirect queued (test hook)  : {queued}")
        mp.log(f"  RunPlayLoop consumed + booted      : {booted}")
        mp.log(f"  client connected to the host       : {connected}")
        mp.log(f"  host accepted the client           : {accepted}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: a browser-initiated (ConnectDirect, no env role) "
               f"session {'booted g_session + connected to the host' if ok else 'did NOT complete the browser-boot connect (see markers)'}.")
    finally:
        mp.tail_log(host_log, 18, "HOST")
        mp.tail_log(client_log, 22, "CLIENT")
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

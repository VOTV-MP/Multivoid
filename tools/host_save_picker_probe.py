"""Does the Host-Game SAVE PICKER host a joinable lobby on a CHOSEN save?

Drives the picker's "Host selected save" path end-to-end against a LOCAL master +
signaling, in MENU mode (the real picker context -- no scenario env):
  1. HOST boots to the MENU; the imgui test hook VOTVCOOP_TEST_HOST_SAVE=<slot> fires
     session_manager::HostWithSave({existing slot}) (the exact path the picker's
     DoHostExisting runs) -> announce to the master -> a pending host-with-save.
  2. The harness DriveHostBootIfPending LOADS <slot> (engine::LoadStorySave, polled)
     THEN StartCoopSession(host / p2p) -- the load-then-host orchestration.
  3. probe reads the host's lobbyId off the master, launches a CLIENT that JOINs it
     (VOTVCOOP_TEST_JOIN_LOBBY) -> a P2P client that dials the host.
  4. PASS iff the host LOADED the chosen world + booted P2P AND the browser client
     connected (host "host accepted client at slot" + client "host assigned us peer slot").

Same machinery as tier2_host_button_probe, but the host path is HostWithSave (load a
chosen save first) instead of HostLobby (host on the already-loaded world). Same-machine
ICE uses host candidates (no STUN/TURN needed).

Run: python tools/host_save_picker_probe.py [slot]   (slot default s_may2026)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request

import mp

MASTER = mp.ROOT / "tools" / "coop_master_server.py"
SIGNAL = mp.ROOT / "tools" / "coop_signaling_server.py"
MLOG = mp.ROOT / "build" / "picker-master.log"
SLOG = mp.ROOT / "build" / "picker-signaling.log"
MPORT = 10001
SPORT = 10000
TOKEN = "probe-token"
SLOT = sys.argv[1] if len(sys.argv) > 1 else "s_may2026"
HOST_BOOT_S = 90     # load the ~19MB save + boot P2P
WATCH_S = 80


def get_lobbies() -> list:
    with urllib.request.urlopen(f"http://127.0.0.1:{MPORT}/v1/lobbies", timeout=8) as r:
        return json.loads(r.read() or b"{}").get("lobbies", [])


def grep(path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def main() -> None:
    mp.kill_all()
    mp.deploy_all()
    MLOG.parent.mkdir(parents=True, exist_ok=True)

    senv = dict(os.environ, COOP_SIGNALING_PORT=str(SPORT), COOP_SIGNALING_TOKEN=TOKEN)
    slog = open(SLOG, "w", encoding="utf-8")
    sig = subprocess.Popen([sys.executable, str(SIGNAL)], env=senv, stdout=slog,
                           stderr=subprocess.STDOUT, text=True)
    menv = dict(os.environ, COOP_MASTER_PORT=str(MPORT), COOP_TURN_SECRET="probe-turn-secret",
                COOP_SIGNALING_TOKEN=TOKEN, COOP_SIGNALING_URL=f"127.0.0.1:{SPORT}",
                COOP_STUN_URI="stun:127.0.0.1:3478", COOP_TURN_URI="turn:127.0.0.1:3478")
    mlog = open(MLOG, "w", encoding="utf-8")
    master = subprocess.Popen([sys.executable, str(MASTER)], env=menv, stdout=mlog,
                              stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    if sig.poll() is not None or master.poll() is not None:
        print("FATAL: signaling/master exited immediately")
        sys.exit(1)

    host_log = mp.HOST_DIR / "votv-coop.log"
    client_log = mp.CLIENT_DIR / "votv-coop.log"
    lobby_id = None
    host_booted = world_loaded = connected = accepted = False
    code = 2
    try:
        mp.log(f"--- HOST (TRUE menu mode; picker 'Host selected save' = {SLOT} via the test hook) ---")
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  set_net_role=False,
                                  set_scenario=None,  # MENU mode: the host loads SLOT from the menu (not pre-loaded)
                                  extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{MPORT}",
                                             "VOTVCOOP_TEST_HOST_SAVE": SLOT})
        mp.log(f"waiting up to {HOST_BOOT_S}s for the host to announce + LOAD '{SLOT}' + boot P2P...")
        t0 = time.time()
        while time.time() - t0 < HOST_BOOT_S:
            time.sleep(2)
            if not lobby_id:
                try:
                    rows = get_lobbies()
                except Exception:  # noqa: BLE001
                    rows = []
                if rows:
                    lobby_id = rows[0]["lobbyId"]
                    mp.log(f"host ANNOUNCED lobby '{rows[0].get('name')}' world='{rows[0].get('world')}' id={lobby_id}")
            world_loaded = world_loaded or grep(host_log, "host-with-save world loaded") > 0
            host_booted = host_booted or grep(host_log, "COOP SESSION START (host / p2p)") > 0
            if host_booted:
                mp.log("host BOOTED the P2P session (world loaded, hosting)")
                break
            if not any(p["PID"] == host_pid for p in mp.list_votv()):
                mp.log("HOST DIED before hosting")
                break

        if lobby_id and host_booted:
            mp.log(f"--- CLIENT (Join lobby {lobby_id} via the test hook) ---")
            client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer=None,
                                        res_x=1280, res_y=720, monitor=2, tile_index=0,
                                        set_net_role=False,
                                        extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{MPORT}",
                                                   "VOTVCOOP_TEST_JOIN_LOBBY": lobby_id})
            mp.log(f"--- watching {WATCH_S}s for the P2P connect ---")
            t0 = time.time()
            while time.time() - t0 < WATCH_S:
                time.sleep(3)
                connected = grep(client_log, "host assigned us peer slot") > 0
                accepted = grep(host_log, "host accepted client at slot") > 0
                t = int(time.time() - t0)
                mp.log(f"  t={t}s connected={connected} accepted={accepted}")
                if connected and accepted:
                    break
                if not any(p["PID"] == client_pid for p in mp.list_votv()):
                    mp.log("  client process died -- stopping watch")
                    break

        ok = (lobby_id is not None) and world_loaded and host_booted and connected and accepted
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  host announced a lobby (HostWithSave)  : {lobby_id is not None}")
        mp.log(f"  host LOADED the chosen save ('{SLOT}') : {world_loaded}")
        mp.log(f"  host booted the P2P session            : {host_booted}")
        mp.log(f"  client joined + connected (P2P)        : {connected}")
        mp.log(f"  host accepted the client               : {accepted}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: the Host-Game save picker "
               + (f"hosts a joinable lobby on the CHOSEN save '{SLOT}' (load-then-host + a client connected)."
                  if ok else "did NOT complete the pick->load->host->join loop (see markers/logs)."))
    finally:
        mp.tail_log(host_log, 20, "HOST")
        mp.tail_log(client_log, 14, "CLIENT")
        for proc, fh, lg, name in ((sig, slog, SLOG, "SIGNALING"), (master, mlog, MLOG, "MASTER")):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
            fh.close()
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

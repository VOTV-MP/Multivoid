"""Does the browser "Host Game" button actually host a joinable P2P lobby?

Drives the EXACT browser paths end-to-end against a LOCAL master + signaling:
  1. HOST instance fires session_manager::HostLobby (the DoHost() path) via the test
     hook -> POST /v1/host -> a P2P host session that registers with signaling.
  2. probe reads the host's lobbyId off the master (GET /v1/lobbies).
  3. CLIENT instance fires session_manager::JoinLobby(lobbyId) (the row-Connect path)
     -> POST /v1/join -> a P2P client session that dials the host via signaling/ICE.
  4. PASS iff the host announced + booted P2P AND the browser client CONNECTED (host
     "host accepted client at slot" + client "host assigned us peer slot").

This is p2p_smoke driven through the BROWSER (HostLobby/JoinLobby), not the env config,
so it answers "is the Host Game button working?" definitively. Same-machine ICE uses
host candidates (no STUN/TURN needed), exactly like the validated p2p_smoke.

Run: python tools/tier2_host_button_probe.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import mp

MASTER = mp.ROOT / "tools" / "coop_master_server.py"
SIGNAL = mp.ROOT / "tools" / "coop_signaling_server.py"
MLOG = mp.ROOT / "build" / "host-button-master.log"
SLOG = mp.ROOT / "build" / "host-button-signaling.log"
MPORT = 10001
SPORT = 10000
TOKEN = "probe-token"
HOST_BOOT_S = 35
WATCH_S = 75


def get_lobbies() -> list:
    with urllib.request.urlopen(f"http://127.0.0.1:{MPORT}/v1/lobbies", timeout=8) as r:
        return json.loads(r.read() or b"{}").get("lobbies", [])


def grep(path: Path, needle: str) -> int:
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
    host_booted = connected = accepted = False
    code = 2
    try:
        mp.log("--- HOST (browser 'Host Game' button via the test hook) ---")
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  set_net_role=False,
                                  extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{MPORT}",
                                             "VOTVCOOP_TEST_HOST_LOBBY": "1"})
        mp.log(f"waiting up to {HOST_BOOT_S}s for the host to announce + boot the P2P session...")
        t0 = time.time()
        while time.time() - t0 < HOST_BOOT_S:
            time.sleep(2)
            try:
                rows = get_lobbies()
            except Exception:  # noqa: BLE001
                rows = []
            if rows:
                lobby_id = rows[0]["lobbyId"]
                mp.log(f"host ANNOUNCED lobby '{rows[0].get('name')}' id={lobby_id} "
                       f"({rows[0].get('players_cur')}/{rows[0].get('players_max')})")
                break
            if not any(p["PID"] == host_pid for p in mp.list_votv()):
                mp.log("HOST DIED before announcing")
                break

        if lobby_id:
            mp.log(f"--- CLIENT (browser Join of lobby {lobby_id} via the test hook) ---")
            client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer=None,
                                        res_x=1280, res_y=720, monitor=2, tile_index=0,
                                        set_net_role=False,
                                        extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{MPORT}",
                                                   "VOTVCOOP_TEST_JOIN_LOBBY": lobby_id})
            mp.log(f"--- watching {WATCH_S}s for the P2P connect ---")
            t0 = time.time()
            while time.time() - t0 < WATCH_S:
                time.sleep(3)
                host_booted = host_booted or grep(host_log, "COOP SESSION START (host / p2p)") > 0
                connected = grep(client_log, "host assigned us peer slot") > 0
                accepted = grep(host_log, "host accepted client at slot") > 0
                t = int(time.time() - t0)
                mp.log(f"  t={t}s host_booted={host_booted} connected={connected} accepted={accepted}")
                if connected and accepted:
                    break
                if not any(p["PID"] == client_pid for p in mp.list_votv()):
                    mp.log("  client process died -- stopping watch")
                    break

        host_booted = host_booted or grep(host_log, "COOP SESSION START (host / p2p)") > 0
        ok = (lobby_id is not None) and host_booted and connected and accepted
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  host announced a lobby (POST /v1/host) : {lobby_id is not None}")
        mp.log(f"  host booted a P2P session               : {host_booted}")
        mp.log(f"  browser client joined + connected (P2P) : {connected}")
        mp.log(f"  host accepted the client                : {accepted}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: the browser Host-Game button "
               + ("HOSTS A JOINABLE P2P LOBBY (announced + booted + a browser client connected)."
                  if ok else "did NOT complete the host->announce->join->P2P loop (see markers/logs)."))
    finally:
        mp.tail_log(host_log, 16, "HOST")
        mp.tail_log(client_log, 16, "CLIENT")
        for proc, fh, lg, name in ((sig, slog, SLOG, "SIGNALING"), (master, mlog, MLOG, "MASTER")):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
            fh.close()
            mp.log(f"--- {name} LOG (tail) ---")
            for line in lg.read_text(errors="replace").splitlines()[-15:]:
                mp.log(f"  {name.lower()}: {line}")
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

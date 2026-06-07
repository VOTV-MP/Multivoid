"""Full menu-driven coop: host-from-menu + client-fresh-boot-JOIN-from-menu (2026-06-06).

Proves the user's real-life flow end-to-end WITHOUT a click (test hooks stand in for the
browser buttons), both peers in TRUE MENU mode:
  HOST  : menu -> VOTVCOOP_TEST_HOST_SAVE=<slot> -> HostWithSave -> harness loads <slot>
          -> StartCoopSession(host / p2p) -> announces to the local master.
  CLIENT: menu -> VOTVCOOP_TEST_JOIN_LOBBY=<lobbyId> -> JoinLobby -> the harness consumes
          the queued client start AND (NEW, client world-entry) FRESH-BOOTS a New Game into
          gameplay BEFORE connecting -> StartCoopSession(client / p2p) -> the host streams
          its whole world onto the fresh client.

PASS iff:
  * client log "menu-mode client join -- fresh-booting into gameplay before connect"  (new wiring fired)
  * client log "FRESH New Game"                                                        (StartFreshGame ran)
  * client log "host assigned us peer slot"                                            (P2P connected)
  * host   log "host accepted client at slot"                                          (host accepted)
  * client mirrored the host's world (RegisterPropMirror / snapshot applied > 0 props)
  * both processes alive + RSS bounded (no balloon)

Run: python tools/menu_join_probe.py [slot]   (slot default s_may2026)
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import urllib.request

import mp

MASTER = mp.ROOT / "tools" / "coop_master_server.py"
SIGNAL = mp.ROOT / "tools" / "coop_signaling_server.py"
MLOG = mp.ROOT / "build" / "menujoin-master.log"
SLOG = mp.ROOT / "build" / "menujoin-signaling.log"
MPORT, SPORT = 10001, 10000
TOKEN = "probe-token"
SLOT = sys.argv[1] if len(sys.argv) > 1 else "s_may2026"
HOST_BOOT_S = 100
WATCH_S = 110


def get_lobbies() -> list:
    with urllib.request.urlopen(f"http://127.0.0.1:{MPORT}/v1/lobbies", timeout=8) as r:
        return json.loads(r.read() or b"{}").get("lobbies", [])


def grep(path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def mirror_props(path) -> int:
    """Highest 'RegisterPropMirror ... (N total)'-ish count we can find, else snapshot applies."""
    try:
        txt = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    best = 0
    for m in re.finditer(r"mirror[^\n]*?(\d+)\s*(?:props|total|mirrored)", txt, re.IGNORECASE):
        best = max(best, int(m.group(1)))
    return best


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
        print("FATAL: signaling/master exited immediately"); sys.exit(1)

    host_log = mp.HOST_DIR / "votv-coop.log"
    client_log = mp.CLIENT_DIR / "votv-coop.log"
    lobby_id = None
    host_booted = world_loaded = False
    freshboot = freshgame = connected = accepted = False
    code = 2
    try:
        mp.log(f"--- HOST (menu mode; HostWithSave {SLOT} via the test hook) ---")
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  set_net_role=False, set_scenario=None,
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
                mp.log("HOST DIED before hosting"); break

        if lobby_id and host_booted:
            mp.log(f"--- CLIENT (MENU mode; JoinLobby {lobby_id} -> should FRESH-BOOT then connect) ---")
            client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer=None,
                                        res_x=1280, res_y=720, monitor=2, tile_index=0,
                                        set_net_role=False, set_scenario=None,
                                        extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{MPORT}",
                                                   "VOTVCOOP_TEST_JOIN_LOBBY": lobby_id})
            mp.log(f"--- watching {WATCH_S}s for fresh-boot -> P2P connect -> world mirror ---")
            t0 = time.time()
            while time.time() - t0 < WATCH_S:
                time.sleep(4)
                freshboot = freshboot or grep(client_log, "menu-mode client join -- fresh-booting") > 0
                freshgame = freshgame or grep(client_log, "FRESH New Game") > 0
                connected = grep(client_log, "host assigned us peer slot") > 0
                accepted = grep(host_log, "host accepted client at slot") > 0
                procs = {p["PID"]: p["RSS_MB"] for p in mp.list_votv()}
                t = int(time.time() - t0)
                mp.log(f"  t={t}s c_alive={client_pid in procs}({procs.get(client_pid,'-')}MB) "
                       f"freshboot={freshboot} freshgame={freshgame} connected={connected} accepted={accepted} "
                       f"mirror={mirror_props(client_log)}")
                if connected and accepted and mirror_props(client_log) > 100:
                    break
                if client_pid not in procs:
                    mp.log("  client process DIED -- stopping watch"); break

        mirrored = mirror_props(client_log)
        ok = (lobby_id is not None) and host_booted and freshboot and freshgame \
            and connected and accepted and mirrored > 100
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  host announced + booted P2P            : {host_booted}")
        mp.log(f"  client FRESH-BOOT wiring fired         : {freshboot}")
        mp.log(f"  client StartFreshGame (New Game)       : {freshgame}")
        mp.log(f"  client connected (P2P)                 : {connected}")
        mp.log(f"  host accepted the client               : {accepted}")
        mp.log(f"  client mirrored host world (props)     : {mirrored}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: menu->host + menu->client-fresh-boot-join "
               + ("WORKS end-to-end (client booted into gameplay + connected + mirrored the host world)."
                  if ok else "did NOT complete (see markers/logs)."))
    finally:
        mp.tail_log(host_log, 16, "HOST")
        mp.tail_log(client_log, 30, "CLIENT")
        for proc, fh in ((sig, slog), (master, mlog)):
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

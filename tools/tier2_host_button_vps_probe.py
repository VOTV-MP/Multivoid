"""Does the browser "Host Game" -> Join flow work against the LIVE VPS master?

Same as tier2_host_button_probe.py, but points both peers at the DEPLOYED VPS
master/signaling instead of spawning a local pair -- so it validates the actual
deploy (systemd coop-master on :10001, the coturn use-auth-secret switch, and the
real-internet reachability), not just the master CODE.

  1. HOST fires session_manager::HostLobby via VOTVCOOP_TEST_HOST_LOBBY=1 -> POST
     /v1/host to the VPS -> a P2P host session that registers with the VPS signaling.
  2. probe reads the host's lobbyId off the VPS master (GET /v1/lobbies).
  3. CLIENT fires session_manager::JoinLobby(lobbyId) via VOTVCOOP_TEST_JOIN_LOBBY
     -> POST /v1/join to the VPS -> a P2P client that dials the host via VPS signaling.
  4. PASS iff the host announced + booted P2P AND the browser client CONNECTED.

Both peers run on THIS machine (same NAT) -> ICE uses host candidates, so this proves
the master+signaling rendezvous over the real internet (NOT cross-NAT relay; that was
validated separately via p2p_smoke --ice relay). The VPS IP comes from the gitignored
creds file -- this script carries no secret.

Run: python tools/tier2_host_button_vps_probe.py
"""

from __future__ import annotations

import json
import sys
import time
import urllib.request
from pathlib import Path

import mp


def _https_ctx():
    """TLS context with FULL verification, but against certifi's roots.

    Measured 2026-07-20: Python on this box rejects Let's Encrypt's ECDSA chain
    ("certificate has expired") out of the stale Windows store, while schannel --
    what the MOD itself uses via WinHTTP -- and openssl both verify it fine. This
    is a Python-tooling artifact; do not read it as a server fault.
    """
    import ssl
    ctx = ssl.create_default_context()
    try:
        import certifi
        ctx.load_verify_locations(cafile=certifi.where())
    except ImportError:
        pass
    return ctx

HOST_BOOT_S = 40
WATCH_S = 80


def main() -> None:
    # Tier B: the master is TLS-only from the client's point of view, and a
    # certificate is valid for a NAME -- dialling the bare IP this probe used to
    # build would fail hostname validation. Schemeless = secure, matching the
    # mod's own URL grammar, so this is what the game is handed too.
    master_url = "master.multivoid.dev:10443"
    lobbies_url = f"https://{master_url}/v1/lobbies"
    mp.log(f"--- VPS host-button probe (master={master_url}) ---")

    # sanity: the VPS master must answer before we launch heavy game instances
    try:
        with urllib.request.urlopen(f"https://{master_url}/healthz", timeout=8, context=_https_ctx()) as r:
            mp.log(f"master /healthz: {r.read().decode('utf-8', 'replace').strip()}")
    except Exception as e:  # noqa: BLE001
        sys.exit(f"FATAL: VPS master unreachable at {master_url}: {e}")

    mp.kill_all()
    mp.deploy_all()

    host_log = mp.HOST_DIR / "multivoid.log"
    client_log = mp.CLIENT_DIR / "multivoid.log"
    lobby_id = None
    host_booted = connected = accepted = False
    code = 2
    try:
        mp.log("--- HOST (browser 'Host Game' via the test hook -> VPS) ---")
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  set_net_role=False,
                                  extra_env={"VOTVCOOP_MASTER_URL": master_url,
                                             "VOTVCOOP_TEST_HOST_LOBBY": "1"})
        mp.log(f"waiting up to {HOST_BOOT_S}s for the host to announce to the VPS + boot P2P...")
        t0 = time.time()
        while time.time() - t0 < HOST_BOOT_S:
            time.sleep(2)
            try:
                with urllib.request.urlopen(lobbies_url, timeout=8, context=_https_ctx()) as r:
                    rows = json.loads(r.read() or b"{}").get("lobbies", [])
            except Exception:  # noqa: BLE001
                rows = []
            # match OUR just-announced lobby (the VPS may carry others) by the test name
            mine = [x for x in rows if x.get("name") == "Test Host"] or rows
            if mine:
                lobby_id = mine[0]["lobbyId"]
                mp.log(f"host ANNOUNCED on the VPS: '{mine[0].get('name')}' id={lobby_id} "
                       f"({mine[0].get('players_cur')}/{mine[0].get('players_max')})")
                break
            if not any(p["PID"] == host_pid for p in mp.list_votv()):
                mp.log("HOST DIED before announcing")
                break

        if lobby_id:
            mp.log(f"--- CLIENT (browser Join of {lobby_id} via the test hook -> VPS) ---")
            client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer=None,
                                        res_x=1280, res_y=720, monitor=2, tile_index=0,
                                        set_net_role=False,
                                        extra_env={"VOTVCOOP_MASTER_URL": master_url,
                                                   "VOTVCOOP_TEST_JOIN_LOBBY": lobby_id})
            mp.log(f"--- watching {WATCH_S}s for the P2P connect (via VPS signaling) ---")
            t0 = time.time()
            while time.time() - t0 < WATCH_S:
                time.sleep(3)
                host_booted = host_booted or mp.HOST_DIR and \
                    (host_log.read_text(errors="replace").count("COOP SESSION START (host / p2p)") > 0
                     if host_log.exists() else False)
                connected = client_log.exists() and \
                    client_log.read_text(errors="replace").count("host assigned us peer slot") > 0
                accepted = host_log.exists() and \
                    host_log.read_text(errors="replace").count("host accepted client at slot") > 0
                t = int(time.time() - t0)
                mp.log(f"  t={t}s host_booted={host_booted} connected={connected} accepted={accepted}")
                if connected and accepted:
                    break
                if not any(p["PID"] == client_pid for p in mp.list_votv()):
                    mp.log("  client process died -- stopping watch")
                    break

        ok = (lobby_id is not None) and host_booted and connected and accepted
        code = 0 if ok else 2
        mp.log("--- VERDICT ---")
        mp.log(f"  host announced on the VPS (POST /v1/host) : {lobby_id is not None}")
        mp.log(f"  host booted a P2P session                 : {host_booted}")
        mp.log(f"  browser client joined + connected (P2P)   : {connected}")
        mp.log(f"  host accepted the client                  : {accepted}")
        mp.log(f"{'PASS' if ok else 'FAIL'}: the Host-Game flow against the LIVE VPS "
               + ("WORKS (announced + booted + a client connected via VPS signaling)."
                  if ok else "did NOT complete the host->VPS->join->P2P loop (see markers/logs)."))
    finally:
        mp.tail_log(host_log, 18, "HOST")
        mp.tail_log(client_log, 18, "CLIENT")
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

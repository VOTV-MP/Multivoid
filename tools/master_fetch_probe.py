"""Autonomous probe: the in-game DLL fetches the lobby list from a LOCAL master.

Verifies the Tier-1 master data plane END-TO-END in the real injected DLL (WinHTTP +
nlohmann, threads, the leaked singletons) -- not just that it compiles:

  1. start a local coop_master_server.py on 127.0.0.1:10001 + seed one lobby,
  2. deploy + boot ONE game instance with the server browser auto-opened
     (VOTVCOOP_BROWSER_OPEN=1) and VOTVCOOP_MASTER_URL pointed at the local master,
  3. the browser's open-edge auto-refresh -> lobby_client GET /v1/lobbies,
  4. confirm the DLL log shows: master URL resolved, browser opened, AND a completed
     refresh reporting "1 server" (the seeded lobby came back parsed),
  5. confirm the game process is STILL ALIVE after the fetch (the thread-safety fixes
     hold -- no terminate/crash from the lobby plane).

PASS = all four log markers + process alive. This is the Tier-1 equivalent of the LAN
smoke (Tier-1 does not boot a coop session, so there is no peer handshake to gate on).

Run: python tools/master_fetch_probe.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import mp  # reuse the launcher rig

MASTER = mp.ROOT / "tools" / "coop_master_server.py"
MLOG = mp.ROOT / "build" / "master-fetch-probe-server.log"
PORT = 10001
BASE = f"http://127.0.0.1:{PORT}"
WATCH_S = 70


def post(path: str, body: dict) -> tuple[int, dict]:
    r = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                               method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=8) as resp:
        return resp.status, json.loads(resp.read() or b"{}")


def main() -> None:
    mp.kill_all()
    mp.deploy_all()

    MLOG.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, COOP_MASTER_PORT=str(PORT),
               COOP_TURN_SECRET="probe-turn-secret", COOP_SIGNALING_TOKEN="probe-token",
               COOP_SIGNALING_URL="127.0.0.1:10000", COOP_STUN_URI="stun:127.0.0.1:3478",
               COOP_TURN_URI="turn:127.0.0.1:3478")
    mlog = open(MLOG, "w", encoding="utf-8")
    master = subprocess.Popen([sys.executable, str(MASTER)], env=env, stdout=mlog,
                              stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    if master.poll() is not None:
        mlog.close()
        print("FATAL: master exited immediately"); print(MLOG.read_text(errors="replace"))
        sys.exit(1)

    try:
        s, h = post("/v1/host", {"name": "Probe Lobby", "version": "0.9.0-n",
                                 "world": "Site-23 (story)", "players_max": 4})
        print(f"[probe] seeded lobby: status={s} lobbyId={h.get('lobbyId')}")
    except Exception as e:  # noqa: BLE001
        print(f"[probe] seed FAILED: {e}")

    host_log = mp.HOST_DIR / "multivoid.log"
    code = 2
    try:
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  extra_env={"VOTVCOOP_MASTER_URL": f"127.0.0.1:{PORT}",
                                             "VOTVCOOP_BROWSER_OPEN": "1"})
        print(f"[probe] watching the DLL log up to {WATCH_S}s for the fetch ...")
        seen_master = seen_open = seen_refresh = False
        refresh_line = ""
        alive = True
        t0 = time.time()
        while time.time() - t0 < WATCH_S:
            time.sleep(3)
            alive = any(p["PID"] == host_pid for p in mp.list_votv())
            txt = host_log.read_text(errors="replace") if host_log.exists() else ""
            seen_master = "session_manager: master server = 127.0.0.1" in txt
            seen_open = "server browser starts visible" in txt
            for ln in txt.splitlines():
                if "lobby: refresh done" in ln:
                    seen_refresh = True; refresh_line = ln.strip()
            t = int(time.time() - t0)
            print(f"[probe] t={t}s alive={alive} master={seen_master} open={seen_open} refresh={seen_refresh}")
            if not alive:
                print("[probe] game process died -- stopping watch")
                break
            if seen_master and seen_refresh:
                break
        ok = seen_master and seen_open and seen_refresh and alive and ("1 server" in refresh_line)
        code = 0 if ok else 2
        print("\n[probe] --- VERDICT ---")
        print(f"[probe]   master URL resolved : {seen_master}")
        print(f"[probe]   browser auto-opened : {seen_open}")
        print(f"[probe]   refresh completed   : {seen_refresh}  ({refresh_line})")
        print(f"[probe]   process still alive  : {alive}")
        print(f"[probe] {'PASS' if ok else 'FAIL'}: the in-game DLL "
              f"{'fetched the lobby list from the local master' if ok else 'did NOT complete the fetch (see markers)'}.")
    finally:
        mp.tail_log(host_log, 25, "HOST")
        master.terminate()
        try:
            master.wait(timeout=5)
        except subprocess.TimeoutExpired:
            master.kill()
        mlog.close()
        print("[probe] --- MASTER LOG ---")
        sys.stdout.write(MLOG.read_text(errors="replace"))
        mp.kill_all()
    sys.exit(code)


if __name__ == "__main__":
    main()

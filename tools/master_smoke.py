#!/usr/bin/env python3
"""Local smoke for tools/coop_master_server.py -- the zero-ports master/lobby server.

Starts the master on loopback with throwaway secrets, then drives the full lobby
lifecycle over HTTP and asserts the design's security properties (no VPS, no game
build). Run: python tools/master_smoke.py  (exit 0 = PASS).

Checks:
  - POST /v1/host returns a session + opaque lobbyId + host identity (<=31 chars)
    + a VALID coturn REST TURN cred (recompute the HMAC-SHA1, verify the password
    + the expiry window).
  - GET /v1/lobbies lists it AND leaks neither sessionId nor token in the row.
  - version filter hides a non-matching lobby.
  - POST /v1/join {lobbyId} mints a fresh peer identity + the host identity to
    dial + a fresh valid TURN cred; an unknown lobbyId 404s.
  - POST /v1/heartbeat updates players_cur + refreshes the TURN cred; a BAD token
    is rejected (403).
  - POST /v1/visibility hides the lobby from the public list but keeps it joinable
    (hidden != gone); relisting restores it.
  - POST /v1/leave removes it; the list empties + join 404s.
  - host strings are clamped + control chars stripped (10 KB name DoS defense).
  - creates are bounded (per-IP cap / rate -> 429).
  - malformed input is rejected (bad json 400, oversized body 413, unknown 404).
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MASTER = ROOT / "tools" / "coop_master_server.py"
LOG = ROOT / "build" / "master-smoke-server.log"

PORT = 18099
BASE = f"http://127.0.0.1:{PORT}"
TURN_SECRET = secrets.token_hex(16)
SIGNALING_TOKEN = secrets.token_hex(16)
SIGNALING_URL = "203.0.113.5:10000"
STUN_URI = "stun:203.0.113.5:3478"
TURN_URI = "turn:203.0.113.5:3478"

_fail = 0


def check(cond: bool, label: str) -> None:
    global _fail
    if cond:
        print(f"  PASS  {label}")
    else:
        _fail += 1
        print(f"  FAIL  {label}")


def req(method: str, path: str, body: dict | None = None, raw: bytes | None = None,
        headers: dict | None = None) -> tuple[int, dict]:
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers=headers or {})
    try:
        with urllib.request.urlopen(r, timeout=8) as resp:
            return resp.status, json.loads(resp.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except (ValueError, OSError):
            return e.code, {}
    except urllib.error.URLError as e:
        return 0, {"error": str(e)}


def turn_valid(turn: dict) -> bool:
    """Recompute the coturn REST cred and verify it matches + is fresh (design 7)."""
    if not turn or "user" not in turn or "pass" not in turn:
        return False
    user = turn["user"]
    mac = hmac.new(TURN_SECRET.encode(), user.encode(), hashlib.sha1).digest()
    expect = base64.b64encode(mac).decode()
    if not hmac.compare_digest(expect, turn["pass"]):
        return False
    try:
        exp = int(user.split(":", 1)[0])
    except (ValueError, IndexError):
        return False
    now = int(time.time())
    return now < exp <= now + turn.get("ttl", 0) + 5


def wait_up(proc: subprocess.Popen, tries: int = 40) -> bool:
    for _ in range(tries):
        if proc.poll() is not None:
            return False
        try:
            s, _b = req("GET", "/healthz")
            if s == 200:
                return True
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.25)
    return False


def main() -> int:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, COOP_MASTER_PORT=str(PORT), COOP_TURN_SECRET=TURN_SECRET,
               COOP_SIGNALING_TOKEN=SIGNALING_TOKEN, COOP_SIGNALING_URL=SIGNALING_URL,
               COOP_STUN_URI=STUN_URI, COOP_TURN_URI=TURN_URI)
    logf = open(LOG, "w", encoding="utf-8")
    proc = subprocess.Popen([sys.executable, str(MASTER)], env=env,
                            stdout=logf, stderr=subprocess.STDOUT, text=True)
    try:
        if not wait_up(proc):
            print("FATAL: master did not come up")
            logf.flush()
            print(LOG.read_text(errors="replace"))
            return 1
        print(f"master up on :{PORT}")

        # --- /v1/latest (v59 launch toast) ---
        s, latest = req("GET", "/v1/latest")
        check(s == 200, "/v1/latest 200")
        check(isinstance(latest.get("proto"), int) and latest["proto"] > 0,
              "latest carries a positive integer proto")
        check(isinstance(latest.get("mod"), str) and latest["mod"] != "",
              "latest carries a mod tag")
        check(isinstance(latest.get("url"), str) and latest["url"].startswith("https://"),
              "latest carries an https release url")

        # --- /v1/host ---
        s, host = req("POST", "/v1/host",
                      {"name": "Dr. Bao's Observatory", "version": "0.9.0-n",
                       "proto": 59,
                       "world": "Site-23 (story)", "locked": False, "players_max": 4})
        check(s == 200, "/v1/host 200")
        check(all(k in host for k in ("sessionId", "lobbyId", "hostIdentity", "token")),
              "host response has session/lobby/identity/token")
        check(len(host.get("hostIdentity", "x" * 99)) <= 31,
              "hostIdentity <= 31 chars (GNS SetGenericString)")
        check(host.get("signalingToken") == SIGNALING_TOKEN,
              "host response carries the signaling bearer token")
        check(turn_valid(host.get("turn", {})), "host TURN cred HMAC valid + fresh")
        check(host["turn"]["user"].split(":", 1)[1] == host["hostIdentity"],
              "host TURN username bound to hostIdentity (per-peer coturn quota)")
        check(any("transport=udp" in u for u in host["turn"].get("uris", []))
              and any("transport=tcp" in u for u in host["turn"].get("uris", [])),
              "host TURN uris carry udp + tcp transports")
        sid, lobby_id, token = host["sessionId"], host["lobbyId"], host["token"]

        # --- /v1/lobbies lists it, leaks no secrets ---
        s, lst = req("GET", "/v1/lobbies")
        rows = lst.get("lobbies", [])
        row = next((r for r in rows if r.get("lobbyId") == lobby_id), None)
        check(row is not None, "lobby appears in /v1/lobbies")
        check(row is not None and row.get("name") == "Dr. Bao's Observatory",
              "row name matches")
        check(row is not None and isinstance(row.get("age"), int) and row["age"] >= 0,
              "row carries an integer age field (relative, no clock sync)")
        check(row is not None and row.get("proto") == 59,
              "row carries the announced proto (v59 join gate)")
        check(row is not None and "sessionId" not in row and "token" not in row,
              "public row leaks NO sessionId/token (opaque lobbyId only)")

        # version filter
        s, lst2 = req("GET", "/v1/lobbies?version=9.9.9-x")
        check(not any(r.get("lobbyId") == lobby_id for r in lst2.get("lobbies", [])),
              "version filter hides a non-matching lobby")

        # --- /v1/join ---
        s, join = req("POST", "/v1/join", {"lobbyId": lobby_id})
        check(s == 200, "/v1/join 200")
        check(join.get("hostIdentity") == host["hostIdentity"],
              "join returns the host identity to dial")
        check(join.get("sessionId") == sid, "join returns the session id")
        check(isinstance(join.get("peerIdentity"), str)
              and join["peerIdentity"] != host["hostIdentity"]
              and len(join["peerIdentity"]) <= 31,
              "join mints a fresh peer identity (<=31 chars)")
        check(turn_valid(join.get("turn", {})), "join TURN cred HMAC valid + fresh")
        check(join["turn"]["user"].split(":", 1)[1] == join["peerIdentity"],
              "join TURN username bound to peerIdentity (per-peer coturn quota)")
        s, _ = req("POST", "/v1/join", {"lobbyId": "deadbeefdeadbeef"})
        check(s == 404, "join unknown lobbyId -> 404")

        # --- /v1/heartbeat ---
        s, hb = req("POST", "/v1/heartbeat",
                    {"sessionId": sid, "token": token, "players_cur": 2})
        check(s == 200 and turn_valid(hb.get("turn", {})),
              "heartbeat 200 + refreshed TURN cred")
        time.sleep(5.2)  # let the /v1/lobbies cache expire so players_cur updates
        s, lst = req("GET", "/v1/lobbies")
        row = next((r for r in lst.get("lobbies", []) if r.get("lobbyId") == lobby_id), None)
        check(row is not None and row.get("players_cur") == 2,
              "heartbeat updated players_cur in the list")
        s, _ = req("POST", "/v1/heartbeat",
                   {"sessionId": sid, "token": "wrong-token", "players_cur": 3})
        check(s == 403, "heartbeat with bad token -> 403")

        # --- /v1/visibility (hide != gone) ---
        s, _ = req("POST", "/v1/visibility",
                   {"lobbyId": lobby_id, "token": token, "listed": False})
        check(s == 200, "/v1/visibility hide 200")
        time.sleep(5.2)
        s, lst = req("GET", "/v1/lobbies")
        check(not any(r.get("lobbyId") == lobby_id for r in lst.get("lobbies", [])),
              "hidden lobby drops from the public list")
        s, _ = req("POST", "/v1/join", {"lobbyId": lobby_id})
        check(s == 200, "hidden lobby is still joinable (hidden != gone)")
        s, _ = req("POST", "/v1/visibility",
                   {"lobbyId": lobby_id, "token": token, "listed": True})
        time.sleep(5.2)
        s, lst = req("GET", "/v1/lobbies")
        check(any(r.get("lobbyId") == lobby_id for r in lst.get("lobbies", [])),
              "relisting restores the lobby")

        # --- string clamp ---
        s, h2 = req("POST", "/v1/host",
                    {"name": "A" * 10000 + "\n\t\x07bad", "version": "0.9.0-n",
                     "world": "W" * 500})
        s2, lst = req("GET", "/v1/lobbies")
        # wait out the cache to see the new row
        time.sleep(5.2)
        s2, lst = req("GET", "/v1/lobbies")
        r2 = next((r for r in lst.get("lobbies", []) if r.get("lobbyId") == h2.get("lobbyId")), None)
        check(r2 is not None and len(r2["name"]) <= 63 and "\n" not in r2["name"]
              and "\x07" not in r2["name"],
              "host name clamped to <=63 + control chars stripped")
        check(r2 is not None and len(r2["world"]) <= 39, "host world clamped to <=39")
        req("POST", "/v1/leave", {"sessionId": h2["sessionId"], "token": h2["token"]})

        # --- DIRECT lobbies (2026-06-11) ---
        s, dh = req("POST", "/v1/host",
                    {"name": "Forwarded Den", "version": "0.9.0-n", "world": "W",
                     "conn": "direct", "direct_port": 47621})
        check(s == 200, "direct /v1/host 200")
        check(dh.get("conn") == "direct", "direct host response reflects conn=direct (audit R1)")
        check("turn" not in dh and "signalingUrl" not in dh,
              "direct host gets NO ICE/TURN creds")
        time.sleep(5.2)
        s, lst = req("GET", "/v1/lobbies")
        drow = next((r for r in lst.get("lobbies", []) if r.get("lobbyId") == dh["lobbyId"]), None)
        check(drow is not None and drow.get("conn") == "direct",
              "direct lobby lists with conn=direct")
        check(drow is not None and "direct_port" not in drow,
              "public row does NOT leak direct_port")
        s, dj = req("POST", "/v1/join", {"lobbyId": dh["lobbyId"]})
        check(s == 200 and dj.get("conn") == "direct", "direct join -> conn=direct")
        check(dj.get("addr", "").endswith(":47621") and "sessionId" not in dj
              and "turn" not in dj, "direct join returns addr ip:port, no ICE/TURN")
        req("POST", "/v1/leave", {"sessionId": dh["sessionId"], "token": dh["token"]})
        # out-of-range direct_port is REJECTED, not silently downgraded (audit R1)
        s, _ = req("POST", "/v1/host",
                   {"name": "Bad Port", "version": "0.9.0-n", "world": "W",
                    "conn": "direct", "direct_port": 80})
        check(s == 400, "direct_port out of range -> 400 (no silent p2p downgrade)")

        # --- /v1/leave ---
        s, _ = req("POST", "/v1/leave", {"sessionId": sid, "token": token})
        check(s == 200, "/v1/leave 200")
        time.sleep(5.2)
        s, lst = req("GET", "/v1/lobbies")
        check(not any(r.get("lobbyId") == lobby_id for r in lst.get("lobbies", [])),
              "left lobby is gone from the list")
        s, _ = req("POST", "/v1/join", {"lobbyId": lobby_id})
        check(s == 404, "join a left lobby -> 404")

        # --- malformed / bounds ---
        s, _ = req("POST", "/v1/host", raw=b"{not json",
                   headers={"Content-Type": "application/json"})
        check(s == 400, "bad json -> 400")
        s, _ = req("POST", "/v1/host", raw=b"x" * (65 * 1024),
                   headers={"Content-Type": "application/json"})
        check(s == 413, "oversized body -> 413")
        s, _ = req("GET", "/v1/nonexistent")
        check(s == 404, "unknown route -> 404")

        # --- creates are bounded (per-IP cap / rate) ---
        saw_429 = False
        for _ in range(16):
            s, _ = req("POST", "/v1/host", {"name": "flood", "version": "0.9.0-n"})
            if s == 429:
                saw_429 = True
                break
        check(saw_429, "rapid creates eventually 429 (per-IP cap/rate)")

        print(f"\n{'PASS' if _fail == 0 else 'FAIL'}: {_fail} failed check(s)")
        return 0 if _fail == 0 else 2
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        logf.close()
        print("--- master server log ---")
        sys.stdout.write(LOG.read_text(errors="replace"))


if __name__ == "__main__":
    sys.exit(main())

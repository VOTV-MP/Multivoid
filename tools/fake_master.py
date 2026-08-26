#!/usr/bin/env python3
"""fake_master.py -- the row FIXTURE for the native server browser: a local master
serving a synthetic lobby list.

WHAT THIS IS. The seeder that docs/MULTIPLAYER_UI.md section 8c calls for -- *"the
harness must seed synthetically and BYPASS the master"* (8c.3) -- and the row
source for step T0. An earlier draft of 8c.-1 rejected "a local test master" as
DUPLICATING the harness seeder; that was wrong and is retracted in the doc. They
are the same artifact: the harness needs a synthetic row source, and this is it.

WHY NOT tools/coop_master_server.py, which already serves /v1/lobbies and is
already wired to the game by master_fetch_probe.py:77. That one is a master
EMULATOR and carries the production DoS semantics on purpose -- MAX_LOBBIES_PER_IP
= 8 (:103) and RL_CREATE 10 per 60 s (:108) -- while every seed here arrives from
127.0.0.1. Eight rows against a viewport of roughly seven reproduces the very
problem this exists to escape, and relaxing those caps would degrade the fidelity
that is the emulator's whole point. A FIXTURE (return N rows, mutate instantly,
deterministic per seed) and an EMULATOR (lobby lifecycle, heartbeats, TTL, rate
limits) are different concepts.

WHY IT IS NEEDED AT ALL. The browser has never rendered more than the live
master's ~2 lobbies, so every cost number in section 8c.-1 is arithmetic over
measured unit costs rather than an end-to-end timing -- and T0 ("does the wheel
scroll this widget at all") is not merely unmeasured but UNRUNNABLE, because at
2 rows the list does not overflow the viewport and there is no scrollbar.
(The viewport is ~7.6 rows by arithmetic over assumed title/header/status
heights; it has NOT been measured. 20 rows clears any plausible value.)

`VOTVCOOP_MASTER_URL` beats every other config layer (config.cpp:481), so
pointing the game at `http://127.0.0.1:<port>` is the whole integration.

NOTE T0 STILL NEEDS A POSITIVE CONTROL that this file cannot provide: an
unchanged screenshot is ambiguous between "the wheel never arrived", "the
ScrollBox does not scroll" and "the capture beat Slate's layout". Drive
SetScrollOffset from a dev hook first and prove the capture CAN show a scrolled
list, or a green run means nothing -- the ESC selftest reported ALL PASS on a
total failure for exactly this reason.

SCHEME MATTERS. The master URL grammar is SCHEMELESS = SECURE
(http_client.cpp:50-72): a bare `host:port` means TLS. A plaintext local server
MUST be addressed as `http://127.0.0.1:<port>` -- and the port is mandatory,
because the client refuses a URL without one.

This is a PROBE/diagnostic tool. It ships with nothing and is exempt from RULE 2's
retirement accounting (feedback_rule2_exempts_probes_diagnostics_tools).

Contract, from tools/coop-server-rs/src/bin/master.rs:531-553 and 761:
    GET /v1/lobbies[?version=X]  ->  200 {"lobbies": [ {row}, ... ]}
    row = lobbyId name version game proto world locked players_cur players_max age conn

CONTROL CHANNEL -- this is what makes it a harness fixture rather than a static
page. Section 8c.2's phases C (grow), D (shrink) and E (shuffle at CONSTANT
count) each need the row set to CHANGE while the game is looking at it, and E is
the phase that would have caught the HashMap-order defect. The first revision of
this file claimed "mutate instantly" in its docstring while assigning the row
list exactly once, which is a documentation claim the code flatly contradicted.

    GET /control/count?n=200   grow (append; existing rows untouched) or shrink
                               (keep a prefix, so surviving ids are stable)
    GET /control/shuffle       reorder, SET HELD CONSTANT -- phase E
    GET /control/state         count, order fingerprint, request + mutation counts

The fingerprint is an ORDER-SENSITIVE digest of the lobbyId sequence: a count or
a set hash cannot see a reorder, which is precisely the defect phase E exists to
catch.

Usage:
    python tools/fake_master.py --port 18080 --count 20
    python tools/fake_master.py --port 18080 --count 100 --mismatch-every 7
    curl "http://127.0.0.1:18080/control/count?n=200"   # phase C
    curl  http://127.0.0.1:18080/control/shuffle        # phase E
"""

from __future__ import annotations

import argparse
import json
import random
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

# The build identity is PARSED from the same sources CMakeLists.txt:23/29 uses --
# NOT hand-copied. A hand-copied proto that drifts one release is a SILENT
# failure of exactly the wrong kind: every row renders amber "(!)", the perf
# numbers stay clean because the cost is identical, and the tester's feel verdict
# is quietly delivered about the wrong screen. So this parses, and it EXITS if it
# cannot -- a fixture that guesses its own contract is worse than no fixture.
_REPO = Path(__file__).resolve().parent.parent
_PROTOCOL_H = _REPO / "src" / "votv-coop" / "include" / "coop" / "net" / "protocol.h"
_CMAKELISTS = _REPO / "src" / "votv-coop" / "CMakeLists.txt"


def _parse_or_die(path: Path, pattern: str, what: str) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise SystemExit(f"fake_master: cannot read {path} to learn {what}: {e}")
    m = re.search(pattern, text)
    if not m:
        raise SystemExit(f"fake_master: cannot find {what} in {path} -- the pattern moved. "
                         f"Fix this rather than hard-coding, or every row renders amber and the "
                         f"run measures the wrong screen.")
    return m.group(1)


GAME_TARGET = _parse_or_die(_CMAKELISTS, r'set\(VOTVCOOP_GAME_TARGET\s+"([^"]+)"\)', "the game target")
PROTO = int(_parse_or_die(_PROTOCOL_H,
                          r"kProtocolVersion\s*=\s*(\d+)", "kProtocolVersion"))

WORDS = [
    "Kerfur", "Signal", "Meadow", "Static", "Dish", "Base", "Void", "Tape",
    "Argus", "Ridge", "Hollow", "Quiet", "Relay", "Drift", "Ember", "Nomai",
]

_state_lock = threading.Lock()
_rows: list[dict] = []
_hits = 0
_mutations = 0
_cfg = {"mismatch_every": 0, "seed": 1234}


def make_rows(count: int, mismatch_every: int, seed: int) -> list[dict]:
    """Deterministic for a given seed, so two runs are comparable."""
    rng = random.Random(seed)
    rows = []
    for i in range(count):
        # A long name every few rows exercises the clipping + gutter fix
        # (lesson_umg_slot_bounds_layout_not_painting) at the 64-char master cap.
        long_name = (i % 9 == 4)
        name = f"{rng.choice(WORDS)} {rng.choice(WORDS)}"
        if long_name:
            name = (name + " " + " ".join(rng.choice(WORDS) for _ in range(6)))[:64]
        bad = mismatch_every > 0 and (i % mismatch_every == 0)
        rows.append({
            "lobbyId": f"fake-{i:04d}",
            "name": name,
            "version": "",                       # legacy-only fallback field
            "game": "0.9.0m" if bad else GAME_TARGET,
            "proto": (PROTO - 1) if bad else PROTO,
            "world": rng.choice(["s_1234", "meadow", "base", "s_pooandsarand2altra"]),
            "locked": (i % 5 == 0),
            "players_cur": rng.randint(0, 4),
            "players_max": 4,
            "age": rng.randint(0, 40),
            "conn": "p2p",
        })
    return rows


def _fingerprint(rows: list[dict]) -> str:
    """Order-SENSITIVE digest of the lobbyId sequence. Phase E mutates order while
    holding the set constant, so a test needs a value that changes on reorder --
    a count or a set hash cannot see the defect E exists to catch."""
    import hashlib
    h = hashlib.sha1("|".join(r["lobbyId"] for r in rows).encode("utf-8"))
    return h.hexdigest()[:12]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code: int, payload: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _json(self, code: int, obj) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"))

    def do_GET(self) -> None:  # noqa: N802 (stdlib naming)
        global _hits, _rows, _mutations
        raw = self.path
        path, _, query = raw.partition("?")
        args = {}
        for part in query.split("&"):
            if "=" in part:
                k, v = part.split("=", 1)
                args[k] = v

        if path == "/v1/lobbies":
            with _state_lock:
                _hits += 1
                body = json.dumps({"lobbies": _rows}).encode("utf-8")
                n, hits = len(_rows), _hits
            self._send(200, body)
            print(f"  [fake_master] GET /v1/lobbies -> {n} rows, {len(body)} B (request #{hits})",
                  flush=True)
            return

        # ---- control channel: the harness phases (8c.2 C / D / E) -------------
        # A fixture whose row set never changes cannot drive grow, shrink or
        # shuffle -- and shuffle is the phase that would have caught the
        # HashMap-order defect. An earlier revision of this file claimed
        # "mutate instantly" in its docstring while assigning _rows exactly once.
        if path == "/control/count":
            try:
                n = int(args.get("n", ""))
            except ValueError:
                self._json(400, {"error": "n must be an integer"}); return
            if n < 0 or n > 5000:
                self._json(400, {"error": "n out of range 0..5000"}); return
            with _state_lock:
                cur = len(_rows)
                if n <= cur:
                    _rows = _rows[:n]            # SHRINK keeps a prefix, so ids are stable
                else:
                    extra = make_rows(n, _cfg["mismatch_every"], _cfg["seed"] + 1)[cur:n]
                    _rows = _rows + extra        # GROW appends; existing rows untouched
                _mutations += 1
                out = {"count": len(_rows), "fingerprint": _fingerprint(_rows)}
            print(f"  [fake_master] control: count {cur} -> {out['count']}  fp={out['fingerprint']}",
                  flush=True)
            self._json(200, out); return

        if path == "/control/shuffle":
            with _state_lock:
                before = _fingerprint(_rows)
                rng = random.Random(_cfg["seed"] + _mutations + 1)
                rng.shuffle(_rows)
                _mutations += 1
                after = _fingerprint(_rows)
                out = {"count": len(_rows), "before": before, "after": after,
                       "changed": before != after}
            print(f"  [fake_master] control: SHUFFLE (set constant) {before} -> {after}", flush=True)
            self._json(200, out); return

        if path == "/control/state":
            with _state_lock:
                self._json(200, {"count": len(_rows), "fingerprint": _fingerprint(_rows),
                                 "requests": _hits, "mutations": _mutations,
                                 "game": GAME_TARGET, "proto": PROTO})
            return

        if path == "/healthz":
            self._send(200, b'{"ok":true}')
            return
        # /v1/latest: a pre-v59 master 404s and the client treats that as silent
        # (lobby_client.cpp FetchLatest), which is exactly what we want -- no
        # update toast during a measurement run.
        self._send(404, b'{"error":"not found"}')

    def log_message(self, fmt: str, *args) -> None:  # silence the default logger
        pass


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--count", type=int, default=20,
                    help="rows to serve (20 = over the ~7.6-row viewport, under kMaxRows=64)")
    ap.add_argument("--mismatch-every", type=int, default=0,
                    help="every Nth row gets a wrong game/proto so the amber tint is exercised (0=off)")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="exit after N seconds (0 = run until killed)")
    args = ap.parse_args()

    global _rows
    _cfg["mismatch_every"] = args.mismatch_every
    _cfg["seed"] = args.seed
    _rows = make_rows(args.count, args.mismatch_every, args.seed)

    srv = HTTPServer(("127.0.0.1", args.port), Handler)
    print(f"[fake_master] serving {args.count} lobbies on http://127.0.0.1:{args.port}/v1/lobbies")
    print(f"[fake_master] point the game at it with "
          f"VOTVCOOP_MASTER_URL=http://127.0.0.1:{args.port}   (the http:// is REQUIRED -- "
          f"schemeless means TLS)")
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        if args.seconds > 0:
            time.sleep(args.seconds)
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        srv.shutdown()
        with _state_lock:
            print(f"[fake_master] done -- {_hits} lobby requests served")


if __name__ == "__main__":
    main()

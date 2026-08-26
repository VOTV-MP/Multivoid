#!/usr/bin/env python3
"""fake_master.py -- a throwaway local master serving a synthetic lobby list.

WHY THIS EXISTS. The native server browser has never rendered more than the live
master's ~2 lobbies, so every cost number in docs/MULTIPLAYER_UI.md section 8c.-1
is arithmetic over measured unit costs rather than an end-to-end timing, and its
step T0 ("does the wheel scroll this widget at all") is not even runnable: at 2
rows the list does not overflow the 7.6-row viewport, so there is no scrollbar
and nothing to scroll.

This serves N rows so those questions become answerable WITHOUT touching the mod.
`VOTVCOOP_MASTER_URL` beats every other config layer (config.cpp:481), so
pointing the game at `http://127.0.0.1:<port>` is the whole integration.

SCHEME MATTERS. The master URL grammar is SCHEMELESS = SECURE
(http_client.cpp:50-72): a bare `host:port` means TLS. A plaintext local server
MUST be addressed as `http://127.0.0.1:<port>` -- and the port is mandatory,
because the client refuses a URL without one.

This is a PROBE/diagnostic tool. It ships with nothing and is exempt from RULE 2's
retirement accounting (feedback_rule2_exempts_probes_diagnostics_tools).

Contract, from tools/coop-server-rs/src/bin/master.rs:531-553 and 761:
    GET /v1/lobbies[?version=X]  ->  200 {"lobbies": [ {row}, ... ]}
    row = lobbyId name version game proto world locked players_cur players_max age conn

Usage:
    python tools/fake_master.py --port 18080 --count 20
    python tools/fake_master.py --port 18080 --count 100 --mismatch-every 7
"""

from __future__ import annotations

import argparse
import json
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

# Must match the build under test or every row renders amber "(!)" and the
# version-tint leg of the browser is untested. src/votv-coop/CMakeLists.txt:23
# and include/coop/net/protocol.h:710.
GAME_TARGET = "0.9.0n"
PROTO = 143

WORDS = [
    "Kerfur", "Signal", "Meadow", "Static", "Dish", "Base", "Void", "Tape",
    "Argus", "Ridge", "Hollow", "Quiet", "Relay", "Drift", "Ember", "Nomai",
]

_state_lock = threading.Lock()
_rows: list[dict] = []
_hits = 0


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


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code: int, payload: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:  # noqa: N802 (stdlib naming)
        global _hits
        path = self.path.split("?", 1)[0]
        if path == "/v1/lobbies":
            with _state_lock:
                _hits += 1
                body = json.dumps({"lobbies": _rows}).encode("utf-8")
                n, hits = len(_rows), _hits
            self._send(200, body)
            print(f"  [fake_master] GET /v1/lobbies -> {n} rows, {len(body)} B (request #{hits})",
                  flush=True)
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

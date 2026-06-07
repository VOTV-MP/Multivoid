#!/usr/bin/env python3
"""Production async P2P signaling server for VOTV coop.

Replaces Valve's thread-per-connection "trivial" example (RULE 2 -- the trivial
server is no longer deployed). Hardened for a public, shared box:

  - asyncio: a single event loop, NOT one OS thread per connection (the trivial
    example's C10k/slowloris DoS on a 1-CPU box is gone).
  - SHARED BEARER-TOKEN auth on the greeting: the open internet can no longer
    register identities or relay blobs -- only holders of the token (the lobby
    members) get past the greeting. This closes the open-registration /
    identity-squatting / free-relay abuse of the trivial server.
  - greeting read timeout (15s): a peer that connects and dribbles/stalls before
    authenticating is dropped -- slowloris can't pin a connection.
  - max line length (64 KiB): an unbounded line can't exhaust memory (mirrors the
    client's inBuf_ cap).
  - per-source-IP + global connection caps: a flood is bounded.
  - TCP keepalive: dead authed peers are reaped without an idle timeout (a valid
    peer keeps its signaling connection open, mostly idle, for the whole session
    -- so there is intentionally NO post-auth idle timeout).
  - evict-on-duplicate-identity: a reconnecting peer (same identity, its old
    socket dead) cleanly replaces the stale entry. This is token-gated, so it is
    NOT an open-internet squatting vector; within a trusted lobby an insider
    using another member's identity is the residual, which the master server's
    unguessable per-session identities close later (Stage 6).

Wire protocol (line-oriented, '\n'-terminated, identities are space-free):
  greeting (first line) : <token> <identity>
  message  (subsequent) : <dest-identity> <hexpayload>
  forwarded to dest as  : <sender-identity> <hexpayload>

Config via env:
  COOP_SIGNALING_PORT   (default 10000)
  COOP_SIGNALING_TOKEN  (REQUIRED; empty => refuse to start -- never run open)
"""

from __future__ import annotations

import asyncio
import hmac
import os
import socket
import sys

PORT = int(os.environ.get("COOP_SIGNALING_PORT", "10000"))
TOKEN = os.environ.get("COOP_SIGNALING_TOKEN", "")

MAX_LINE = 64 * 1024          # bytes; a single line longer than this drops the conn
GREETING_TIMEOUT = 15.0       # seconds to send the (authenticating) greeting
# Auth-AWARE caps: a bounded PRE-AUTH pool separate from the authed budget, so an
# anonymous flood (no token) can only fill the small pending pool -- where each
# conn is dropped after GREETING_TIMEOUT -- and can NEVER exhaust the budget
# reserved for token-holders. (Audit F1.) Sized so worst-case transient read
# buffering ((MAX_AUTHED+MAX_PENDING) * ~128 KiB) stays under the 128 MiB cgroup.
MAX_PENDING = 128             # accepted-but-not-yet-authed connections (global)
MAX_AUTHED = 512              # authed (token-holder) connections (global)
MAX_AUTHED_PER_IP = 32        # authed connections from one source IP (NAT'd lobbies)

clients: dict[str, asyncio.StreamWriter] = {}   # identity -> writer (authed only)
authed_per_ip: dict[str, int] = {}
pending = 0                   # pre-auth connections in flight
authed = 0                    # authed connections


def log(msg: str) -> None:
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    global pending, authed
    peer = writer.get_extra_info("peername")
    ip = peer[0] if peer else "?"
    identity: str | None = None
    is_authed = False

    # Admission into the bounded PRE-AUTH pool. An anonymous flood can fill only
    # this small pool (each conn is dropped after GREETING_TIMEOUT) and can never
    # exhaust the authed budget -> a token-holder is never refused at the door.
    if pending >= MAX_PENDING:
        log(f"[{ip}] refused: pre-auth pool full")
        writer.close()
        return
    pending += 1

    try:
        # TCP keepalive so a dead authed peer is reaped without an app idle timeout.
        try:
            sock = writer.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        except OSError:
            pass

        # --- greeting: "<token> <identity>", short timeout (anti-slowloris) ---
        try:
            line = await asyncio.wait_for(reader.readuntil(b"\n"), GREETING_TIMEOUT)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            log(f"[{ip}] no/oversized greeting -- dropped")
            return
        parts = line.decode("utf-8", "ignore").strip().split(" ")
        if len(parts) != 2:
            log(f"[{ip}] malformed greeting -- dropped")
            return
        token, ident = parts[0], parts[1]
        if not hmac.compare_digest(token, TOKEN):
            log(f"[{ip}] bad token -- dropped")
            return
        if not ident or " " in ident:
            log(f"[{ip}] empty/spaced identity -- dropped")
            return

        # Auth OK -> enforce the authed-pool caps (only token-holders count here),
        # then promote this connection out of the pre-auth pool.
        if authed >= MAX_AUTHED or authed_per_ip.get(ip, 0) >= MAX_AUTHED_PER_IP:
            log(f"[{ident}@{ip}] refused: authed cap")
            return
        pending -= 1
        is_authed = True
        identity = ident
        authed += 1
        authed_per_ip[ip] = authed_per_ip.get(ip, 0) + 1

        # Evict a stale entry under this identity (token-gated reconnect). Close
        # the previous writer if it isn't us.
        prev = clients.get(identity)
        if prev is not None and prev is not writer:
            try:
                prev.close()
            except OSError:
                pass
            log(f"[{identity}@{ip}] replaced previous connection")
        clients[identity] = writer
        log(f"[{identity}@{ip}] registered")

        # --- message loop: no idle timeout (authed peers stay connected) ---
        while True:
            try:
                line = await reader.readuntil(b"\n")
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionResetError):
                break
            if not line:
                break
            text = line.decode("utf-8", "ignore")
            sp = text.find(" ")
            if sp <= 0:
                continue
            dest = text[:sp].strip()
            payload = text[sp + 1:]          # keeps the trailing '\n'
            if not dest:
                continue
            dest_w = clients.get(dest)
            if dest_w is None:
                continue                      # destination not present -> drop (best-effort)
            try:
                dest_w.write((identity + " " + payload).encode("utf-8"))
                # Bound drain so a slow/stalled destination can't head-of-line block
                # this sender's handler. Best-effort: drop on timeout. (Audit F2.)
                await asyncio.wait_for(dest_w.drain(), timeout=5.0)
            except (OSError, ConnectionResetError, asyncio.TimeoutError):
                pass
    except Exception as e:  # noqa: BLE001 -- never let one connection kill the loop
        log(f"[{ip}] handler error: {e}")
    finally:
        if is_authed:
            authed -= 1
            authed_per_ip[ip] = max(0, authed_per_ip.get(ip, 0) - 1)
            if authed_per_ip.get(ip) == 0:
                authed_per_ip.pop(ip, None)
            if identity is not None and clients.get(identity) is writer:
                del clients[identity]
                log(f"[{identity}@{ip}] disconnected")
        else:
            pending -= 1
        try:
            writer.close()
        except OSError:
            pass


async def main() -> None:
    if not TOKEN:
        log("FATAL: COOP_SIGNALING_TOKEN not set -- refusing to start an open server")
        sys.exit(1)
    # limit= bounds the per-connection read buffer (a too-long line raises
    # LimitOverrunError, which we treat as a dropped connection). Bind IPv4 all
    # interfaces (clients reach the public IPv4).
    server = await asyncio.start_server(handle, host="0.0.0.0", port=PORT, limit=MAX_LINE)
    log(f"signaling listening on 0.0.0.0:{PORT} (token auth required)")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

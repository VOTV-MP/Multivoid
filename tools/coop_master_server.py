#!/usr/bin/env python3
"""Production async HTTP master / lobby server for VOTV coop (zero-open-ports MP).

Same shape as MTA's master (announce + fetch + last-seen expiry + on-disk client
cache + redundant masters): reference/mtasa-blue/Server/mods/deathmatch/utils/
CMasterServerAnnouncer.h (host announce+heartbeat with retry) +
reference/mtasa-blue/Client/core/ServerBrowser/CServerList.h (the browser data
model). Implements the design at research/findings/
votv-zero-ports-connectivity-ladder-design-2026-06-05.md sections 8 (endpoints),
10 (token/auth/abuse), 7 (coturn REST creds). RULE 3: VPS infra, never ships.

This is the piece the ladder deferred the ephemeral TURN credential-minting to:
the DLL never holds a static TURN password -- it fetches a short-lived
HMAC-derived cred here, per session, refreshed on the heartbeat.

Endpoints (JSON over HTTP/1.1, one request per connection, Connection: close):
  POST /v1/host       {name,version,proto,world,locked,players_max}
GET  /v1/latest     -> {proto,mod,url}  (v59: the launch toast / update check)
                      -> {sessionId, lobbyId, hostIdentity, signalingUrl,
                          signalingToken, token, stun, turn{user,pass,ttl,uris}}
  POST /v1/heartbeat  {sessionId, token, players_cur, listed}  (30s cadence)
                      -> {ok, turn{...}}   (refreshes last_seen + TURN cred)
  POST /v1/leave      {sessionId, token}                       -> {ok}
  POST /v1/visibility {lobbyId, token, listed}                 -> {ok}
  GET  /v1/lobbies?version=  -> {lobbies:[{lobbyId,name,version,world,locked,
                                           players_cur,players_max,age}...]}
  POST /v1/join       {lobbyId}
                      -> {sessionId, peerIdentity, hostIdentity, signalingUrl,
                          signalingToken, stun, turn{user,pass,ttl,uris}}
  GET  /healthz       -> {ok, lobbies:N}

Security baked in (design 10):
  - host session bearer `token` is a server-side opaque capability (secrets,
    compare_digest), NOT forgeable/replayable beyond its holder. (We diverge from
    the doc's stateless HMAC token here on purpose: the master is already
    stateful -- it owns the lobby table -- so a stored random token is simpler
    AND strictly stronger than a signed-but-stateless one. The HMAC/single-use
    nonce form matters only for the SEPARATE signaling process, which still uses
    the shared bearer for now; per-session single-use signaling tokens are the
    Stage-6 hardening -- see SIGNALING_TOKEN below.)
  - TURN creds are time-limited REST creds (design 7): username "<unixExpiry>:
    <sessionId>", password base64(HMAC-SHA1(TURN_SECRET, username)); coturn
    validates with `use-auth-secret`/`static-auth-secret=TURN_SECRET`. TTL short
    (a long cred is a long open relay); heartbeat < TTL/2 refreshes before expiry.
  - the public /v1/lobbies row carries an OPAQUE lobbyId, never the sessionId or
    token; the real join capability (sessionId + identities + fresh TURN cred) is
    minted only in /v1/join.
  - host-supplied strings clamped + control-chars stripped (a 10 KB name is a
    cheap DoS on every client that fetches the list).
  - per-IP rate limits on every mutating endpoint; per-IP + global lobby caps
    (LRU evict the stalest); lobbies expire at LOBBY_TTL with no heartbeat.
  - /v1/lobbies served from a short cache so a fetch flood does bounded work.
  - bounded async HTTP (header/body caps + read timeout) -- no thread per request,
    no slowloris pin (mirrors coop_signaling_server.py's posture).

Config via env:
  COOP_MASTER_PORT           (default 10001)
  COOP_MASTER_SIGNING_SECRET (reserved for the Stage-6 signed signaling token;
                              optional now)
  COOP_TURN_SECRET           (REQUIRED; coturn static-auth-secret)
  COOP_SIGNALING_TOKEN        (REQUIRED; the shared bearer the DLL greets
                              signaling with -- returned to clients here)
  COOP_SIGNALING_URL         (e.g. "203.0.113.5:10000"; returned to clients)
  COOP_STUN_URI              (e.g. "stun:203.0.113.5:3478")
  COOP_TURN_URI              (e.g. "turn:203.0.113.5:3478"; transports appended)
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import os
import secrets
import sys
import time
import traceback

# ---- config -----------------------------------------------------------------

PORT = int(os.environ.get("COOP_MASTER_PORT", "10001"))
SIGNING_SECRET = os.environ.get("COOP_MASTER_SIGNING_SECRET", "")
TURN_SECRET = os.environ.get("COOP_TURN_SECRET", "")
SIGNALING_TOKEN = os.environ.get("COOP_SIGNALING_TOKEN", "")
SIGNALING_URL = os.environ.get("COOP_SIGNALING_URL", "")
STUN_URI = os.environ.get("COOP_STUN_URI", "")
TURN_URI = os.environ.get("COOP_TURN_URI", "")

# ---- limits / tunables ------------------------------------------------------

HTTP_TIMEOUT = 15.0            # seconds to receive a whole request (anti-slowloris)
MAX_HEADER = 16 * 1024        # request line + headers cap (== the start_server limit)
MAX_BODY = 64 * 1024          # JSON body cap (read by exact Content-Length)
MAX_CONNS = 256               # concurrent in-flight handlers; shed past this so a
                              # slowloris flood stays bounded to this cgroup, not the box

TURN_TTL = 120                # seconds; design 7: short (60-120), not 1h
LOBBY_TTL = 300.0             # seconds without a heartbeat -> lobby expires (design 8)
LOBBIES_CACHE_TTL = 5.0       # seconds; serve GET /v1/lobbies from a cache (DoS)

MAX_LOBBIES_PER_IP = 8        # a host machine may run a few lobbies
MAX_LOBBIES_GLOBAL = 1000     # global LRU cap (evict the stalest last_seen)

# per-(IP, class) sliding-window rate limits (window seconds, max events). Classes
# are bucketed SEPARATELY so one class can never starve another.
RL_CREATE = (60.0, 10)        # POST /v1/host  ("a few/min"; also lobby-capped per IP)
RL_JOIN = (60.0, 20)          # POST /v1/join  (a NAT'd household joins together +
                              # reconnects; short TURN cred -> looser than host, bounded)
RL_MUTATE = (60.0, 240)       # heartbeat/leave/visibility (heartbeats are frequent)

# A standalone /v1/portcheck endpoint (have the master UDP-probe the requester's
# forwarded port) was DESIGNED + audited 2026-06-11 and DEFERRED: a master that
# emits outbound UDP on request is a reflection/amplification surface its provider
# can flag, and per-IP rate limits don't bound aggregate UDP across a distributed
# source set (audit S1-S3). The SAFE shape, for when the DIRECT-host port-check UI
# lands: FOLD the probe into the /v1/host direct announce (already RL_CREATE-capped
# + tied to a real, just-validated session), send ONE nonce datagram, have the
# host echo-confirm on the socket it just bound. No standalone reflector.

# Trust a forwarded client IP ONLY from a loopback peer (a local reverse proxy, e.g.
# xray fronting :443). Never from a remote peer -> a remote client cannot spoof its
# rate-limit identity via X-Forwarded-For. (Audit H1: the rate-IP would otherwise
# collapse to 127.0.0.1 for every request once the master sits behind a proxy.)
TRUSTED_PROXY_PEERS = {"127.0.0.1", "::1"}

# clamp host-supplied strings to the DLL's ServerRow buffer sizes
# (server_browser.cpp: name[64], world[40], version[24]) minus the NUL.
MAX_NAME = 63
MAX_WORLD = 39
MAX_VERSION = 23

# ---- latest released mod (v59: the launch toast + the browser join gate) ----
# OPERATOR-MAINTAINED: bump on each mod release (part of the VPS deploy step).
# Served verbatim by GET /v1/latest; hosts also announce their own `proto`
# (kProtocolVersion) which the browser uses for the reject-on-Join gate.
LATEST_PROTO = 66
LATEST_MOD = "0.9.0-n"
LATEST_URL = "https://github.com/pelmentor/VOTV_MP/releases"

# ---- state ------------------------------------------------------------------


class Lobby:
    __slots__ = ("session_id", "lobby_id", "token", "host_identity", "name",
                 "version", "proto", "world", "locked", "players_cur", "players_max",
                 "listed", "last_seen", "ip", "conn", "direct_port")

    def __init__(self, ip: str):
        self.session_id = secrets.token_hex(16)       # 32 chars, secret
        self.lobby_id = secrets.token_hex(8)          # 16 chars, opaque public id
        self.token = secrets.token_urlsafe(24)        # host bearer capability
        self.host_identity = "h" + secrets.token_hex(8)   # 17 chars (<=31 for GNS)
        self.name = ""
        self.version = ""
        self.proto = 0       # host's kProtocolVersion (v59 join gate; 0 = not announced)
        self.world = ""
        self.locked = False
        self.players_cur = 0
        self.players_max = 4
        self.listed = True
        self.last_seen = time.monotonic()
        self.ip = ip
        # Connection type (2026-06-11 direct-lobby support): "p2p" (default;
        # join returns signaling/ICE creds) or "direct" (the host runs a plain
        # UDP listen on a FORWARDED port; join returns addr "ip:port" -- the ip
        # is what the HOST's announce arrived from, never client-supplied).
        self.conn = "p2p"
        self.direct_port = 0


lobbies: dict[str, Lobby] = {}            # sessionId -> Lobby
lobby_by_public: dict[str, str] = {}      # lobbyId   -> sessionId
rate: dict[str, list[float]] = {}         # "ip|class" -> recent monotonic timestamps
conns = 0                                 # in-flight request handlers (MAX_CONNS cap)
# /v1/lobbies cache: the FULL listed-row set built once per TTL, then filtered per
# request. (Audit: a single body slot keyed by version filter was bypassable with a
# varying ?version= -> O(lobbies) rebuild every request. Building once + filtering a
# cached list bounds the per-request work and the cache size.)
_cache: dict[str, object] = {"t": 0.0, "rows": [], "all_body": b""}


def log(msg: str) -> None:
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


# ---- helpers ----------------------------------------------------------------


def clamp_str(v: object, maxlen: int) -> str:
    """Coerce to str, strip control chars (keep printable + space), clamp length."""
    s = v if isinstance(v, str) else ("" if v is None else str(v))
    s = "".join(c for c in s if c.isprintable())
    return s[:maxlen]


def resolve_client_ip(peer_ip: str, headers: dict) -> str:
    """The trusted client IP. Used as the rate-limit / lobby-cap key AND (2026-06-11)
    as an ACTION TARGET: the direct-lobby advertised address (lo.ip) and the
    /v1/portcheck UDP probe destination -- so it MUST be spoof-proof, not merely
    spoof-resistant.

    Trust a forwarded header ONLY from a loopback peer (our single co-located reverse
    proxy, e.g. xray fronting :443 per the deployment design). X-Real-IP is the proxy's
    OVERWRITTEN single value (a client-sent one is replaced) -> trusted as-is. For
    X-Forwarded-For we take the RIGHTMOST entry -- the one OUR trusted proxy appended
    (= the peer IP it actually observed). Audit C-1 (2026-06-11): the prior code took
    the LEFTMOST entry, which is the value the CLIENT sent and our proxy merely appended
    after -> a client could forge `X-Forwarded-For: <victim>` and make the master probe
    or advertise the victim's address. (Single trusted hop assumed, matching the
    deployment; a multi-proxy chain would need to peel exactly the trusted-hop count.)
    A non-loopback peer's forwarded headers are ignored entirely -> the raw TCP peer
    (handshake-validated, unspoofable off-path) wins."""
    if peer_ip in TRUSTED_PROXY_PEERS:
        xr = headers.get("x-real-ip")
        if xr:
            return xr.strip()
        xff = headers.get("x-forwarded-for")
        if xff:
            parts = [p.strip() for p in xff.split(",") if p.strip()]
            if parts:
                return parts[-1]  # rightmost = what the trusted proxy observed
    return peer_ip


def rate_ok(ip: str, cls: str, window: float, limit: int) -> bool:
    """Per-(IP, class) sliding window. Classes are bucketed SEPARATELY so frequent
    heartbeats (the `mutate` class) can never eat the `create` budget (a host that
    heartbeats every 30s would otherwise throttle its own /v1/join), and vice versa."""
    key = ip + "|" + cls
    now = time.monotonic()
    bucket = rate.get(key)
    if bucket is None:
        bucket = []
        rate[key] = bucket
    cutoff = now - window
    # prune in place (kept small by the window)
    i = 0
    for ts in bucket:
        if ts >= cutoff:
            break
        i += 1
    if i:
        del bucket[:i]
    if len(bucket) >= limit:
        return False
    bucket.append(now)
    return True


def turn_creds(label: str) -> dict:
    """coturn REST time-limited credential (design 7), bound to `label` (the host or
    PEER identity, not the shared sessionId) so coturn's per-username quota accounting
    is per-peer -- one abuser's allocations can't eat a co-joiner's quota. Empty if
    TURN is not configured. coturn validates this via use-auth-secret/static-auth-
    secret=TURN_SECRET (the provisioner must be in that mode, NOT lt-cred static user)."""
    if not TURN_URI or not TURN_SECRET:
        return {}
    exp = int(time.time()) + TURN_TTL
    username = f"{exp}:{label}"
    mac = hmac.new(TURN_SECRET.encode("utf-8"), username.encode("utf-8"),
                   hashlib.sha1).digest()
    password = base64.b64encode(mac).decode("ascii")
    uris = [f"{TURN_URI}?transport=udp", f"{TURN_URI}?transport=tcp"]
    return {"user": username, "pass": password, "ttl": TURN_TTL, "uris": uris}


def ice_block(label: str) -> dict:
    """The connectivity block every host/join response carries (TURN cred bound to
    `label` = the requesting peer's identity)."""
    return {
        "signalingUrl": SIGNALING_URL,
        "signalingToken": SIGNALING_TOKEN,
        "stun": STUN_URI,
        "turn": turn_creds(label),
    }


def evict_if_full() -> None:
    """Global LRU cap: drop the stalest lobby when over the global ceiling."""
    while len(lobbies) >= MAX_LOBBIES_GLOBAL:
        stalest = min(lobbies.values(), key=lambda lo: lo.last_seen)
        drop_lobby(stalest)
        log(f"evicted stalest lobby {stalest.lobby_id} (global cap)")


def drop_lobby(lo: Lobby) -> None:
    lobbies.pop(lo.session_id, None)
    lobby_by_public.pop(lo.lobby_id, None)


def lobbies_per_ip(ip: str) -> int:
    return sum(1 for lo in lobbies.values() if lo.ip == ip)


# ---- endpoint handlers : return (status:int, body:dict) ---------------------


def h_host(ip: str, body: dict) -> tuple[int, dict]:
    if not rate_ok(ip, "create", *RL_CREATE):
        return 429, {"error": "rate"}
    if lobbies_per_ip(ip) >= MAX_LOBBIES_PER_IP:
        return 429, {"error": "too many lobbies for this address"}
    evict_if_full()

    lo = Lobby(ip)
    lo.name = clamp_str(body.get("name"), MAX_NAME) or "VOTV Coop"
    lo.version = clamp_str(body.get("version"), MAX_VERSION) or "0.0.0"
    try:  # v59: the host's wire-protocol version (the browser join gate)
        lo.proto = max(0, min(65535, int(body.get("proto", 0))))
    except (TypeError, ValueError):
        lo.proto = 0
    lo.world = clamp_str(body.get("world"), MAX_WORLD)
    lo.locked = bool(body.get("locked", False))
    try:
        pm = int(body.get("players_max", 4))
    except (TypeError, ValueError):
        pm = 4
    lo.players_max = max(1, min(4, pm))
    # Direct-lobby announce (2026-06-11): conn="direct" + the host's LISTEN
    # port. The advertised ADDRESS is always this request's source ip (the
    # host's public address as the master sees it) -- a client cannot announce
    # someone else's ip. Port clamped to the unprivileged range.
    if body.get("conn") == "direct":
        try:
            dp = int(body.get("direct_port", 0))
        except (TypeError, ValueError):
            dp = -1
        if not (1024 <= dp <= 65535):
            # REJECT, do NOT silently downgrade to p2p (audit R1): the client
            # already chose a LanDirect topology locally, so a silent p2p
            # listing would hand joiners ICE creds the host can't speak ->
            # every join breaks with no diagnostic. Fail the announce loudly
            # BEFORE the lobby is registered (the dict inserts are below).
            return 400, {"error": "direct_port out of range (1024-65535)"}
        lo.conn = "direct"
        lo.direct_port = dp
    lo.players_cur = 1
    lo.last_seen = time.monotonic()

    lobbies[lo.session_id] = lo
    lobby_by_public[lo.lobby_id] = lo.session_id
    log(f"host {lo.lobby_id} '{lo.name}' v{lo.version} from {ip} "
        f"({len(lobbies)} live)")

    resp = {
        "sessionId": lo.session_id,
        "lobbyId": lo.lobby_id,
        "hostIdentity": lo.host_identity,
        "token": lo.token,
        "conn": lo.conn,  # audit R1: reflect the ACCEPTED conn so the client can
                          # confirm its direct request was honored (a p2p reflection
                          # means the client must not run a LanDirect listen).
    }
    # A DIRECT host runs a plain UDP listen and never touches signaling/ICE, so
    # it gets NO signaling token / STUN / TURN creds (don't hand out the shared
    # signaling bearer to a host that won't use it). P2P hosts get the full block.
    if lo.conn != "direct":
        resp.update(ice_block(lo.host_identity))
    return 200, resp


def _auth_by_session(body: dict) -> Lobby | None:
    sid = body.get("sessionId")
    tok = body.get("token")
    if not isinstance(sid, str) or not isinstance(tok, str):
        return None
    lo = lobbies.get(sid)
    if lo is None:
        return None
    if not hmac.compare_digest(tok, lo.token):
        return None
    return lo


def h_heartbeat(ip: str, body: dict) -> tuple[int, dict]:
    if not rate_ok(ip, "mutate", *RL_MUTATE):
        return 429, {"error": "rate"}
    lo = _auth_by_session(body)
    if lo is None:
        return 403, {"error": "unknown session or bad token"}
    try:
        pc = int(body.get("players_cur", lo.players_cur))
    except (TypeError, ValueError):
        pc = lo.players_cur
    lo.players_cur = max(0, min(lo.players_max, pc))
    if "listed" in body:
        lo.listed = bool(body["listed"])
    lo.last_seen = time.monotonic()
    return 200, {"ok": True, "turn": turn_creds(lo.host_identity)}


def h_leave(ip: str, body: dict) -> tuple[int, dict]:
    if not rate_ok(ip, "mutate", *RL_MUTATE):
        return 429, {"error": "rate"}
    lo = _auth_by_session(body)
    if lo is None:
        return 403, {"error": "unknown session or bad token"}
    drop_lobby(lo)
    log(f"leave {lo.lobby_id} ({len(lobbies)} live)")
    return 200, {"ok": True}


def h_visibility(ip: str, body: dict) -> tuple[int, dict]:
    if not rate_ok(ip, "mutate", *RL_MUTATE):
        return 429, {"error": "rate"}
    # visibility is keyed by the public lobbyId, still token-gated to the owner.
    pub = body.get("lobbyId")
    tok = body.get("token")
    if not isinstance(pub, str) or not isinstance(tok, str):
        return 400, {"error": "lobbyId + token required"}
    sid = lobby_by_public.get(pub)
    lo = lobbies.get(sid) if sid else None
    if lo is None or not hmac.compare_digest(tok, lo.token):
        return 403, {"error": "unknown lobby or bad token"}
    lo.listed = bool(body.get("listed", True))
    log(f"visibility {lo.lobby_id} listed={lo.listed}")
    return 200, {"ok": True}


def h_join(ip: str, body: dict) -> tuple[int, dict]:
    if not rate_ok(ip, "join", *RL_JOIN):
        return 429, {"error": "rate"}
    pub = body.get("lobbyId")
    if not isinstance(pub, str):
        return 400, {"error": "lobbyId required"}
    sid = lobby_by_public.get(pub)
    lo = lobbies.get(sid) if sid else None
    if lo is None:
        return 404, {"error": "lobby not found"}
    # NOTE: lo.locked (passworded) is a browser UI hint, deliberately NOT gated here.
    # The real admission gate is the game-layer post-`Connected` join-secret challenge
    # (design 10) -- a secret the master never sees, so it cannot (and must not) gate
    # on it. A master-side lock check would give false assurance + the public lobbyId
    # already leaks nothing sensitive; the host reclaims an unproven slot after connect.
    # Direct lobby (2026-06-11): the join capability is simply the host's
    # address -- the client UDP-connects straight to it (the host forwarded
    # the port). No signaling/ICE creds minted (nothing to relay).
    if lo.conn == "direct" and lo.direct_port:
        log(f"join {lo.lobby_id} DIRECT -> {lo.ip}:{lo.direct_port} from {ip}")
        return 200, {"conn": "direct", "addr": f"{lo.ip}:{lo.direct_port}"}
    # A fresh, unguessable identity per joiner (the signaling rendezvous key).
    peer_identity = "c" + secrets.token_hex(8)
    log(f"join {lo.lobby_id} as {peer_identity} from {ip}")
    resp = {
        "sessionId": lo.session_id,
        "peerIdentity": peer_identity,
        "hostIdentity": lo.host_identity,
        "conn": "p2p",
    }
    resp.update(ice_block(peer_identity))
    return 200, resp


def _build_rows() -> list:
    """Build the public row list (listed lobbies only) ONCE -- iterates the live
    table; called at most every LOBBIES_CACHE_TTL by h_lobbies, then filtered cheaply
    per request. Keeps a varying ?version= flood from forcing a live-table walk."""
    now = time.monotonic()
    rows = []
    for lo in lobbies.values():
        if not lo.listed:
            continue
        rows.append({
            "lobbyId": lo.lobby_id,
            "name": lo.name,
            "version": lo.version,
            "proto": lo.proto,  # v59: the browser's reject-on-Join gate key
            "world": lo.world,
            "locked": lo.locked,
            "players_cur": lo.players_cur,
            "players_max": lo.players_max,
            "age": int(now - lo.last_seen),
            "conn": lo.conn,  # "p2p" | "direct" -- browser badge + join routing
        })
    return rows


def h_lobbies(version_filter: str) -> bytes:
    """GET handler -> raw JSON bytes. The full listed set is rebuilt at most once per
    LOBBIES_CACHE_TTL; the unfiltered body is cached whole (the common browser fetch),
    and a version filter is applied to the cached row list (bounded, <=MAX_LOBBIES)."""
    now = time.monotonic()
    if now - float(_cache["t"]) >= LOBBIES_CACHE_TTL:
        rows = _build_rows()
        _cache["t"] = now
        _cache["rows"] = rows
        _cache["all_body"] = json.dumps({"lobbies": rows}).encode("utf-8")
    if not version_filter:
        return _cache["all_body"]  # type: ignore[return-value]
    rows = [r for r in _cache["rows"] if r["version"] == version_filter]  # type: ignore[union-attr]
    return json.dumps({"lobbies": rows}).encode("utf-8")


# ---- HTTP plumbing ----------------------------------------------------------

POST_ROUTES = {
    "/v1/host": h_host,
    "/v1/heartbeat": h_heartbeat,
    "/v1/leave": h_leave,
    "/v1/visibility": h_visibility,
    "/v1/join": h_join,
}


def write_response(writer: asyncio.StreamWriter, status: int, body: bytes,
                   ctype: str = "application/json") -> None:
    reason = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
              405: "Method Not Allowed", 413: "Payload Too Large",
              429: "Too Many Requests", 500: "Internal Server Error"}.get(status, "OK")
    head = (f"HTTP/1.1 {status} {reason}\r\n"
            f"Content-Type: {ctype}\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"Cache-Control: no-store\r\n\r\n").encode("ascii")
    writer.write(head + body)


def json_bytes(obj: dict) -> bytes:
    return json.dumps(obj).encode("utf-8")


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    global conns
    peer = writer.get_extra_info("peername")
    peer_ip = peer[0] if peer else "?"

    # Concurrent-connection admission cap: shed before any read so a slowloris flood
    # stays bounded to this process (cgroup-/MemoryMax-bound), never the shared box.
    if conns >= MAX_CONNS:
        log(f"[{peer_ip}] refused: connection cap ({MAX_CONNS})")
        try:
            writer.close()
        except OSError:
            pass
        return
    conns += 1

    try:
        # ---- read header block (bounded by the start_server limit + timed) ----
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), HTTP_TIMEOUT)
        except asyncio.LimitOverrunError:
            write_response(writer, 413, json_bytes({"error": "headers too large"}))
            return
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
            return

        lines = head.split(b"\r\n")
        try:
            method, raw_path, _ = lines[0].decode("latin-1").split(" ", 2)
        except ValueError:
            write_response(writer, 400, json_bytes({"error": "bad request line"}))
            return

        headers: dict[str, str] = {}
        for ln in lines[1:]:
            if not ln:
                continue
            c = ln.decode("latin-1").split(":", 1)
            if len(c) == 2:
                headers[c[0].strip().lower()] = c[1].strip()

        path, _, query = raw_path.partition("?")
        client_ip = resolve_client_ip(peer_ip, headers)

        # ---- body (POST only, bounded) ----
        body_obj: dict = {}
        if method == "POST":
            try:
                clen = int(headers.get("content-length", "0"))
            except ValueError:
                clen = -1
            if clen < 0 or clen > MAX_BODY:
                write_response(writer, 413, json_bytes({"error": "body too large"}))
                return
            raw = b""
            if clen:
                try:
                    raw = await asyncio.wait_for(reader.readexactly(clen), HTTP_TIMEOUT)
                except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
                    return
            if raw:
                try:
                    body_obj = json.loads(raw.decode("utf-8"))
                    if not isinstance(body_obj, dict):
                        raise ValueError("not an object")
                except (ValueError, UnicodeDecodeError):
                    write_response(writer, 400, json_bytes({"error": "bad json"}))
                    return

        # ---- route ----
        if method == "GET" and path == "/v1/lobbies":
            vf = ""
            for kv in query.split("&"):
                if kv.startswith("version="):
                    vf = clamp_str(kv[len("version="):], MAX_VERSION)
            write_response(writer, 200, h_lobbies(vf))
        elif method == "GET" and path == "/v1/latest":
            # v59 launch toast: the latest released mod (operator-maintained
            # constants). Static + tiny -- no rate class needed beyond the
            # connection cap (same exposure as /healthz).
            write_response(writer, 200, json_bytes(
                {"proto": LATEST_PROTO, "mod": LATEST_MOD, "url": LATEST_URL}))
        elif method == "GET" and path == "/healthz":
            write_response(writer, 200, json_bytes({"ok": True, "lobbies": len(lobbies)}))
        elif method == "POST" and path in POST_ROUTES:
            status, resp = POST_ROUTES[path](client_ip, body_obj)
            write_response(writer, status, json_bytes(resp))
        else:
            write_response(writer, 404, json_bytes({"error": "not found"}))

    except Exception as e:  # noqa: BLE001 -- one request must never kill the loop
        # Log the traceback (RULE 1: surface the bug, don't just swallow it) -- the
        # broad catch only keeps one request from killing the loop; it must not hide
        # the cause. The CLIENT still gets a bare 500 (no internals leaked).
        log(f"[{peer_ip}] handler error: {type(e).__name__}: {e}\n{traceback.format_exc()}")
        try:
            write_response(writer, 500, json_bytes({"error": "internal"}))
        except OSError:
            pass
    finally:
        try:
            await writer.drain()
        except (OSError, ConnectionError):
            pass
        try:
            writer.close()
            await writer.wait_closed()   # ensure FIN is sent (Connection: close)
        except (OSError, ConnectionError):
            pass
        conns -= 1


async def sweeper() -> None:
    """Expire lobbies with no heartbeat in LOBBY_TTL (design 8); prune rate buckets."""
    while True:
        await asyncio.sleep(30)
        now = time.monotonic()
        dead = [lo for lo in lobbies.values() if now - lo.last_seen > LOBBY_TTL]
        for lo in dead:
            drop_lobby(lo)
            log(f"expired {lo.lobby_id} (stale {int(now - lo.last_seen)}s)")
        # Prune rate buckets that are empty OR fully aged out. A one-shot IP leaves a
        # non-empty bucket that rate_ok() never revisits to clear, so the sweeper must
        # drop fully-stale buckets too (else `rate` leaks one entry per distinct IP).
        # Buckets are append-ordered, so v[-1] is the newest timestamp.
        cutoff = now - max(RL_CREATE[0], RL_JOIN[0], RL_MUTATE[0])
        for k in [k for k, v in rate.items() if not v or v[-1] < cutoff]:
            rate.pop(k, None)


async def main() -> None:
    if not TURN_SECRET:
        log("FATAL: COOP_TURN_SECRET not set -- refusing to mint TURN creds")
        sys.exit(1)
    if not SIGNALING_TOKEN:
        log("FATAL: COOP_SIGNALING_TOKEN not set -- clients could not reach signaling")
        sys.exit(1)
    # limit=MAX_HEADER bounds readuntil() for the header block (a header block past
    # MAX_HEADER raises LimitOverrunError -> 413); the body is read with readexactly()
    # to an already-Content-Length-bounded size, which is not subject to `limit`.
    server = await asyncio.start_server(handle, host="0.0.0.0", port=PORT,
                                        limit=MAX_HEADER)
    log(f"master listening on 0.0.0.0:{PORT} "
        f"(signaling={SIGNALING_URL or '?'} stun={STUN_URI or '?'} "
        f"turn={'on' if TURN_URI else 'off'})")
    asyncio.create_task(sweeper())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

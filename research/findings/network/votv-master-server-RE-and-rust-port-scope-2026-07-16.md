# Master/lobby + signaling server — RE + Rust-port scope (2026-07-16)

**Type:** point-in-time RE + DESIGN scope (a planned next-session task: rewrite the master/signaling
server to Rust). Not built.

**SECURITY:** all live credentials are GITIGNORED and must stay so — never paste them into a committed
file. The VPS SSH creds (IP / user / password) live in `reference_master_server_vps.md` (`.gitignore:114`);
the ops key in `vps_id_ed25519` + pinned host key `vps_known_hosts` (`.gitignore:119`). The master's
runtime secrets are ENV-only (`COOP_TURN_SECRET`, `COOP_MASTER_SIGNING_SECRET`, `COOP_SIGNALING_TOKEN`,
`COOP_SIGNALING_URL`) — never hardcoded. `tools/vps.py` reads the SSH creds from the gitignored md; SSH
prefers key auth (pinned host key), password fallback. Keep this discipline in the Rust port.

## What actually runs on the VPS (it IS in the repo — 2026-07-16 correction)

| File | LOC | Role |
|---|---|---|
| `tools/coop_master_server.py` | 679 | HTTP master/lobby: `/v1/host`, `/v1/heartbeat` (30s), `/v1/leave`, `/v1/visibility`, `/v1/lobbies`, `/v1/join`, `/v1/latest`, `/v1/portcheck`. Pure stdlib (http.server), no framework. |
| `tools/coop_signaling_server.py` | 198 | P2P signaling RELAY — routes opaque SDP/ICE blobs between two peers. NOT a WebRTC/media stack. |
| coturn (not in repo) | — | STUN/TURN on :3478, `static-auth-secret=$COOP_TURN_SECRET`. Mature C daemon — DO NOT rewrite. |
| `tools/vps_provision.sh` | ~250 | provisions the box: generates `TURN_SECRET` (openssl rand) fed to BOTH coturn + the master, writes the systemd units. |
| `tools/vps.py` | ~180 | ops helper (SSH/SFTP via paramiko, key-auth-preferred). |

Client side (the mod, C++): `lobby_announcer.cpp` (host announce + 30s heartbeat + `/v1/leave` on graceful
Stop), `lobby_client.cpp` (browser `/v1/lobbies` fetch), `signaling_client.cpp`, `http_client.h`,
`session_manager.cpp` (drives it). `ui/server_browser.cpp` is the in-mod ImGui browser (NOT the server).

## Master internals worth porting carefully

- **TURN credential minting** (`turn_creds`, master ~L253-267): coturn `use-auth-secret` scheme — username
  `<exp>:<label>`, password `base64(HMAC-SHA1(TURN_SECRET, username))`, `TURN_TTL=120`. Get the HMAC + b64
  byte-exact or TURN auth breaks. The DLL never holds a static TURN password; it fetches a short-lived one.
- **IP spoof-resistance** (`resolve_client_ip`, ~L199): feeds the `/v1/portcheck` UDP probe destination, so
  it must NOT trust `X-Forwarded-For` blindly. Port the trust model faithfully.
- **Lobby lifecycle:** `LOBBY_TTL = 300.0` (master L100) = seconds without a heartbeat before a lobby is
  reaped. `MAX_LOBBIES_GLOBAL = 1000` LRU (evict stalest `last_seen`). `LOBBIES_CACHE_TTL = 5.0` (serve the
  list from a cache — DoS bound). Rate limits: `RL_CREATE`, `RL_JOIN`.
- **Hand-rolled validation:** `clamp_str` + manual dict-key checks everywhere. In Rust this is exactly what
  `serde` typed request structs delete for free — the single biggest maintainability win of the port.

## The ghost-lobby (bug C) — a one-liner, independent of the port

A TASK-KILLED host runs no cleanup, so no `/v1/leave` is sent; the entry lingers up to `LOBBY_TTL`=300s
until the reaper drops it (measured 2026-07-16: browser showed a dead host at 237s/297s age). Fix = lower
`LOBBY_TTL` to ~90 (3 missed 30s heartbeats). Deploying it is a PRODUCTION VPS action (`tools/vps.py put`
+ restart the systemd unit) — do only with the user's explicit go. Do this whether or not the Rust port
happens.

## Rust port scope (grounded — ~877 LOC total)

- **Signaling (198 LOC): trivial.** It's a message router — tokio + a peer-pair map. A weekend.
- **Master (679 LOC): straightforward.** axum or plain hyper + serde (typed request/response structs) + an
  HMAC-SHA1 crate for the TURN creds + a small in-memory `Lobby` map with a reaper task. The careful spots
  are the two above (TURN cred format, IP resolution).
- **Keep coturn as-is.** Only the two Python services move.
- **Migration:** wire-compatible (same JSON endpoints + the same signaling protocol), so run old + new in
  parallel on different ports, cut the client's master URL over, keep `/v1/leave` semantics. No `kProtocol`
  change (that's the P2P wire; this is the master/HTTP wire, versioned separately via `proto` in the body).
- Rust vs Go both fit; Rust's serde + single-static-binary (no Python-on-VPS bitrot) + no-GIL are the
  specific wins for THIS code. Decision: Rust (user, 2026-07-16).

## Status

- Master/signaling: **AS-IS in Python, working** (the live service the coop connects through today).
- Rust port: **PLANNED next session** (user directive 2026-07-16). This doc is the scope/file-map.
- Ghost-lobby TTL fix: **identified (master L100), NOT applied** (production VPS action, awaiting go).

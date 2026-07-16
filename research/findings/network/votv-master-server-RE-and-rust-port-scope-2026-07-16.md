# Master/lobby + signaling server — RE + Rust-port scope (2026-07-16)

**Type:** point-in-time RE + DESIGN scope → **now AS-BUILT + LIVE** (written, wire-verified,
deployed, security-hardened AND migrated to the new coop VPS `172.86.94.3` — all 2026-07-16; see
"## Status" + "## MIGRATED" below). The Rust project lives at `tools/coop-server-rs/` (its README
is the operator guide).

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

## The ghost-lobby (bug C) — FIXED + DEPLOYED (2026-07-16, user go)

A TASK-KILLED host runs no cleanup, so no `/v1/leave` is sent; the entry lingered up to `LOBBY_TTL`=300s
until the reaper dropped it (measured 2026-07-16: browser showed a dead host at 237s/297s age). **FIXED:
`LOBBY_TTL = 90s`** (3 missed 30s heartbeats; `src/bin/master.rs`, commit `6d640679`) — deployed live
same day (binary `ad9844b6`, `.prev` rollback kept).

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

## AS-BUILT (2026-07-16) — the Rust port is written + wire-verified

Built at `tools/coop-server-rs/` (cargo project, two binaries `coop-master` + `coop-signaling`
sharing `src/common.rs`; ~870 LOC Rust). Deps: tokio, serde/serde_json, hmac+sha1, base64,
getrandom, socket2. Faithful to the two careful spots: the TURN cred is **byte-exact**
(`base64(HMAC-SHA1(secret,"<exp>:<label>"))`, unit-tested vs a fixed Python reference vector
`testsecret_abc123` / `1700000000:h0011223344556677` -> `c7pJt+2pR4aVy8LJIi6NtjympwM=`), and the
IP trust model is the rightmost-XFF-from-loopback-proxy rule ported verbatim. serde typed parsing +
tolerant `as_int`/`as_bool`/`coerce_str` reproduce the Python's `int()`/`bool()`/`str()` coercion;
`clamp_str` strips control/separator chars + codepoint-clamps.

**Verification (real, not a smoke-label):** a differential harness spun up all four servers (Rust +
Python, master + signaling) on distinct ports with identical secrets and asserted the Rust responses
are structurally identical to the Python's across every endpoint AND that absolute invariants hold —
**31/31 checks passed**: `/v1/latest` exact + `==py`; host-p2p keyset `==py` with full ICE block;
TURN keyset+ttl=120+2 URIs; lobby-row keyset `==py` with control-char-stripped name, echoed proto,
clamped players_max; `?version=` filter; join-p2p keyset `==py` + `c...` peerIdentity; join-missing
404; heartbeat-bad-token 403; direct-port-out-of-range 400; direct host conn=direct (no ICE); join
direct addr; bad-json 400; 404; **signaling relay delivered `idA deadbeef` A->B on both**; bad-token
greeting dropped on both. Plus 4 `cargo test` unit tests (TURN vector, token_hex shape, clamp, ct_eq).

Behaviour-equivalent improvements (NOT wire changes): multi-thread tokio + one `Mutex<MasterState>`
(no lock across an await); signaling = one-task-owns-the-socket `select!` so evict-on-dup closes the
old socket immediately (dropping the relay-channel Sender is the stop signal); bounded per-dest relay
channel (drop-on-full) replaces the 5s drain. Detail in `tools/coop-server-rs/README.md`.

## Security audit + Tier A hardening (2026-07-16) — DEPLOYED

A 4-agent audit (master HTTP / signaling relay / crypto+coturn / client parsing) ran after the deploy.
Consolidated + fixed per RULE 1; **Tier A is BUILT + DEPLOYED + committed** (server `249a22b0`, client
`7e8b1d2c`). Highlights:
- **S-1 (HIGH, port regression):** signaling relay buffered up to 1024x64 KiB (~64 MiB) per stalled
  destination → one token-holder OOMs the shared box. Fixed: 8 KiB frame cap + queue 64 (<=512 KiB/dest).
- **C-1 (HIGH, client remote crash):** a hostile/MITM master's deeply-nested JSON overflowed the worker
  stack on the recursive `~basic_json` destructor (uncatchable SEH). Fixed: depth-32 parse cap in
  `json_util.h::ParseObject`.
- master: IPv6 /64 rate-keying (+ bucket cap), TURN username IP-bound, atomic authed-admission, Arc'd
  `/v1/lobbies` body, bounded write, `panic=unwind` + poison-tolerant locks (per-conn fault isolation),
  `clamp_str` bidi/zero-width/PUA strip. client: L4/L5/L6 (clamp/cap/comma-reject).
- coturn (live-applied): `user-quota`, aggregate `bps-capacity`, CGNAT `100.64/10` + v4-mapped-IPv6
  denies; provisioner prints secret FINGERPRINTS not raw values.
- **ROOT (UNFIXED):** the control plane is **cleartext** (master HTTP :10001 + signaling TCP :10000) —
  the `signalingToken` (static shared bearer) + TURN creds are sniffable → signaling MITM / relay theft.
  **Tier B = TLS front** (a :443 front+domain OR standalone stunnel/nginx + client-pinned self-signed cert;
  needs a client https/wss cutover). **Tier C = per-session signaling tokens** (retire the shared
  bearer / identity-hijack). Both await a USER decision.

## MIGRATED to the new coop VPS (2026-07-16 evening, user decision)

The whole stack moved to the **new Cloudzy box `172.86.94.3`**; the old box now hosts **only unrelated services**
(coop services stopped/removed there per RULE 2, verified: no coop listeners, master dead, the box's other tenants
untouched). The new box was provisioned by the REWORKED `tools/vps_provision.sh` (commit `2932a18d`):
Rust ExecStart directly (no Python ever landed), stop-before-replace binary install, `curl -4`
public-IP (the dual-stack box answered ifconfig.me over v6 → master handed unbracketed-v6 URIs,
measured), ufw allows (10000/10001/3478/61000-61100/udp + **80/tcp for Let's Encrypt**), realm
`votv.mp`, fresh secrets (clients fetch signaling token + TURN creds FROM the master; only
`net.master` is a client-side constant). **Functionally verified from OUTSIDE**: healthz,
`/v1/latest`→111, host→join full ICE, signaling relay A→B, leave (+ the 5s `/v1/lobbies` cache),
TTL=90 reaper (`expired ... stale 98s`), TURN-cred HMAC recomputed on-box = MATCH.
Client side (commit `ee8b463e`, DLL `AFBF5728` x4 hash-verified): `protocol.h`
`kOfficialMasterUrl/kOfficialSignalingUrl` → `172.86.94.3`; `session_manager.cpp` `kDefaultMaster`
duplicate literal retired (aliases protocol.h); all 4 installs' inis carry explicit `net.*`
(HOST had NO `[net]` block — rode the compiled default; CLIENT_3 had no ini at all — created).
Domain `votv.mp`: Cloudflare DNS-only zone, NS delegation pending at the .mp registry (~24h;
`.mp` is NOT transferable to CF Registrar — checked the 422-TLD list).

## Status

- Master/signaling: **RUST, LIVE on `172.86.94.3`** (migrated 2026-07-16; see "## MIGRATED").
  Same binaries byte-exact as the audited deploy (master `ad9844b6`, signaling `930b6173`).
- Ghost-lobby TTL: **DONE** — `LOBBY_TTL = 90s` (commit `6d640679`); reaper verified on the new box.
- `/v1/latest` release info: **env-overridable** (`COOP_LATEST_PROTO/MOD/URL`; compiled default 111;
  provisioner writes `COOP_LATEST_PROTO=111`). Live-verified on the new box -> proto 111.
- RULE-2 finalization: provision script is Rust-native (`2932a18d`) and the old box is wiped — DONE.
  REMAINING (user-gated): delete the retired Python reference impls from the REPO
  (`tools/coop_master_server.py`, `tools/coop_signaling_server.py`) + `tools/vps.py`'s fate (it
  targets the old box, now coop-free; banner added meanwhile).
- NEXT: **Tier B TLS** on the new box once DNS lands (LE cert `master.votv.mp` via :80 + rustls in
  our bins — :443 is another tenant's there too + client https/wss cutover, `net.master=https://master.votv.mp`),
  then **Tier C per-session signaling tokens**. Control plane is cleartext until Tier B.

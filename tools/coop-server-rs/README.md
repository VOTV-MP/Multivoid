# coop-server-rs — VOTV coop master + signaling (Rust)

Rust port of the two Python services. **RULE 3: VPS infra, it never ships in the
mod.** Wire-compatible with the Python originals (identical JSON endpoints,
identical signaling line protocol, byte-exact coturn TURN credential) so old + new
could run in parallel during cutover.

Status: **AS-BUILT, DEPLOYED, AND CURRENT — cut over 2026-08-31.** For two days
before that the deployed binaries were two arcs behind the tree; they are not any
more. coturn stays as-is (not ported).

| on the box | binary | proven in production 2026-08-31 |
|---|---|---|
| `coop-master` | b149 source, `18663ad7054f6dab` | `/v1/join` DIRECT returns `hostIdentity`; an identity-less b≤133 host gets the named 400 |
| `coop-signaling` | b149 source, `ce2212a1e8fc7eed` | `sig_gate --remote` **PASS 14/14** — the A59 registration challenge. The binary it replaced was **FAIL C, no challenge at all** |

`[V]` **before/after on the live relay, not inferred from the staged run**: FAIL C
on the old binary, PASS 14/14 after the restart, same instrument minutes apart.
TLS verified from outside afterwards (`:10443/healthz` → 200, `:10442` → TLS 1.2).

Two consequences worth knowing before touching the box:

- **The b≤133 cohort is retired and that is now LIVE**, not pending. Within seconds
  of the restart the relay refused three real IPs by name and the one listed lobby
  (proto 133) was dropped. Authorised by the user, whose call was that the master
  had to be current before their local zip test and the cohort was not a reason to
  wait.
- **`COOP_MAX_BUILD` is deleted from `/etc/coop-master.env`.** The gate was retired
  whole on 2026-08-31 (`24418b66`; `master.rs:348`) and the value was ignored, but
  left in place it is a stale claim that would re-arm on a rollback.

Rollback set on the box: `coop-master.bak-20260831`,
`coop-signaling.bak-20260831`, `/etc/coop-master.env.bak-20260831`.
Deploy recipe (CRLF trap, `ETXTBSY`, the differential): `docs/RELEASE.md` step 6c.

`tools/coop_signaling_server.py` was **retired 2026-08-29** (RULE 2): the cutover
was finished and its last job was to be the local fixture four rig scenarios
launched, which meant a line-protocol change would have had to land in two
implementations while the rig proved it against the copy that never ships.
`tools/mp.py:signaling_exe()` builds and launches `coop-signaling` instead
(measured 9.86 s from cold on Windows). `coop_master_server.py` is still here --
it is the rig's master fixture and has not been given the same treatment.

## Binaries

| bin | replaces | default port | env |
|---|---|---|---|
| `coop-master` | `coop_master_server.py` | `COOP_MASTER_PORT` (10001) | `COOP_TURN_SECRET`*, `COOP_SIGNALING_TOKEN`*, `COOP_SIGNALING_URL`, `COOP_STUN_URI`, `COOP_TURN_URI` |
| `coop-signaling` | `coop_signaling_server.py` (retired) | `COOP_SIGNALING_PORT` (10000) | `COOP_SIGNALING_TOKEN`* |

`*` required — the process refuses to start (exit 1) without it, same as the Python
`FATAL` guards. All secrets are ENV-only; nothing is hardcoded (keep it that way).

## Build

```
cargo build --release      # -> target/release/coop-master(.exe), coop-signaling(.exe)
cargo test                 # unit tests incl. the byte-exact TURN-cred vector vs Python
```

Cross-compile for the Linux VPS (from Windows) once cutover is scheduled, e.g.
`cargo build --release --target x86_64-unknown-linux-gnu` (needs the linux target +
a linker; or build on the box / in CI). A single static-ish binary per service — no
Python-on-VPS runtime to bitrot.

## What is faithful to the Python (verified)

- **Endpoints**: `/v1/host`, `/v1/heartbeat`, `/v1/leave`, `/v1/visibility`,
  `/v1/join`, `/v1/lobbies`, `/v1/latest`, `/healthz` — identical JSON field names,
  status codes, and error shapes (31/31 differential checks).
- **TURN credential** (the byte-exact spot): `username = "<unixExp>:<label>"`,
  `password = base64(HMAC-SHA1(TURN_SECRET, username))`, `ttl = 120`, two `?transport`
  URIs. Unit-tested against a fixed Python reference vector.
- **Security posture**: rightmost-`X-Forwarded-For` from a loopback proxy only
  (spoof-proof rate/target IP), constant-time token compare, opaque `lobbyId` vs
  secret `sessionId`, `clamp_str` control-char strip + codepoint clamp, per-(IP,class)
  sliding-window rate limits, global LRU + per-IP lobby caps, `LOBBY_TTL` sweep,
  bounded header/body/conn caps + read timeouts.
- **Signaling**: `<token> <identity>` greeting, `<dest> <hex>` relay, pre-auth vs
  authed pools, per-IP caps, evict-on-duplicate-identity.

## Where it IMPROVES on the Python (behaviour-equivalent, not a wire change)

- **Concurrency**: multi-thread tokio + a single `Mutex<MasterState>` (master);
  one-task-owns-the-socket `select!` (signaling). No lock is ever held across an
  await — mirrors the Python asyncio single-loop safety without giving up cores.
- **Signaling evict-on-dup is immediate + clean**: dropping a peer's relay-channel
  `Sender` closes the channel, so the old connection's `select!` sees `recv()==None`
  and shuts its whole socket down at once — no reliance on the OS to unwedge a stale
  reader (the Python `prev.close()` path).
- **Relay backpressure**: a bounded per-destination channel (drop-on-full) replaces
  the Python 5s drain timeout — a slow destination can never head-of-line-block a
  sender, and memory per destination is hard-bounded.
- **serde typed JSON** deletes the hand-rolled dict-key validation.

## Cutover (per the migration plan)

Wire-compatible, so: run the Rust services on **different ports** alongside the
Python ones, point one test client's master URL at the Rust master, verify a real
host+join+relay, then cut the production master URL over and retire the Python
(RULE 2 — the `.py` files go once the Rust is proven in production, no dual-run kept).
`LATEST_PROTO`/`LATEST_MOD`/`LATEST_URL` + `LOBBY_TTL` are operator constants in
`src/bin/master.rs` — keep them in sync with any Python-side change until cutover.

Scope + careful-spots detail:
`research/findings/network/votv-master-server-RE-and-rust-port-scope-2026-07-16.md`.

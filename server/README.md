# coop-server-rs: the master and the signaling relay

The two services the mod talks to outside a session: the master (the lobby list, the join, the
TURN credential) and the signaling relay (the rendezvous a joiner and a host use to reach each
other). Both are VPS infrastructure and never ship in the mod. What they are for, who owns what
and what they refuse is on `docs/master-server.md`; this file is how to build, run and change
them.

## Binaries

| bin | default port | env |
|---|---|---|
| `coop-master` | `COOP_MASTER_PORT` (10001) | `COOP_TURN_SECRET`*, `COOP_SIGNALING_TOKEN`*, `COOP_DOMAIN`*, `COOP_MASTER_NAME_ALIASES`, `COOP_MIN_LISTED_PROTO`, `COOP_BLOCKED_IDENTITIES`, `COOP_SIGNALING_URL`, `COOP_STUN_URI`, `COOP_TURN_URI` |
| `coop-signaling` | `COOP_SIGNALING_PORT` (10000) | `COOP_SIGNALING_TOKEN`* |

`*` required: the process refuses to start (exit 1) without it. All secrets are env-only;
nothing is hardcoded, and it stays that way.

The master's names: `COOP_DOMAIN` is required and is the name its clients dial, written as a
bare host (`master.example.org` or an IPv4 literal: no scheme, path, login, port or IPv6 form; the
master refuses to start without it or on anything else). A lobby proof is signed for that name, so the `net.masters` entry a player adds
must dial the same host. `COOP_MASTER_NAME_ALIASES` is an optional comma list of other names it
answers to, never a substitute for it (an IPv4 literal, a dev host): each entry follows the same rules
bar an optional `:port`, which is stripped, and an invalid entry stops the master at start; names are
trimmed and lower-cased, and a loopback alias is dropped with a warning. `COOP_MIN_LISTED_PROTO` (default 0) is the
floor: a create from a build below it is refused, and a value that is not a decimal number
stops the master at start.

## Build

```
cargo build --release      # -> target/release/coop-master(.exe), coop-signaling(.exe)
cargo test                 # unit tests incl. the byte-exact TURN-credential vector
```

The Linux binaries are built on the box itself: copy `Cargo.toml`, `Cargo.lock`, `.cargo/` and
`src/` there and run `cargo build --release --target x86_64-unknown-linux-musl`. TLS brought in
`ring`, whose build compiles C, so a Windows cross-build needs a musl C cross-compiler that a
stock Windows toolchain does not have; the box has `musl-tools`. One static binary per service;
no interpreter on the box to rot.

The rig builds and launches the signaling binary for its own scenarios, so a
line-protocol change is proven against the copy that ships. The browser rig uses
a synthetic lobby list as its source instead of a master, because a real master's rate
limits and per-IP caps are the wrong fixture for many synthetic rows from one address.

## The wire

- **Endpoints**: `/v1/host/challenge`, `/v1/host`, `/v1/heartbeat`, `/v1/leave`, `/v1/visibility`, `/v1/join`,
  `/v1/lobbies`, `/v1/latest`, `/v1/thanks`, `/healthz`. The mod's side is
  `coop/net/lobby_client` and `coop/net/lobby_announcer`.
- **The lobby proof**: a create is two calls. `POST /v1/host/challenge` (body `{}`) answers a
  `nonce`; `POST /v1/host` then carries `nonce` and `proof`, the host's Ed25519 signature over
  `"multivoid-lobby-announce-v1" || identity || nonce || master_name` (`src/lobby_proof.rs`, the
  shared vector in `tests/vectors/lobby_proof.json`). The nonce is stateless: its expiry, random
  bytes and an HMAC (a secret drawn when the master starts) over the caller's address bucket, so
  issuing stores nothing and no flood of challenges can fill the master; only a spent nonce is
  kept, until it expires (single use; a full set, 65536 (sized to the master's memory), answers
  `503 busy` and never forgets a nonce). A restart voids
  outstanding nonces and the host asks again. The challenge has its own rate class. A build at or
  above `MIN_PROVED_PROTO` (`src/version_gate.rs`) without a valid proof is `409 proof-required`;
  a proof that is present must verify (`400 bad-proof`); a nonce that is not this master's for
  this address, or is spent or expired, is `410 nonce-expired`; a build below
  `COOP_MIN_LISTED_PROTO` is `403 build-too-old`; an identity in `COOP_BLOCKED_IDENTITIES` (comma
  list of `gen:` keys; an entry of another shape refuses the start) is `403 blocked` and never listed (asked after the proof and the floor, so only a create that proves its key, or a build that need not, learns it).
  A valid proof marks the row `proved`, and a
  proved create for a key already listed replaces that key's row (new session and token). When
  the table is full a create evicts the stalest unproved row, then the stalest proved one; an
  unproved create is refused (`429`) when every row is proved. `/v1/lobbies` rows carry
  `identity` and `proved`. The host name a client signs is the host of its master URL, by the
  rules of `canon_host_name` (`src/common.rs`; cases in `tests/vectors/master_names.json`).
- **The thanks list**: `/v1/thanks` answers `{"text": ...}` with the file `COOP_THANKS_FILE`
  names (default `/etc/coop-thanks.txt`), a copy of `src/votv-coop/assets/thanks/thanks.txt`.
  Re-read at most every thirty seconds, capped at 64 KiB, 404 when it cannot be served whole.
  Publishing a name is copying the file to the box; nothing restarts.
- **The TURN credential**, the byte-exact spot: `username = "<unixExp>:<label>"`,
  `password = base64(HMAC-SHA1(TURN_SECRET, username))`, two `?transport` URIs, and a `ttl` per
  holder (`src/ice.rs`): 120 s for a joiner, 600 s for a host, whose heartbeat reports the one it
  holds and is answered with a new one once the master's record is half spent. Unit-tested against
  a fixed reference vector; a mismatch breaks coturn auth silently.
- **Signaling**: a `<token> <identity>` greeting, then `<dest> <hex>` relay lines; pre-auth and
  authed pools; per-IP caps; a duplicate identity evicts the older connection. The registration
  challenge (a peer proves its key before it may be addressed) is drilled against a live relay,
  locally against a built relay, or with `--remote` against the live one.

## Security posture

Rightmost `X-Forwarded-For` from a loopback proxy only, so the rate-limit and target address
cannot be spoofed; constant-time token compare; an opaque `lobbyId` against a secret `sessionId`;
control-character strip and codepoint clamp on every string; per-(address, class) sliding-window
rate limits (the table of them holds 50000 keys and, full, forgets the least recently seen rather
than refuse a new address; `GET /v1/lobbies` has its own class, 120 per minute); a global LRU and per-address lobby caps; a `LOBBY_TTL` sweep; bounded header and
body; connection admission at accept (`admission.rs`: a total and a per-address cap, taken before
the TLS handshake) under one deadline that covers the handshake and every read before the
answer, or on the relay before registration; a relay destination's queue is capped in bytes;
an address past `COOP_CONNECT_FLOOD_MAX` connects (default 20, at most 40, 0 is off) inside ten seconds is
ignored for a minute, and `COOP_CONNECT_FLOOD_EXCLUDE` (comma-separated addresses) is never limited.

## Design notes

- Multi-thread tokio with a single `Mutex<MasterState>` on the master; one task owns the socket
  in the relay. No lock is ever held across an await.
- Evicting a duplicate identity drops the peer's relay `Sender`, which closes the channel and
  shuts the old socket down at once; nothing waits on the OS to unwedge a stale reader.
- A bounded per-destination channel with drop-on-full: a slow destination can never
  head-of-line-block a sender, and memory per destination is hard-bounded at 64 KiB, so every
  destination together fits the unit's memory cap.
- Typed JSON through serde replaces hand-rolled key validation.
- `COOP_LATEST_GAMES` (the newest release per game version, `<game>:<build>:<tag>,...`) and
  `COOP_LATEST_URL` in the master's environment name the builds for the update check, which is
  informational and never a gate; the release ritual writes the list from the release ledger.
  `LOBBY_TTL` sits beside `LATEST_URL` in the source.

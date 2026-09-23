# coop-server-rs: the master and the signaling relay

The two services the mod talks to outside a session: the master (the lobby list, the join, the
TURN credential) and the signaling relay (the rendezvous a joiner and a host use to reach each
other). Both are VPS infrastructure and never ship in the mod. What they are for, who owns what
and what they refuse is on `docs/master-server.md`; this file is how to build, run and change
them.

## Binaries

| bin | default port | env |
|---|---|---|
| `coop-master` | `COOP_MASTER_PORT` (10001) | `COOP_TURN_SECRET`*, `COOP_SIGNALING_TOKEN`*, `COOP_SIGNALING_URL`, `COOP_STUN_URI`, `COOP_TURN_URI` |
| `coop-signaling` | `COOP_SIGNALING_PORT` (10000) | `COOP_SIGNALING_TOKEN`* |

`*` required: the process refuses to start (exit 1) without it. All secrets are env-only;
nothing is hardcoded, and it stays that way.

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

- **Endpoints**: `/v1/host`, `/v1/heartbeat`, `/v1/leave`, `/v1/visibility`, `/v1/join`,
  `/v1/lobbies`, `/v1/latest`, `/v1/thanks`, `/healthz`. The mod's side is
  `coop/net/lobby_client` and `coop/net/lobby_announcer`.
- **The thanks list**: `/v1/thanks` answers `{"text": ...}` with the file `COOP_THANKS_FILE`
  names (default `/etc/coop-thanks.txt`), a copy of `src/votv-coop/assets/thanks/thanks.txt`.
  Re-read at most every thirty seconds, capped at 64 KiB, 404 when it cannot be served whole.
  Publishing a name is copying the file to the box; nothing restarts.
- **The TURN credential**, the byte-exact spot: `username = "<unixExp>:<label>"`,
  `password = base64(HMAC-SHA1(TURN_SECRET, username))`, `ttl = 120`, two `?transport` URIs.
  Unit-tested against a fixed reference vector; a mismatch breaks coturn auth silently.
- **Signaling**: a `<token> <identity>` greeting, then `<dest> <hex>` relay lines; pre-auth and
  authed pools; per-IP caps; a duplicate identity evicts the older connection. The registration
  challenge (a peer proves its key before it may be addressed) is drilled against a live relay,
  locally against a built relay, or with `--remote` against the live one.

## Security posture

Rightmost `X-Forwarded-For` from a loopback proxy only, so the rate-limit and target address
cannot be spoofed; constant-time token compare; an opaque `lobbyId` against a secret `sessionId`;
control-character strip and codepoint clamp on every string; per-(address, class) sliding-window
rate limits; a global LRU and per-address lobby caps; a `LOBBY_TTL` sweep; bounded header and
body; connection admission at accept (`admission.rs`: a total and a per-address cap, taken before
the TLS handshake) under one deadline that covers the handshake and every read after it.

## Design notes

- Multi-thread tokio with a single `Mutex<MasterState>` on the master; one task owns the socket
  in the relay. No lock is ever held across an await.
- Evicting a duplicate identity drops the peer's relay `Sender`, which closes the channel and
  shuts the old socket down at once; nothing waits on the OS to unwedge a stale reader.
- A bounded per-destination channel with drop-on-full: a slow destination can never
  head-of-line-block a sender, and memory per destination is hard-bounded.
- Typed JSON through serde replaces hand-rolled key validation.
- `LATEST_PROTO`, `LATEST_MOD` and `LATEST_URL` in the master binary's source are the operator
  constants that name the newest build for the update check, which is informational and never
  a gate; `LOBBY_TTL` sits beside them.

# The master server

## Purpose

The two services that live outside the game: the master, which keeps the lobby list, brokers a
join and answers the update check, and the signaling relay, which is the rendezvous two peers
use to traverse NAT, with a TURN server as the fallback path. What they know and do not know,
how a host lists itself, how a client joins through them, and what a self-hoster runs. Nothing
here ships inside the mod.

## How it works

### Two binaries

Both services are Rust, in `tools/coop-server-rs/`: `coop-master` and `coop-signaling`, static
binaries configured by environment variables, with every secret in the environment and none in
the code; a binary refuses to start without its required secret. Each terminates TLS inside
itself on a second port beside its plaintext one, with a real certificate on the public host. The
shipped mod speaks TLS to the master, and a bare `host:port` in the ini means TLS; an explicit
`http://` is the deliberate downgrade for a self-hoster without a certificate. The mod's
signaling leg is still plaintext TCP.

### The master

A small HTTP JSON service. A host announces itself with the world, the player count and its
identity and gets back a session id, an opaque lobby id and a host token; it heartbeats every
thirty seconds with its player count and whether it is listed, can flip its listing, and leaves
on stop. A lobby that misses three heartbeats is reaped, so a killed host disappears from the
browser within a minute and a half (`coop/net/lobby_announcer`). A client fetches the list for its
version and, on a join, asks the master for the way to dial the host: the credentials, both
identities and the ICE configuration (`coop/net/lobby_client`). For a brokered lobby that
includes short-lived TURN credentials the master mints from a shared secret, the same recipe the
TURN server checks. The master also answers the update check with the latest released pair; with
no released record it answers nothing, and the client stays silent.

The master never sees game traffic. Its posture is the ordinary one for a public endpoint:
per-address and per-class rate limits, a global and a per-address lobby cap, an opaque lobby id
distinct from the secret session id, control characters stripped and strings clamped, bounded
bodies and headers, and the forwarded-for header trusted only from a loopback proxy.

### Direct and brokered

A host is reachable one of two ways (`coop/session/host_mode`). Direct is a listen on a port,
reached over a LAN or a forwarded port; it may be listed or not, and an unlisted direct host
makes no master call at all. Brokered is a peer-to-peer session through NAT traversal, where the
master is the only rendezvous: the signaling relay carries the candidates and the TURN server
relays the media when no direct path opens. The two are not two transports, and neither restricts
who may connect; the boundary a host wants comes from the password and the admission challenge,
which apply to every lane.

### The signaling relay

A line protocol over TCP, ported from the transport library's own example. A peer greets the
relay with the shared token and its identity, which is its own public key; the relay answers a
nonce and the peer signs it with the key its identity names, and only a proved identity is
registered. From then on every line names a destination identity and a hex payload, and the
relay forwards it with the sender's identity in front; a duplicate registration evicts the
older connection at once, and a slow destination has a bounded queue that drops rather than
blocks. The mod fails closed on a relay that never challenges it: registering unproved would
reopen exactly what the challenge closes, so a build that requires the challenge is published
only after a drill proves the deployed relay speaks it (`tools/sig_gate.py`).

### Self-hosting

Run the two binaries with their environment (the TURN secret, the signaling token, the
signaling and TURN addresses) and a TURN server that shares the secret, then point the ini's
custom master and signaling rows at them. `tools/mp.py` builds and launches the signaling binary
locally for a scripted run, and `tools/fake_master.py` serves a synthetic lobby list to the browser.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the lobby list | the master | announced by hosts, reaped on missed heartbeats |
| a join's dialing information | the master | handed to the joiner once |
| a peer's identity | the peer | its own public key; the master relays the host's to the joiner |
| the rendezvous | the signaling relay | forwarded lines, proved registrations |
| the media when no path opens | the TURN server | credentials minted by the master |
| the game traffic | never the servers | end to end between peers |

## Wire messages

| Route | Who | Carries |
|---|---|---|
| `/v1/host`, `/v1/heartbeat`, `/v1/visibility`, `/v1/leave` | the host | the announce; the keepalive with players and listing; the listing flip; the leave |
| `/v1/lobbies`, `/v1/join` | the client | the list for a version; the dialing information for a lobby |
| `/v1/latest`, `/healthz` | the client; an operator | the released pair; liveness |
| the signaling lines | both peers | the greeting, the challenge and its proof, the forwarded candidates |

## Late join

Not applicable: the servers act before a session exists. A lobby whose host died lingers at
most until its heartbeats lapse.

## Known limits

| Limit | Evidence |
|---|---|
| The signaling leg is plaintext, so an on-path attacker can relay the registration challenge and hold a victim's name; encrypting it is the next transport item | `[V]` `coop/net/signaling_client.h` |
| One master, no redundancy | `[V]` `coop/net/lobby_announcer` |
| The `http://` downgrade grammar still ships and is queued for removal | `[V]` `coop/net/http_client` |

## Code map

| Concept | Files |
|---|---|
| the services | `tools/coop-server-rs/src/bin/master.rs`, `tools/coop-server-rs/src/bin/signaling.rs`, `tools/coop-server-rs/src/tls.rs`, `tools/coop-server-rs/src/common.rs`, `tools/coop-server-rs/README.md` |
| the mod's master client | `coop/net/lobby_client`, `coop/net/lobby_announcer`, `coop/net/http_client`, `coop/session/session_manager` |
| the rendezvous | `coop/net/signaling_client.h`, `coop/net/ice_config.h`, `coop/session/host_mode` |
| the drills and fixtures | `tools/sig_gate.py`, `tools/fake_master.py` (a synthetic lobby list for the browser), `tools/cert_check.py` (the off-box certificate check) |

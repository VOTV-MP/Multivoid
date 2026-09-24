# The master server

## Purpose

The two services that live outside the game: the master, which keeps the lobby list, brokers a
join and answers the update check, and the signaling relay, which is the rendezvous two peers
use to traverse NAT, with a TURN server as the fallback path. What they know and do not know,
how a host lists itself, how a client joins through them, and what a self-hoster runs. Nothing
here ships inside the mod.

## How it works

### Two binaries

Both services are Rust, in `server/`: `coop-master` and `coop-signaling`, static
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
no released record it answers nothing, and the client stays silent. And it serves the thanks
list the main menu rolls ([ui.md](ui.md)): the text of one file on the box, named by
`COOP_THANKS_FILE` and re-read at most every thirty seconds, so publishing a name is moving a
file into place, with no restart. **Three answers, and only one of them is destructive.** The text
is served as 200. A file that is not there at all answers 404, the master saying it has no list,
and that is the one answer on which a client drops what this master said before and shows the copy
in its build. A file that is there but cannot be served whole this moment -- empty, oversized, not
UTF-8, unreadable -- is not a removal: the last good copy keeps being served, and if there is none
yet the answer is 503, on which the client keeps whatever it already holds. That distinction is
what makes publishing safe while players are connected. The disk is read off the runtime's threads
and outside the cache's lock, and the response body is built once per re-read window rather than
per request.

### Several masters

There is more than one master, and they do not know about each other: each box has its own lobby
list, its own signaling relay and TURN server, its own secrets and its own certificate. The
client's list of them is one ini row, `net.masters`, whose default is the word `default`: it
stands for the official masters compiled into the build (USA first, then EU), so a player who
adds one of their own after it (`default,Home=host:port`) still gets whatever official masters a
later build ships. The browser shows one master's lobbies at a time, chosen with a tab above the
list, and the choice is the `net.master` row, written on the click and read back on the next
launch (`coop/net/master_slots`, `ui/server_browser_tabs`). A lobby lives on the master it was
announced to, and everything a session needs follows from that one fact: a host announces to the
chosen master, which is then also its rendezvous and relay; the list on screen knows which master
it came from, and a join goes through that master, the password prompt included; switching tabs
drops the old list at once, since a lobby id means nothing on another master. The update check and
the thanks list come from the chosen master too, so only the master the player picked learns
their address. Every release before the list existed knows only the first master's address, which
is why the default slot keeps it.

The master never sees game traffic. Its posture is the ordinary one for a public endpoint:
per-address and per-class rate limits, a global and a per-address lobby cap, an opaque lobby id
distinct from the secret session id, control characters stripped and strings clamped, bounded
bodies and headers, and the forwarded-for header trusted only from a loopback proxy. Both services
admit a connection at accept, before its TLS handshake, into a pool with a total and a per-address
cap, and one deadline bounds the handshake and everything read before the answer (on the relay,
before registration): a source that opens sockets and sends nothing holds a few slots for fifteen
seconds, never the whole pool.

### Direct and brokered

A host is reachable one of two ways (`coop/session/host_mode`). Direct is a listen on a port,
reached over a LAN or a forwarded port; it may be listed or not, and an unlisted direct host
makes no master call at all. Brokered is a peer-to-peer session through NAT traversal, where the
master is the only rendezvous: the signaling relay carries the candidates and the TURN server
relays the media when no direct path opens. The two are not two transports, and neither restricts
who may connect; the boundary a host wants comes from the password and the admission challenge,
which apply to every lane.

A player's `net.ice` says what peers see of their address (`coop/net/ice_policy.h`): `all`, the
default, offers every candidate, and `relay` only the TURN relay's, so a peer sees the relay's
address instead. A relay-only player is refused a direct join, since a direct dial shows the host
their address, and a brokered session with no TURN server; hosting direct still publishes the
host's address, since a direct listen has no candidates to choose. Any other value, and a
`multivoid.ini` that cannot be read (unless the environment twin `VOTVCOOP_NET_ICE` sets a valid
value, which answers first), refuse every brokered session and every direct join rather than fall
back to `all`. Each refusal is decided when the session starts, under its own code
(`MV-J28`..`MV-J30`); a host from the hosting window is told before its world loads.

### The signaling relay

A line protocol over TCP, ported from the transport library's own example. A peer greets the
relay with the shared token and its identity, which is its own public key; the relay answers a
nonce and the peer signs it with the key its identity names, and only a proved identity is
registered. From then on every line names a destination identity and a hex payload, and the
relay forwards it with the sender's identity in front; a duplicate registration evicts the
older connection at once, and a slow destination has a bounded queue that drops rather than
blocks. The mod fails closed on a relay that never challenges it: registering unproved would
reopen exactly what the challenge closes, so a build that requires the challenge is published
only after a drill proves the deployed relay speaks it.

### Self-hosting

Run the two binaries with their environment (the TURN secret, the signaling token, the
signaling and TURN addresses) and a TURN server that shares the secret, then add the master to
`net.masters` as another entry (`Home=host:port`) and pick its tab in the browser. `net.signaling`
names a relay only for a P2P connection made with no master at all; a lobby's own relay always
comes from its master. The rig builds and launches the signaling binary
locally for a scripted run, and serves a synthetic lobby list to the browser in place of a master.

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
| `/v1/thanks` | the client, on opening the browser | the thanks list's text |
| the signaling lines | both peers | the greeting, the challenge and its proof, the forwarded candidates |

## Late join

Not applicable: the servers act before a session exists. A lobby whose host died lingers at
most until its heartbeats lapse.

## Known limits

| Limit | Evidence |
|---|---|
| The signaling leg is plaintext, so an on-path attacker can relay the registration challenge and hold a victim's name; encrypting it is the next transport item | `[V]` `coop/net/signaling_client.h` |
| The masters do not share lobbies: a game is found only on the master it was announced to, and a player sees one master's list at a time | `[V]` `coop/net/master_slots.h`, `coop/net/lobby_client.h` (`RefreshAsync`) |
| The list of official masters is compiled into each build, so a master added later reaches only the builds after it | `[V]` `coop/net/protocol.h` (`kOfficialMasterSlots`) |
| The `http://` downgrade grammar still ships and is queued for removal | `[V]` `coop/net/http_client` |

## Code map

| Concept | Files |
|---|---|
| the services | `server/src/bin/master.rs`, `server/src/bin/signaling.rs`, `server/src/tls.rs`, `server/src/common.rs`, `server/README.md` |
| the mod's master client | `coop/net/master_slots`, `coop/net/lobby_client`, `coop/net/lobby_announcer`, `coop/net/http_client`, `coop/session/session_manager` |
| the rendezvous | `coop/net/signaling_client.h`, `coop/net/ice_config.h`, `coop/net/ice_policy.h`, `coop/session/host_mode` |
| the services | `server/src/bin/master.rs`, `server/src/bin/signaling.rs`, `server/src/tls.rs` |

# GNS forward-compat plan: P2P + master server

**Date:** 2026-05-28
**Scope:** Lock in the design constraints PR-4 (multi-peer) must satisfy so
the eventual P2P + master-server work isn't blocked by today's choices.
**Status:** Forward-looking. Today's mod ships LAN direct-IP only;
P2P and master-server are post-migration milestones.

**See also:** `votv-master-server-mta-adaptation-2026-05-28.md` -- the
MTA `reference/mtasa-blue/Client/core/ServerBrowser/` source is our
adaptation target for the lobby/browser UX side of the master server
(MTA shipped this exact pattern at multi-thousand-server scale for
15+ years; we adapt the SHAPE -- JSON aggregator + tabbed browser +
on-disk cache + redundant masters -- not the wire format).

## Why this matters now

User directive 2026-05-28 (during PR-4 design): "we should implement gns
with PEER-TO-PEER and MASTER SERVER in mind."

The GNS substrate (PR-1..PR-3 done) supports three connection topologies
natively, and PR-4's multi-peer infrastructure is the integration point
that decides whether tomorrow's P2P / master-server work is a clean
add-on or a from-scratch redo:

1. **LAN direct-IP** (today): `ConnectByIPAddress("ip:port")`. Requires
   port forwarding or shared LAN. Fine for friends' coop where host has
   a public IP or both are on the same Wi-Fi.
2. **P2P with custom signaling** (future): `ConnectP2P(identity, signaling)`.
   Uses ICE / STUN / TURN to NAT-traverse between two peers behind
   home routers. Requires an out-of-band signaling channel to exchange
   ICE candidates -- which is one role of the master server.
3. **Valve SDR P2P** (NOT used): would require Steamworks integration
   and Valve relay tokens. Violates RULE 3 (standalone, no external
   runtime); not pursuing.

Today GNS is already compiled with `ENABLE_ICE=ON` (PR-1 CMake), so the
binary is P2P-capable; we just don't drive that path yet.

## What stays the same across topologies

The Session's main code paths are topology-agnostic. Once `HSteamNetConnection`
exists, everything below is identical regardless of origin:

- `SendMessageToConnection` / `SendMessages` with lanes (PR-3) -- works.
- `ReceiveMessagesOnConnection` / `ReceiveMessagesOnPollGroup` -- works.
- `SteamNetConnectionStatusChangedCallback_t` -- fires on every connection.
- `ConfigureConnectionLanes` -- per-connection, works on any.
- `GetConnectionRealTimeStatus` -- works on any.
- `CloseConnection` -- works on any.

This means **PR-4's per-peer storage, PollGroup, fan-out, and per-peer
status callback logic are correct as-is for both LAN-direct and P2P** --
no rework needed when P2P lands.

## What needs to be pluggable

Only two areas change between topologies:

### A. Connection initiation (`Session::Start`)

Today: `CreateListenSocketIP(addr)` (host) / `ConnectByIPAddress(addr)`
(client). Both take a `SteamNetworkingIPAddr` (numeric IPv4 + port).

P2P future: `CreateListenSocketP2P(virtualPort, opts)` (host) /
`ConnectP2P(identity, virtualPort, opts)` (client). Both take a
`SteamNetworkingIdentity` -- an opaque peer ID rather than an IP. The
signaling backend resolves identity to ICE candidates.

**PR-4 design constraint:** the connect/listen path lives in **two
specific branches** of `Session::Start`. P2P adds a sibling branch
(driven by `Config::topology`). Everything else in `Session` is
topology-blind. **No refactor of the multi-peer storage, PollGroup, or
status callback needed.**

### B. Identity advertising (`Config`)

Today's Config:
```cpp
struct Config {
    Role role;
    std::string peerIp;      // LAN direct-IP only
    uint16_t port;
    int sendHz;
};
```

Forward-compatible Config (target shape; not committed in PR-4):
```cpp
struct Config {
    Role role;
    enum class Topology : uint8_t { LanDirect, P2P } topology = Topology::LanDirect;

    // LAN direct (topology == LanDirect):
    std::string peerIp;
    uint16_t port;

    // P2P (topology == P2P):
    std::string peerIdentity;     // string form of SteamNetworkingIdentity
    std::string signalingUrl;     // master-server endpoint that hands out ICE candidates
    std::string signalingAuthToken;  // bearer token issued at lobby join

    int sendHz;
};
```

PR-4 keeps the current Config shape (LAN direct fields only). Adding
the topology field is a one-line + one-branch change at the P2P
landing time -- it does not require PR-4 to anticipate it.

## Master server -- what it actually does

The "master server" is a small custom service (HTTP + WebSocket, run
by us or self-hosted) with two responsibilities:

1. **Lobby browser**: hosts POST `/lobbies` to publish "I'm running a
   session", clients GET `/lobbies` to list public ones. Each lobby
   carries a host identity + opaque metadata (player count, mod
   version, world name).
2. **Signaling relay** (only for P2P topology): forwards ICE
   candidates between two peers that want to connect. Client opens
   WebSocket to `/signal/<sessionId>`, host has a parallel WebSocket
   open, master server pipes JSON messages back and forth. **No game
   traffic flows through the master server** -- only the ~5 ICE
   candidate exchange at connect time.

Master server is **out of scope of the GNS migration** (PR-1..PR-4
deliverables). It is a SEPARATE service. The Session code only
touches it via the `signalingUrl` field above + an interface to
plug in a signaling backend (GNS exposes
`ISteamNetworkingConnectionSignaling` for this; we'd implement it
on top of our master server's WebSocket protocol).

## What we DO NOT do

- **Steam-relay SDR**: requires Steamworks SDK + Valve relay tokens.
  RULE 3 (standalone, no external runtime) and our self-host model
  rule it out. Pursue only if we ever ship via Steam.
- **NAT punch via built-in STUN against Valve's STUN server**: GNS
  has ICE built-in (`ENABLE_ICE=ON`) but the STUN server URL is
  configurable. We'd provide our own STUN URL (or use a public one
  like `stun.l.google.com:19302`) in P2P connect.

## Roadmap (post PR-4)

| Stage | Deliverable | Notes |
| --- | --- | --- |
| PR-5 | Master server v0: lobby browser only | Tiny HTTP service. Hosts POST/GET lobbies. No signaling yet. Mod's UI adds an in-game "browse lobbies" dialog. |
| PR-6 | Master server v1: signaling channel | Add WebSocket signaling for ICE. Implement `ISteamNetworkingConnectionSignaling` against it. Add Config::Topology and the P2P branch in Session::Start. |
| PR-7 | Hardening | Auth tokens, rate-limit, lobby moderation, master-server-side abuse handling. |

Each stage is post-migration (after PR-4 ships). PR-4 itself stays
LAN-direct only; the design notes above are just the guardrails.

## PR-4 design notes inline (the actual constraints PR-4 honors)

1. `Session::Start` keeps `CreateListenSocketIP` / `ConnectByIPAddress`
   today but its structure is "decide topology -> branch -> create
   connection / listen socket -> register with PollGroup / track
   client slot". P2P will add a sibling topology branch only.
2. `peerConns_` array indexed by peer slot (0..kMaxPeers-1) is the
   single source of truth for active connections. Storage is
   topology-blind.
3. `HandleConnStatusChanged` accepts incoming connections via
   `AcceptConnection`. Whether the inbound connection came from a
   LAN client or a P2P-signaled peer, the host path is identical.
4. `PollGroup` on host receives all client messages. Identification
   via `SetConnectionUserData(hConn, peerSlot)` + reading
   `msg->m_nConnUserData` -- O(1) lookup. Works identically for any
   connection origin.
5. Lane configuration (PR-3) is per-connection -- applies to LAN and
   P2P uniformly.

If a future change to Session looks like "and we'll do it differently
for P2P", that's the signal we drifted from this plan and should
restructure to keep topology behind a single Start-branch.

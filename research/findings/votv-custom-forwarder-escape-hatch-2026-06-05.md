# Custom GNS forwarder relay — ESCAPE HATCH design

**Date:** 2026-06-05
**Status:** FALLBACK ONLY. Build this **iff** the WebRTC spike (Stage 0 of the connectivity-ladder plan) fails.
**Parent:** `votv-zero-ports-connectivity-ladder-design-2026-06-05.md` (the primary plan = WebRTC ICE ladder rungs 0-3).
**Origin:** architect pass 2026-06-05 (was only in ephemeral tool-results; persisted here per audit finding G / WP4 "current artifact is authority").

## When this is used (RULE 2: exactly one rung-3 mechanism)

The primary plan's rung 3 (relay fallback) is **coturn TURN**, reached via GNS ICE. If the WebRTC/libwebrtc build (2020 webrtc-lite from source + abseil-MSVC + re-added OpenSSL + `/EH` reconcile) cannot be made to link `/MT` within the 5-day cap, we abandon rungs 1-2 (no WebRTC, no P2P-direct) and implement rung 3 as **this custom app-level forwarder**. We then ship:
- **rung 0** = Direct IP (`Topology::LanDirect`, already shipped) — the no-VPS backup;
- **rung 3** = this forwarder (always-relay, zero-ports, works behind any NAT);
- **no rungs 1-2** (no hole-punch).
The forwarder and coturn are **never built in parallel** — the spike picks one.

## Model

Both HOST and CLIENT call `ConnectByIPAddress(vpsIp, relayPort)` — **outbound only**, so neither player opens an inbound port. The VPS is the only open port (it is our server). A `votv-relay` process on the VPS runs `CreateListenSocketIP`, accepts every peer, pairs them by `sessionId`, and forwards at the **transport** level. The host's existing **game-level** fan-out (`session_relay.cpp`) is unchanged — the forwarder does NOT do game-level fan-out (that would require it to understand `PacketHeader` and duplicate the host's epoch/slot rewrite). Two forwarding rules only:
- packet from `hostConn` → forward to the target client conn(s) (host→client fan-out is already correct on the host);
- packet from `clientConn[i]` → forward to `hostConn` (client→host; the host's `session_relay.cpp` then relays to other clients as today).
Client↔client therefore takes CLIENT → relay → HOST (epoch/slot rewrite here) → relay → OTHER CLIENTS — one extra VPS hop, dominated by internet RTT, fine for coop.

## VPS relay server (new build target `src/relay-server/`)

Separate executable `votv-relay`, NOT part of `votv-coop.dll`, statically links the same vendored GNS + protobuf. Linux binary on the VPS, never shipped.

```cpp
struct RelaySession {
    std::string sessionId, token;           // token = HMAC(secret, sessionId)
    HSteamNetConnection hostConn;
    struct PeerEntry { HSteamNetConnection conn; uint8_t slot; };
    std::array<PeerEntry, kMaxPeers-1> clients;   // slots 1..3
    int clientCount; uint64_t createdMs, lastActivityMs;
};
// sessions_: unordered_map<sessionId, RelaySession>; connToSession_: reverse map.
// HandleConnStatusChanged (accept), DrainMessages (ReceiveMessagesOnPollGroup),
// ForwardPacket, PurgeExpiredSessions (15-min idle; 60s after all drop).
```

**Per-peer mapping decision:** the HOST keeps ONE relay connection (`hRelayConn_`); the forwarder tags each host↔client message with an 8-byte `RelayHeader{targetSlot}`. The host decodes `clientSlot` to route into the right peer state — substituting for `SetConnectionUserData`, which has no accept on the host in this topology. Clients keep their single conn in `peerConns_[0]` unchanged. (This is the topology-minimal choice; it touches `Start()`, the host receive path, and the two `Relay*ToOtherClients` methods only.)

## Wire (protocol v34 — the forwarder DOES need framing, unlike the ICE plan)

```cpp
struct RelayHeader { uint8_t relayMagic; uint8_t targetSlot; uint8_t _pad[6]; }; // 8B
// relayMagic 0xRE = data frame, 0xRC = control frame (distinguished before ParseHeader)
enum class RelayCtrlKind : uint8_t { RegisterHost=1, JoinSession=2, AssignSlot=3,
    PeerJoined=4, PeerLeft=5, SessionFull=6, SessionNotFound=7, AuthFailed=8, Ack=9 };
// RegisterHost/JoinSession bodies carry sessionId[33]+token[65].
```
`kProtocolVersion` → 34. Control frames (`0xRC`) never enter the game `reliableInbox_`; `HandleMessage` checks the first byte before `ParseHeader`.

## DLL changes

`Config{ Topology topology; std::string relayIp; uint16_t relayPort; std::string sessionId, token; }`. `Start()` Relay branch: both roles `ConnectByIPAddress(relay)`; host stores `hRelayConn_` + sends `RegisterHost`; client stores `peerConns_[0]` + sends `JoinSession`. Host receive becomes `ReceiveMessagesOnConnection(hRelayConn_)`; strip `RelayHeader`, dispatch `HandleMessage(clientSlot, …)`. `RelayUnreliableToOtherClients`/`RelayReliableToOtherClients` gain a `topology==Relay` path that prepends `RelayHeader{targetSlot}` and sends once on `hRelayConn_`. Everything else topology-blind.

## Capacity basis (the 1.6 MB/s figure cited by the primary plan §9)

Assumptions: kMaxPeers=4 → 100 users = 25 sessions; 60 Hz pose; ~100 B avg payload + ~32 B GNS overhead = ~132 B/frame; reliable steady-state negligible (~1 KB/s/session, bursting to ~200 KB/s during a connect-snapshot).
- Per session, 4 peers: 4 senders × 60 Hz × 132 B = ~31.7 KB/s in + ~31.7 KB/s out = **~63.4 KB/s bidirectional**.
- 25 sessions: **~1.6 MB/s ≈ 13 Mbps** steady-state (egress-inclusive — the forwarder relays both directions).
- CPU: AES-GCM ~4 GB/s with AES-NI → ~0.04% of a core at 1.6 MB/s. ~6000 msgs/s in, ~9000 out (host fan-out) — a single core does >100k simple socket ops/s. Not CPU-bound.
- FDs/memory: GNS shares internal UDP sockets; ~150 B/session table entry; ~64 KB GNS send buffer/conn (filled only during the connect burst) → ~6.4 MB peak at 100 conns.
- **VPS:** 1 vCPU / 512 MB / 10+ Mbps ($5/mo) with ~10× headroom **on CPU/FD/memory — but bandwidth is the binding constraint** (12.7 Mbps sustained against a 10+ Mbps / ~4.2 TB-mo tier; size the egress quota, not the core). **Note (audit D):** unlike the ICE ladder, the forwarder is **always-relay** — 100% of traffic transits the VPS (there is no rung-2 offload), so the monthly bandwidth quota at 13 Mbps sustained ≈ **~4.2 TB/month** if all 25 sessions run continuously. Size the tier/quota accordingly; this is the price of always-relay and the reason the ICE ladder (mostly-direct) is preferred when buildable.

## Auth / abuse (same hardening as the primary plan section 10)

Token = signed `HMAC(masterSecret, sessionId|exp)`; relay validates before accepting `RegisterHost`/`JoinSession`; `AuthFailed` → close. `kMaxPeers-1` client cap → `SessionFull`. Per-connection token-bucket rate limit (~200 msg/s) → drop+log. IP ban enforced at the relay via `GetConnectionInfo().m_addrRemote` (this topology HAS a real remote IP, unlike P2P). Out-of-band host join-secret recommended (same mechanism as the ladder plan) to gate slots.

## Build stages (forwarder branch)

1. Relay skeleton (`src/relay-server/`, ~500 LOC: GNS listen + session table + opaque forward; CLI sessionId for test). 2. DLL Relay branch (`Topology::Relay`, `hRelayConn_`, control msgs). 3. HTTP master/lobby list (reused from the ladder plan) + `server_browser` wiring. 4. `RelayHeader` framing + slot assignment + 4-peer test. 5. Hardening (token auth, rate-limit, expiry, ban list, redundant master).

## Divergences from the ICE ladder

- **Needs protocol v34** (`RelayHeader` framing in-stream); the ICE plan needs no bump.
- **VPS sees the framing** (8-byte header it adds) but game payload stays GNS-AES-GCM encrypted end-to-end peer↔peer — the forwarder terminates two GNS connections (host↔VPS, client↔VPS) and forwards ciphertext; it does NOT see game plaintext (the earlier "terminates GNS, sees plaintext" worry was about a naive forwarder — this one forwards the encrypted GNS app messages).
- **Always-relay**: no rung-2 direct; every byte transits the VPS. Higher VPS bandwidth, simpler (no WebRTC).
- Touches the host per-peer model more than ICE does (single multiplexed `hRelayConn_` + `RelayHeader` demux vs ICE's real per-peer accepts).

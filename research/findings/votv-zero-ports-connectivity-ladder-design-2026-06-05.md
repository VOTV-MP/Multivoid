# Zero-open-ports connectivity ladder (rung 0-3): design

**Date:** 2026-06-05
**Status:** v3 design + IN IMPLEMENTATION. **Stage 0 (WebRTC build spike — this doc's #1 risk) PASSED** (webrtc-lite builds/links/runs on MSVC 14.50; `USE_STEAMWEBRTC=ON` builds our `votv-coop.dll`, two-abseil collision solved by pinning protobuf 3.21.12). **Stage 2 (the P2P path) DONE + LIVE-VALIDATED:** `Config::Topology{LanDirect,P2P}` + `ice_config` + `signaling_client` (trivial-TCP, ported) + `Session::StartP2P` (CreateListenSocketP2P / ConnectP2PCustomSignaling) connect 2 peers via ICE — **both locally AND over the real internet through a VPS signaling server** (the VPS relayed the ICE rendezvous; the whole coop stack rode the new transport). VPS provisioned in COEXIST mode (signaling tcp/10000 + coturn 3478) on the user's shared Xray box. **NEXT = rung-3 TURN-relay validation (ICE relay-only via coturn) + a true cross-NAT (second-network) test, then the master server / browser.** Details + evidence: `[[project-zero-ports-connectivity-ladder]]`.

> **KNOWN RISK (named 2026-06-05, RULE-1 audit) — `:443` reachability undermines the firewall-friendliness goal on restrictive networks.** The whole point of zero-ports is networks that allow **only :443 outbound**. The current VPS endpoints (signaling :10000, coturn :3478) are **blocked** on exactly those networks — and the validation box (the user's Xray proxy) already has xray on :443, so WS-on-443 can't live there. The production zero-ports endpoint MUST own :443: either a **dedicated box** with the signaling WS on :443, or **front the existing :443** (reverse-proxy / SNI-route `/ws` to the signaling service alongside xray), PLUS **TURN-over-TLS on :443** (coturn supports it) for the CGNAT/symmetric-NAT relay case — otherwise rung 3 is unavailable on the very networks that need it most. This is a deployment/decision constraint, not a code issue; resolve before calling the feature "works on locked-down networks."
**Decision owner:** user (picked WebRTC-ICE ladder + open-port backup, 2026-06-05).
**Supersedes:** `votv-gns-p2p-masterserver-plan-2026-05-28.md` (its STUN-direct/no-relay-fallback assumption was incomplete; lobby-list shape preserved + extended).
**Companion:** `votv-custom-forwarder-escape-hatch-2026-06-05.md` (the fallback if the WebRTC build fails).
**Audited:** 2× multi-agent adversarial passes (build-feasibility, GNS-API correctness, security, RULE/modularity/capacity). Findings are folded into the body below; the residual verdict is in Revision history.

---

## 0. The decision in one paragraph

Players must open **zero inbound ports**, must **not depend on the relay 100%**, and must still play if **the VPS is entirely down**. The only architecture satisfying all three is the **ICE connectivity ladder** GNS implements via its `steamwebrtc` module, plus keeping today's direct-IP topology as a manual backup. ICE auto-selects, in priority order: same-network direct -> STUN-hole-punched peer-to-peer (zero ports, no relay) -> TURN relay (our VPS) only when the NAT defeats hole-punching. Direct-IP (today's `LanDirect`) stays as rung 0 for the VPS-down case. One mechanism (ICE) covers rungs 1-3; one existing topology covers rung 0. **The single real cost — and the project's largest unproven build — is compiling Google `libwebrtc` from source** (it is NOT vendored; only the GNS shim is). We de-risk it with a gated spike before committing (§4), and if it fails we fall back to the always-relay forwarder (§12).

---

## 1. The ladder

```
 Rung 0  DIRECT IP (manual, host forwards a port)   needs NO VPS    -- "VPS-down" backup
            |  existing LanDirect topology: CreateListenSocketIP / ConnectByIPAddress
            |  selected manually in the browser ("Direct IP..." entry)
            v
 Rung 1  ICE host / LAN candidates                  same network, direct
            v
 Rung 2  ICE server-reflexive (STUN hole-punch)     PEER-TO-PEER, zero ports   <- the common case
            v
 Rung 3  ICE relay (TURN = our coturn on the VPS)    fallback when hole-punch fails (NAT-dependent)
```

Rungs 1-3 are **one** `ConnectP2P` call with `ICE_Enable = All`; GNS gathers all candidate types and negotiates the best working path automatically. Verified config knobs in our vendored GNS (`src/votv-coop/third_party/GameNetworkingSockets/include/steam/steamnetworkingtypes.h:1616-1734`):
- `P2P_STUN_ServerList` (103), `P2P_TURN_ServerList` (107) / `TURN_UserList` (108) / `TURN_PassList` (109)
- `P2P_Transport_ICE_Enable` (104) bitmask: `Disable(0) | Relay(1) | Private(2) | Public(4) | All(0x7fffffff)`
- `P2P_Transport_ICE_Implementation` (110) — set explicitly to select the steamwebrtc impl.

**The WebRTC source is NOT vendored — only the shim is.** `src/votv-coop/third_party/GameNetworkingSockets/src/external/steamwebrtc/` (the shim: `ice_session.cpp`, `webrtc_sdp.cc`) + the GNS glue (`steamnetworkingsockets_p2p_webrtc.cpp`) are present. But `src/external/webrtc/` is an **empty, uninitialized git submodule** pointing at `webrtc.googlesource.com/src`, **pinned to a June-2020 (~M85) snapshot**, and `src/external/abseil/` likewise. `USE_STEAMWEBRTC=ON` compiles a curated ~150-file **`webrtc-lite` from that source** — there is no option to link a prebuilt libwebrtc, and modern prebuilts (M119+) are ABI-incompatible with the 2020 shim. ICE is compiled out today (`USE_STEAMWEBRTC=OFF`, `src/votv-coop/CMakeLists.txt`). See §4 for what building it actually entails.

**Why ICE beats the custom forwarder (independent of the user's reasons):** with `CreateListenSocketP2P`, the host *accepts a real `HSteamNetConnection` per client*, exactly like `CreateListenSocketIP` today. So the existing per-peer model — `peerConns_[slot]`, the PollGroup, `SetConnectionUserData` slot tagging, `session_relay.cpp` fan-out, the epoch latch — is **preserved**, with a much smaller diff than the forwarder (which broke that model with a bespoke `RelayHeader` + per-slot demux). The topology-blind invariant (`session.h:17-22`) holds.

---

## 2. Components

```
                         VPS (single box we control; the only open ports are here)
                +--------------------------------------------------------------+
                |  HTTP master/lobby   :443   (POST /v1/host, GET /v1/lobbies, /v1/join, ...)
                |     - issues signed session tokens + short-lived TURN creds    |
                |  WebSocket signaling :443/ws  (ICE candidate/SDP exchange)      |
                |     - routes opaque blobs between the two peers of a session    |
                |     - async, signed-token-gated, reject-duplicate-role          |
                |  coturn (STUN+TURN)  :3478 / relay range                         |
                |     - hole-punch assist (STUN) + relay fallback (TURN)           |
                +--------------------------------------------------------------+
                       ^ outbound only            ^ outbound only
        signaling + TURN creds  |                 |  signaling + TURN creds
                                |                 |
                    +-----------+                 +-----------+
                    |                                          |
              [HOST peer]                              [CLIENT peer 1..3]
              CreateListenSocketP2P(vport)             ConnectP2PCustomSignaling(hostId,vport)
              accepts a real HSteamNetConnection       gets a real HSteamNetConnection
              per client (peerConns_[slot] intact)     in peerConns_[0]
                    \                                          /
                     \________ rung 1/2: direct P2P __________/
                              (game traffic NEVER touches the VPS)
                              rung 3 only: via coturn TURN (encrypted bytes)
```

- **Client DLL** (ships): GNS built `USE_STEAMWEBRTC=ON`; a `ConnectP2P` branch in `Session::Start`; a WebSocket signaling client implementing `ISteamNetworkingConnectionSignaling` + the recv-context; ICE STUN/TURN config from the master. Still a single standalone DLL (RULE 3) — `libwebrtc` statically linked, no runtime dep.
- **VPS** (infra, never ships): HTTP master, async WebSocket signaling, coturn. One small service + coturn.

Game traffic flows through the VPS only on rung 3 (TURN), only for peers that can't hole-punch.

---

## 3. GNS P2P + custom signaling wiring (grounded in the vendored headers/examples)

From `steamnetworkingcustomsignaling.h` + `examples/trivial_signaling_client.{h,cpp}` + `trivial_signaling_server.py`:

1. **Signaling client** (new DLL files): a WebSocket to `wss://<vps>/ws?session=<id>&token=<signed>`. It implements:
   - `ISteamNetworkingConnectionSignaling::SendSignal(hConn, info, pMsg, cbMsg)` — GNS calls this **from any thread**; we forward the opaque blob over the WebSocket to the session peer. The WebSocket write is guarded by a mutex/lock-free queue (the example uses a `recursive_mutex`).
   - `ISteamNetworkingSignalingRecvContext` — inbound blobs from the WebSocket are fed to `SteamNetworkingSockets()->ReceivedP2PCustomSignal(pMsg, cbMsg, &recvContext)`. **(Audit fix)** The global `Callback_CreateConnectionSignaling` factory is a **no-op on the `ConnectP2PCustomSignaling` path — do not install it.**
2. **Host inbound:** the recv-context's `OnConnectRequest(hConn, identityPeer, vport)` returns the per-connection signaling object (slot permitting) — it does **NOT** check the join-secret (its signature carries no app data, and the identity there is an unverified self-asserted string). Acceptance proceeds via the **existing** Connecting status callback (`AcceptConnection` + `SetConnectionUserData(slot)` + PollGroup, unchanged). The join-secret is enforced as a **post-`Connected` app-layer challenge-response before any session state is published** (§10). `SendRejectionSignal` is a no-op (presence-hiding). The accepted `hConn` lands in `peerConns_[slot]` like a LanDirect accept.
3. **Host listen:** `CreateListenSocketP2P(vport=0, nOpts, opts)`; `hPollGroup_` created as in LanDirect.
4. **Client connect:** build a per-connection `ISteamNetworkingConnectionSignaling`, then `ConnectP2PCustomSignaling(pSignaling, &hostId, vport=0, nOpts, opts)`. **(Audit fix)** GNS **takes ownership** of `pSignaling` and `Release()`s it on failure — do not double-Release; handle the `k_HSteamNetConnection_Invalid` return.
5. **Init calls (audit fix):** call `InitAuthentication()` (pre-warm ephemeral cert gen) and `InitRelayNetworkAccess()` (needed for TURN; else multi-second first-relay delay) at P2P init.
6. **ICE config (audit fix):** pass `P2P_Transport_ICE_Enable=All`, `ICE_Implementation`, `STUN/TURN` lists in the **create-time `opts` array** of `CreateListenSocketP2P`/`ConnectP2PCustomSignaling` — NOT post-creation `SetConfigValue` (which races ICE start). `Config`'s `std::string`s outlive the call (they live in `cfg_`).
7. **State (audit fix):** the P2P path adds a `FindingRoute` state between `Connecting` and `Connected`; it passes through the existing status callback harmlessly — do not add a branch, but expect it in logs.

**Identity (audit fix):** `SteamNetworkingIdentity::SetGenericString` caps at **31 chars** (silently false beyond). `hostIdentity`/`sessionId` used as identity must be <=31 chars (e.g. a 20-char base64 token); assert + log if `SetGenericString` returns false. The signaling server routes by session, so identity only needs to be stable within a session.

---

## 4. The long pole: building `libwebrtc` from source — de-risk FIRST (gating)

`USE_STEAMWEBRTC=ON` compiles `webrtc-lite` from the pinned 2020 submodule (audio/video excluded — far smaller than full libwebrtc, and the curated CMake path does NOT need depot_tools/gn/ninja). It is nonetheless the hardest build in the project. Treat it like the GNS plan treated protobuf-static (`votv-gns-integration-plan-2026-05-27` §12): prove it in a sandbox before any integration.

**Three separately-blocking realities (audit):**
- **abseil `lts_2020_02_25`** (pinned) has a documented unresolved MSVC failure on this path (`dllimport`/`ABSL_DLL`, GNS issue #140) — needs an `ABSL_DLL`-neutralizing patch.
- `USE_STEAMWEBRTC=ON` **re-introduces OpenSSL** (WebRTC DTLS is BoringSSL/OpenSSL-only) — the dep we removed via `USE_CRYPTO=BCrypt`. Source a `/MT` x64 static OpenSSL (or switch GNS crypto wholesale) — decided **before** the spike, as it changes the link closure.
- **`/EH` conflict:** GNS core `/EHs-c-` (no exceptions), webrtc-lite *uses* exceptions, our DLL `/EHa` — three models in one image. webrtc-lite TUs need `/EHsc`; the mix must link+run.

**Spike gates, IN ORDER (any hard "no" at (a)-(c) -> fork to the forwarder §12, don't burn the cap):**
- (a) abseil `lts_2020_02_25` compiles `/MT` clean? *(cheapest; the documented failure point — do this first, before pulling the multi-GB webrtc snapshot.)*
- (b) `/MT` x64 static OpenSSL sourced + webrtc `openssl_*.cc` compile?
- (c) `/EHsc` added to webrtc-lite; the 3-EH-model DLL links?
- (d) all ~150 webrtc-lite TUs compile on 2026 MSVC (2020 source)?
- (e) steamwebrtc + GNS static lib links `/MT` into a throwaway exe?
- (f) `example_chat` / `test_p2p` establishes a P2P connection via a local STUN?
- (g) DLL-size delta measured (threshold = acceptable injected-DLL footprint into a UE4 game — tens of MB is fine; do NOT anchor on a guessed 10 MB)?
- (h) game boots clean with the DLL attached, **no session started** (static-init inertness — webrtc/abseil global ctors run at attach even though `GameNetworkingSockets_Init` stays deferred).

**Cap:** 5 days is a tripwire, not a confident estimate (abseil patch + OpenSSL + `/EH` can each eat the week). **Decision:** spike passes -> §13 stages; spike fails -> escape hatch (§12), shipping rung 0 + always-relay forwarder, no rungs 1-2.

---

## 5. Client DLL changes

### 5.1 Config (`include/coop/net/session.h`, replacing the `// Future:` stub at :55)

```cpp
enum class Topology : uint8_t {
    LanDirect,  // rung 0: CreateListenSocketIP / ConnectByIPAddress -- UNCHANGED, kept as backup
    P2P,        // rungs 1-3: CreateListenSocketP2P / ConnectP2PCustomSignaling + ICE
};
struct Config {
    Role role = Role::Host;
    Topology topology = Topology::LanDirect;
    // rung 0:
    std::string peerIp = "127.0.0.1"; uint16_t port = kDefaultPort;
    // rungs 1-3:
    std::string signalingUrl, sessionId /*<=31ch*/, hostIdentity /*<=31ch*/;
    std::string stunList, turnList, turnUser, turnPass;   // turn* = short-lived coturn-REST creds
    std::string joinSecret;       // out-of-band host-issued secret (auth; see s10)
    int sendHz = 60;
};
```

### 5.2 `Session::Start` — a P2P sibling branch (the only structural change)

LanDirect branch unchanged. P2P branch: host `CreateListenSocketP2P` + `CreatePollGroup` (same as LanDirect host); client builds a per-conn signaling object + `ConnectP2PCustomSignaling`, stores handle in `peerConns_[0]`. ICE config in the opts array (§3.6). Everything downstream — accept path, PollGroup receive, `session_relay.cpp` fan-out, lanes, epoch latch, inbox drain — is **unchanged** (real per-peer connections). `Stop()` closes whichever of `hListen_` / `hListenP2P_` is non-zero, then drains as today.

**(Audit fix — modularity):** `session.cpp` is already **856 LOC (past the 800 soft cap)**. Before adding the P2P branch, **extract the connect/listen topology dispatch** (both LanDirect AND P2P branches) into `session_start.cpp` — one relocated dispatcher (RULE-2-clean, not duplication) — so `session.cpp` drops under cap and the P2P branch is additive in the new file. (Stage 1.5 in §13.) The `Start`-branch dispatch alone is only ~40-80 LOC; if extracting just that doesn't clear 800, also move the `Stop`/accept-dispatch helpers into `session_start.cpp` so the green checkpoint (`session.cpp` < 800) is actually met, not assumed.

### 5.3 New DLL files (one feature each, under the 800-LOC soft cap)

- `coop/net/signaling_client.{h,cpp}` (~320 LOC): WebSocket client; implements `ISteamNetworkingConnectionSignaling` + recv-context; thread-safe `SendSignal`; pumps inbound -> `ReceivedP2PCustomSignal`. Ports `trivial_signaling_client.cpp`. **WebSocket transport: WinHTTP** (Win10+, no new dep) if it sustains the duplex `SendSignal` cadence — **verified in Stage 0/1, not Stage 2** (§13); fallback is a header-only static WS client (RULE 3 — no runtime DLL dep).
- `coop/net/ice_config.{h,cpp}` (~120 LOC): builds the GNS `SteamNetworkingConfigValue_t` opts array (ICE_Enable, ICE_Implementation, STUN/TURN) for the create calls.
- `coop/net/lobby_client.{h,cpp}` (~250) + `lobby_announcer.{h,cpp}` (~220): HTTP (WinHTTP) GET list + cache; host POST/heartbeat/leave/visibility. Reused from the architect pass.
- `coop/session_manager.{h,cpp}` (~150): browser -> Config -> `Session::Start` glue.

### 5.4 Protocol version

**No game-protocol bump.** ICE leaves the game wire byte-identical to LanDirect; signaling is an out-of-band WebSocket plane. `protocol.h` (a single-feature constants header, the legit 1500-cap exception) is untouched. (Divergence from the forwarder's v34 framing.)

### 5.5 Per-peer connection type in the player table (user request)

Each scoreboard row shows HOW that player connected. **(Audit fix — 3 reliably-readable states only):**
- **TURN** — `GetConnectionInfo(hConn).m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed (16)`, **sampled only at the Connected state** (it can flap during FindingRoute).
- **Host** — known from `Config` (the host's own row / rung 0).
- **Direct-P2P** — everything else (rungs 1-2). The LAN-vs-hole-punch distinction is NOT reliably readable from `_Fast`/`m_addrRemote` (header says so) — do not attempt it; expose the raw path in the `(?)` tooltip via `GetDetailedConnectionStatus`.

**Authority + flow (star topology):** the **host is authoritative** — it polls `GetConnectionInfo` on each `peerConns_[slot]` (~1 Hz, off the hot path, O(kMaxPeers)) and publishes a per-peer `ConnType` byte in the **existing roster/scoreboard sync** (`roster.h`/`scoreboard.h`). Every peer renders the same host-provided table. UI (`imgui_overlay.cpp` scoreboard surface): a badge per row — `Host` / `Direct` (green) / `TURN` (amber, a visible "slower relay path" cue) — with a `(?)` tooltip (plain-English label; detail in the tooltip, WP10). Cost: one `GetConnectionInfo`/client/sec + one byte per peer broadcast on change. Negligible.

### 5.6 Hide-from-browser toggle (host-controlled lobby visibility; user request)

Use case: host publishes, friends find + join via the browser, then the host hides the lobby so no strangers join. The **session stays live and connected friends are unaffected** — it just drops from the public list. (Resolves the security audit's "unlisted lobbies"; `listed` is distinct from `locked`=password.)

- **UI — host's player-list / scoreboard surface (`imgui_overlay.cpp`):** a host-only checkbox **"Hide from server browser"** (default = listed, so friends can find it first), `(?)` tooltip: "Your game keeps running and connected friends stay in — it just stops showing up for new players browsing."
- **Master — `listed: bool`** (default true) per lobby; `GET /v1/lobbies` returns only `listed==true`. `POST /v1/visibility {lobbyId, token, listed}` (or `listed` in `/v1/heartbeat`) flips it, token-gated to the owning host.
- **Announcer:** `SetListed(bool)` POSTs the change. Hiding must **NOT** call `/v1/leave` (session stays alive) — it only flips `listed`. Re-list by toggling on.
- **Effect:** a hidden lobby exposes no `lobbyId`, so it can't be browsed into; remains joinable only via a direct invite/sessionId the host shares (moot here — friends already in). Optional: also auto-omit when `players_cur==players_max`. Cost: one POST on toggle.

---

## 6. Signaling server (VPS)

An **async** WebSocket service (port the example's logic, but NOT its thread-per-connection model — `ThreadingMixIn` is a C10k DoS on a small box; reimplement async, in the master process). **(Audit fixes baked in):**
- **Auth before per-session state:** require a **signed, single-use, expiring token** (`HMAC(masterSigningSecret, sessionId|role|nonce|exp)`, TTL <=60s, +/-5s skew) presented in the `Sec-WebSocket-Protocol` header or first post-upgrade frame — **NOT the query string** (it leaks to logs); reject on bad MAC / passed `exp` / spent nonce. Validation is **co-process with the master** (shared address space; secret never exported); if ever split, Ed25519 verify-only. (Full token model in §10.)
- **Reject-duplicate-role, never evict the incumbent:** a second socket presenting a host token for a session that already has a live host is **ignored/closed** (the reference's last-writer-wins eviction, lines 113-123, is a takeover bug — invert it).
- **One token, one socket, one session;** clients get a distinct per-peer signed token from `/v1/join`.
- **Per-socket rate-limit** on inbound blobs (N/sec + small burst) + a per-session blob cap; drop + log, never crash.
- Routes opaque blobs between the two endpoints of a pending connection; never parses ICE contents. Drops sockets on session expiry.

Signaling volume is ~a few KB per connection setup, idle thereafter — not a capacity factor.

---

## 7. coturn (VPS)

Off-the-shelf coturn = STUN (rung 2 assist) + TURN (rung 3 relay). **(Audit-hardened config):**
- **`lt-cred-mech` REST / time-limited creds:** TURN `username = "<unixExpiry>:<sessionId>"`, `password = base64(HMAC-SHA1(turnSecret, username))`. The master computes these in `/v1/host` and `/v1/join` responses; coturn validates with `turnSecret`.
- **TTL 60-120s** (NOT 1h — a 1h cred is a 1h open relay = bandwidth theft); the master re-issues on the 30s heartbeat.
- **`max-bps`** (per-allocation, sized to one game stream ~tens of KB/s) + **`bps-capacity`** (whole-server cap = your bandwidth budget) + **`total-quota`/`user-quota`**.
- **Explicit `denied-peer-ip`** for `0.0.0.0/8`, `10/8`, `127/8`, **`169.254/16` (cloud metadata!)**, `172.16/12`, `192.168/16`, `::1`, `fc00::/7`, `fe80::/10`, and the VPS's own subnet (SSRF/metadata-pivot defense); `no-multicast-peers`.
- Bind master+signaling+coturn-admin to `127.0.0.1` (firewalled); disable coturn CLI or strong `cli-password`.
- **`turnSecret` != `masterSigningSecret`** (separate secrets), rotatable (coturn supports overlapping `static-auth-secret`). Never shipped in the DLL — the DLL gets only derived short-lived creds.
- **Verify the SSRF denial (Stage 1 test):** `turnutils_uclient` targeting `169.254.169.254` (and the IPv6 metadata address if the box has IPv6) must be **refused**. Keep the heartbeat (30s) < TTL/2 so live creds refresh before expiry — a network blip forcing a re-`Allocate` after the cred expired would drop the relay session (robustness, not a vuln).

GNS consumes `turn:<vps>:3478` + creds via `P2P_TURN_ServerList/UserList/PassList`.

---

## 8. HTTP master / lobby list

Same shape as MTA's master (announce + fetch + last-seen expiry + on-disk cache + redundant masters; `reference/mtasa-blue/.../CMasterServerAnnouncer.h` + `CServerList.h`). **(Audit-hardened):**

- `POST /v1/host` {name, version, world, locked, players_max} -> {sessionId, hostIdentity, signalingUrl, signed token, stun, turn{user,pass,ttl}}. **Rate-limited per IP** (a few/min); cap live lobbies/IP; global LRU cap. Stamps last_seen.
- `POST /v1/heartbeat` {sessionId, token, players_cur, listed} — 30s cadence; refreshes last_seen + TURN creds + `listed`. Token-gated.
- `POST /v1/leave` {sessionId, token} — immediate removal. Token-gated.
- `POST /v1/visibility` {lobbyId, token, listed} — the hide toggle (§5.6).
- `GET /v1/lobbies?version=` -> array of {**lobbyId** (opaque), name, version, world, locked, players_cur/max, **age** (int seconds since last heartbeat — relative, so the client needs no clock sync; impl chose this over an absolute `last_seen`)} for `listed==true`, filtered by version, expire at 300s. **(Audit fix) The public list carries an opaque `lobbyId`, NOT the raw signaling `sessionId`/token** — the real join capability is minted only in `/v1/join`. Implemented in `tools/coop_master_server.py` (Stage-5 master; built + 35-check smoke `tools/master_smoke.py` 2026-06-05).
- `POST /v1/join` {lobbyId} -> {sessionId, signed per-peer token, signalingUrl, hostIdentity, stun, turn{user,pass,ttl}}. **Rate-limited.**
- **Server-side clamp + strip control chars** on all host-supplied strings (name/world) — a 10 KB `name` is a cheap DoS on every client that fetches.

DLL side: `lobby_client` (browser list + cache to `%APPDATA%\VotVCoop\server_cache.json`), `lobby_announcer` (host lifecycle + `SetListed`). The ImGui `server_browser.cpp` (already built) consumes the list; `DoHost()`/`DoConnect()` -> `session_manager` builds the `Config`. The **"Direct IP..."** entry builds `topology=LanDirect` = rung 0, reachable even when the master is down.

---

## 9. Capacity (>= 100 users)

100 users = ~25 four-player sessions (`kMaxPeers=4`). **(Audit fix — honest, with the pessimistic case):**

- **Rungs 1-2 (peers that hole-punch):** game traffic is P2P-direct; **zero VPS bandwidth.** Only signaling (few KB/connect) + lobby HTTP. Effectively free.
- **Rung 3 (TURN, the peers that can't punch):** TURN relays both directions (egress-billed). Base = the forwarder's all-relay figure, **~1.6 MB/s ≈ 13 Mbps egress-inclusive for all 25 sessions at 100% relay** (derived in the escape-hatch doc). Sensitivity:

  | TURN-fallback rate | VPS relay egress | ~monthly (24/7) |
  |---|---|---|
  | 10% (optimistic) | ~1.3 Mbps | ~0.4 TB |
  | 30% | ~3.9 Mbps | ~1.3 TB |
  | **50%** (mixed mobile/CGNAT) | **~6.5 Mbps** | **~2.1 TB** |
  | **80%** (symmetric-heavy) | **~10.4 Mbps** | **~3.4 TB** |

- **Verdict:** a $5/mo box (1 vCPU / 512 MB-1 GB, coturn+master+signaling) is **comfortable IF TURN stays <=30%.** CGNAT/mobile-heavy populations can push TURN to 50-80%, where the **monthly bandwidth quota** (not CPU/FDs/memory — AES-GCM is ~0.04% of a core at these rates) becomes the first bottleneck and may exceed cheap-tier quotas. **Provision a bandwidth alarm + a documented bigger-tier scale step.** True real-world TURN rate is unmeasured — instrument it in Stage 4.
- **Scale-out (1000+):** coturn shards trivially; the master assigns sessions to STUN/TURN endpoints; signaling is stateless per session. Same shape as Valve SDR relay assignment. Not needed for 100.

---

## 10. Trust / auth / abuse

GNS's open-source build accepts any unsigned cert (`AllowRemoteUnsignedCert()` hard-returns `Allow` — `steamnetworkingsockets_connections.cpp:1806`; no CA, no pinning, and **no application cert-verify callback exists anywhere in the GNS API**), and the GNS crypto handshake (`cert`+`crypt`) rides the rendezvous blob over **our signaling channel**. Two facts drive the model:

**(i) Honest trust statement.** coturn cannot **passively** read traffic (it relays AES-GCM ciphertext on rung 3). But an **active** malicious/compromised VPS that also runs signaling **can MITM** the P2P handshake by swapping keys on each leg, and GNS gives no defense (the cert layer isn't authenticatable; GNS exposes no channel-binding primitive, so even a challenge-response is relay-able by the MITM). **Default posture: players trust the VPS operator.** For a coop mod whose traffic is poses / door states / inventory (no credentials, no real-world value) on a self-hosted or chosen-community box, that is the correct threat model. The design must NOT advertise an end-to-end guarantee it lacks. *(Optional hardening for an untrusted/public shared relay: key an app-layer AEAD with `HKDF(joinSecret, transcript)` over the game stream — a relaying operator who doesn't know the secret then yields undecryptable bytes and the connection drops. Deferred unless a public relay warrants it.)*

**(ii) The join-secret is an APPLICATION-LAYER gate, not a cert hook (audit-corrected — the v2 "authenticates the peer cert" claim was unimplementable).** A per-player secret the host issues and shares out-of-band (in the invite / typed by the joiner), **never seen by the VPS.** It cannot touch the GNS cert (no hook) and cannot be checked in `OnConnectRequest` (its signature carries no app data; the identity there is an unverified self-asserted string). It runs as a **post-`Connected` mutual challenge-response over the established `HSteamNetConnection`, before any session state is published:** on the Connected edge the host sends a nonce, the client returns `HMAC(joinSecret, nonce)`, the host validates and only THEN allocates the slot / publishes the peer to the roster — else `CloseConnection`. This:
  - **gates slot admission** -> defeats slot-theft (only 3 slots/session; a stranger who reaches signaling is held in a no-session-state window and reclaimed on proof-fail, bounded to the proof RTT);
  - **gates rendezvous** (the signaling token below is secret-derived) -> defeats third-party hijack;
  - raises the bar against opportunistic interception, but does NOT by itself cryptographically beat a determined operator-MITM (that needs the optional AEAD above — for coop, trust-the-operator covers it).
  Presence-hiding holds: `OnConnectRequest` returns the signaling object unconditionally (slot permitting); the proof failure closes silently, sharing no session data in the window.

**Signed master tokens.** `HMAC(masterSigningSecret, sessionId|role|nonce|exp)`, validated at signaling + heartbeat/leave/visibility. **Single-use:** the signaling server records spent nonces and refuses re-presentation (one-token-one-socket) — a TTL alone does not bound replay within the window. TTL <=60s for the join/connect token; clock-skew tolerance +/-5s. Carry it in the `Sec-WebSocket-Protocol` header or first post-upgrade frame, **NOT the query string** (query strings leak to proxy/access logs + browser history). **Reject-duplicate-role** (never evict the incumbent host). **master<->signaling boundary: co-process** (one process mints AND validates; the secret never leaves its address space) — decided, not "preferred"; if ever split, switch to Ed25519 (master signs, signaling holds a verify-only public key) so a signaling compromise can't forge tokens.

**Ban filter (audit-corrected).** The existing `AcceptFilterFn` is **IP-based and FAIL-CLOSES on P2P** — `m_addrRemote` is all-zeros at the Connecting edge (`steamnetworkingtypes.h:692`), so the current code would reject EVERY P2P connection. Change `AcceptFilterFn`'s signature to carry `m_identityRemote` and filter by identity for P2P (IP for LanDirect). Note: the P2P identity is forgeable (self-asserted, no CA), so identity-banning is **best-effort deterrence, not enforcement** — durable exclusion relies on the host withholding the join-secret.

**No secret in the shipped DLL** — it holds only short-lived, server-signed, derived creds (the join-secret is host-issued at runtime, never compiled in). The compile-time-secret shortcut is forbidden (a baked secret is public).

**DoS.** Async signaling (not thread-per-conn) + per-IP rate limits on every mutating master endpoint + `/v1/lobbies` from a 5-10s cache + coturn quotas (§7). An outage degrades gracefully to rung 0 (Direct IP). Calibrated to "coop mod," not DDoS-grade.

---

## 11. Host-authoritative model vs P2P — NO conflict

P2P/ICE changes the **transport** (how bytes reach the host), not the **topology** or **authority**:
- **Star, not mesh.** Each client opens ONE connection — to the host — which stays the relay hub (`session_relay.cpp`) and the single authority. Clients never connect to each other (host relays them at the game layer).
- **Star minimizes the P2P surface:** a game needs only `kMaxPeers-1` ICE sessions (all to the host), vs N²/2 for a mesh — far fewer hole-punches/TURN allocations.
- **Only effect:** a **TURN-relayed** client (rung 3) pays the extra VPS hop on host-authoritative round-trips (the known door-latency tradeoff); a **hole-punched** client (rung 2) has no VPS hop and is as good as direct. The per-peer accept model is preserved, so authority + fan-out are untouched.

---

## 12. Escape hatch (only if the §4 spike fails): the custom forwarder

If `libwebrtc` can't be built within the cap, abandon rungs 1-2 (no WebRTC) and implement rung 3 as the **custom GNS forwarder** — both peers `ConnectByIPAddress` outbound to a `votv-relay` process that pairs by sessionId and forwards. Full design (session table, `RelayHeader` framing, per-slot routing, build stages, the 1.6 MB/s capacity derivation): **`votv-custom-forwarder-escape-hatch-2026-06-05.md`**. In that branch: rung 0 (Direct IP) backup + rung 3 (always-relay forwarder), no rungs 1-2. **Exactly one rung-3 mechanism is ever built** (coturn if the spike passes, forwarder if it fails) — the spike is the fork; they are never compiled in parallel (RULE 2).

---

## 13. Reversible build stages

| Stage | Deliverable | Test / gate |
|---|---|---|
| **0. WebRTC spike (gating)** | §4 gates (a)-(h), abseil first | Links `/MT` + a P2P conn establishes + clean attach; else fork to §12 |
| **1. VPS up** | coturn (STUN+TURN, REST creds, hardened §7); async signaling (signed-token, §6); HTTP master (§8) | `turnutils_uclient` relays; signaling echoes a blob with auth; `curl /v1/host` returns signed creds. **WinHTTP-WebSocket viability proven here** (gates Stage 2). |
| **1.5 Extraction** | Move topology dispatch out of the 856-LOC `session.cpp` into `session_start.cpp` | Build green; `session.cpp` under 800; LanDirect byte-identical |
| **2. DLL P2P branch** | `Topology::P2P` + `Start` branch + `signaling_client` (recv-context, ownership, threading) + `ice_config`; `USE_STEAMWEBRTC=ON` | Two DLLs on the SAME LAN connect via rung 1 through real signaling; existing smoke runs over P2P |
| **3. STUN hole-punch (rung 2)** | Reflexive candidates across two real NATs | Two machines behind different home routers connect P2P-direct, zero ports, TURN off |
| **4. TURN fallback (rung 3)** | Force `ICE_Enable=Relay`; **instrument real TURN-fallback rate** (§9) | A peer that can't punch connects via coturn; encrypted relay confirmed; rate logged |
| **5. Lobby/browser + features** | `lobby_client`/`announcer`; `server_browser` DoHost/DoConnect; **§5.5 conn badge** + **§5.6 hide toggle** | Host -> browser -> Connect -> P2P session; badge shows Direct/TURN; hide removes from list; master-down -> Direct IP works |
| **6. Hardening** | Post-`Connected` join-secret proof (§10), single-use signed tokens, reject-dup-role, coturn quotas, rate limits, opaque lobbyId, redundant master, `/healthz` | Forged join held in no-session-state window + reclaimed on proof-fail; replayed token refused; SSRF to 169.254.169.254 refused; TURN cred expiry; brute-force closes socket. (Operator-MITM = trust-the-operator, or test the AEAD relay-MITM if that option is taken.) |

Wall-clock: Stage 0 ~2-5 days (the risk); 1 ~2; 1.5 ~0.5; 2 ~3-4; 3-4 ~2 (real-NAT testing); 5 ~3-4; 6 ~3-4. Dominated by Stage 0 + real-NAT verification. Each stage is independently smoke-testable (the one prior hidden dependency — signaling transport gating Stage 2 — is pulled forward into Stage 1).

---

## 14. Risks, open questions, divergences

**Risks:** (1) libwebrtc-from-source build — the whole gamble; mitigated by §4 gates + escape hatch. HIGHEST. (2) DLL size — measure in Stage 0. (3) GNS internal/webrtc threads + proxy-DLL load order — keep `GameNetworkingSockets_Init` deferred; verify attach-inertness (§4h). (4) Real-NAT TURN rate higher than hoped -> capacity (§9); instrument in Stage 4. (5) WinHTTP-WebSocket duplex cadence -> resolved in Stage 1, header-only fallback (RULE 3).

**Open questions (verify before/while implementing):** (1) `/MT` OpenSSL + abseil-patch specifics for the 2020 snapshot. (2) WinHTTP-WebSocket suffices for `SendSignal`? (3) exact `SteamNetworkingConfigValue_t` opts-array form for ICE/STUN/TURN. (4) coturn REST cred TTL vs ICE allocation refresh timing.

**Divergences:** From the 2026-05-28 plan — add the TURN relay floor (hole-punch isn't guaranteed) + correct its stale "ENABLE_ICE=ON" (live CMake is OFF). From the forwarder pass — no `RelayHeader`/v34, no multiplexed conn, `peerConns_`/PollGroup/accept unchanged, encrypted (not plaintext) relay; forwarder demoted to escape hatch. From MTA — no ASE UDP per-server query (measure ping post-connect via `GetConnectionRealTimeStatus`); ImGui not CEGUI; no anti-cheat/serials. Keep MTA's announce/fetch/cache/redundant-master shape.

---

## 15. New / changed files

**VPS infra (never ships):** async signaling server, coturn config, HTTP master (`master_server`), systemd units, `tools/deploy-relay.sh`.
**DLL new:** `coop/net/signaling_client.{h,cpp}`, `coop/net/ice_config.{h,cpp}`, `coop/net/lobby_client.{h,cpp}`, `coop/net/lobby_announcer.{h,cpp}`, `coop/session_manager.{h,cpp}`, `coop/net/session_start.{h,cpp}` (extracted dispatcher).
**DLL changed:** `include/coop/net/session.h` (Topology + Config + `hListenP2P_`), `src/coop/net/session.cpp` (P2P branch moves to session_start; Stop covers P2P listen), `src/coop/net/session_status.cpp` (recv-context accept via join-secret + identity-based ban filter), `src/coop/net/session_relay.cpp` (unchanged — fan-out works on P2P conns), `src/ui/imgui_overlay.cpp` (§5.5 badge + §5.6 hide checkbox), `src/ui/server_browser.cpp` (DoHost/DoConnect -> session_manager), `roster.h`/`scoreboard.h` (+1-byte ConnType), `CMakeLists.txt` (`USE_STEAMWEBRTC=ON`, OpenSSL, link libwebrtc, new sources).
**No `protocol.h` version bump.**

---

## Revision history

- **v3 (2026-06-05):** Second adversarial audit pass (3 agents). **Internal consistency + completeness + RULE/capacity all passed clean** (no leftover v1/v2 contradictions; all 7 user asks present). One blocking correction: the v2 "join-secret authenticates the peer cert" was proven unimplementable — open-source GNS has no cert-verify hook (`AllowRemoteUnsignedCert`=Allow, no callback) and `OnConnectRequest` carries no app data. §10 rewritten: the join-secret is a **post-`Connected` app-layer challenge-response** (slot/rendezvous gate); operator-MITM is **trust-the-operator by default** (honest for coop) with an optional `HKDF(joinSecret,transcript)` AEAD for an untrusted public relay. Also: `OnConnectRequest` returns the signaling object unconditionally and the secret-proof gates slot allocation (§3.2); ban filter MUST move to identity (IP fail-closes all P2P) and is best-effort (identity forgeable); tokens single-use + out-of-query-string + co-process (§6/§10); coturn SSRF-deny verified in Stage 1 + heartbeat<TTL/2 (§7); Stage-1.5 extraction quantified; escape-hatch headroom scoped to CPU-not-bandwidth.
- **v2 (2026-06-05):** Folded in two adversarial audit passes (4 agents). Material changes: WebRTC is **from-source** webrtc-lite (not prebuilt) with abseil/OpenSSL/`/EH` day-1 gates (§4); security model corrected — honest MITM statement + the out-of-band **join-secret** that closes MITM/hijack/slot-theft, signed tokens, coturn TTL/`max-bps`/`denied-peer-ip`, opaque `lobbyId` (§6/7/8/10); GNS-API specifics — recv-context, no global factory, signaling ownership/threading, `SetGenericString` 31-char, `InitAuthentication`/`InitRelayNetworkAccess`, ICE-config-in-opts, **ban filter by identity not IP** (§3/§10); connection badge simplified to Host/Direct/TURN (§5.5); capacity sensitivity table + "comfortable if TURN <=30%" caveat (§9); `session.cpp` extraction-first (§5.2/Stage 1.5). Added §5.5 (conn-type badge), §5.6 (hide-from-browser), §11 (host-auth vs P2P). Escape hatch persisted to its own doc (§12).
- **v1 (2026-06-05):** initial ICE-ladder design.

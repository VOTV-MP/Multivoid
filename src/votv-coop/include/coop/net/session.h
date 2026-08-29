// coop/net/session.h -- the networking application layer (PR-4 multi-peer edition).
//
// "A session is a host listening on a port + zero..kMaxPeers-1 clients connected."
// Session owns the GameNetworkingSockets connection(s) and runs a dedicated net
// thread that drives RunCallbacks + ReceiveMessagesOnPollGroup (host) /
// ReceiveMessagesOnConnection (client). Game thread <-> net thread bridge is
// pose slots + a reliable inbox.
//
// PR-2 swapped the hand-rolled Winsock UDP for GNS. PR-3 added 3 priority lanes.
// PR-4 (this file) generalizes to kMaxPeers connections via a PollGroup on
// host. Per-peer remote pose storage is indexed by `peerSlot` matching
// coop::players::Registry's indexing (slot 0 = host, slots 1..kMaxPeers-1 =
// clients). On host, slot 0 is unused for remote state (it's local); slots
// 1..3 hold remote-client poses. On client, slot 0 holds the host's pose;
// slots 1..3 unused (host-authoritative model -- clients only talk to host).
//
// Topology-blind: per the forward-compat plan
// (research/findings/network/votv-gns-p2p-masterserver-plan-2026-05-28.md), every code
// path below is topology-agnostic except Session::Start. P2P + master server
// will add a sibling branch in Start; the multi-peer storage / PollGroup /
// status callback / lanes / receive loop all work unchanged for any
// HSteamNetConnection origin (LAN direct, ICE-signaled P2P, future SDR).

#pragma once

#include "coop/net/link_kind.h"            // how a player's traffic reaches the session
#include "coop/net/net_stats.h"            // session traffic accounting (the one counter owner)
#include "coop/net/protocol.h"
#include "coop/net/send_backlog.h"         // R-4b: the reliable-send delivery guarantee
#include "coop/player/players_registry.h"  // kMaxPeers (host + 3 clients = 4)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace coop::net {

// kMaxPeers (host + 3 clients = 4) is canonically defined in
// coop::players::kMaxPeers. Alias it into coop::net so this header's
// std::array<T, kMaxPeers> sizes resolve without coop::players:: prefixes
// at every use site, and so any future bump to kMaxPeers cascades here
// automatically.
inline constexpr uint8_t kMaxPeers = coop::players::kMaxPeers;

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

// Transport topology. The whole session below is topology-blind EXCEPT
// Session::Start (see the header comment) -- every HSteamNetConnection, once it
// exists, is driven identically regardless of how it was established.
enum class Topology : uint8_t {
    LanDirect,  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress. Host
                // port-forwards (rung 0) or both peers are on one LAN (rung 1).
                // The original path -- kept as the no-signaling backup.
    P2P,        // rungs 1-3: CreateListenSocketP2P / ConnectP2PCustomSignaling +
                // ICE. Both peers connect OUTBOUND to a signaling server (no host
                // port-forward); ICE then hole-punches a direct path (rung 2,
                // STUN) or relays via TURN (rung 3, coturn).
};

struct Config {
    Role role = Role::Host;
    Topology topology = Topology::LanDirect;

    // --- LanDirect (rung 0/1) ---------------------------------------------
    std::string peerIp = "127.0.0.1";   // client: the host's address
    uint16_t port = kDefaultPort;

    // --- P2P (rungs 1-3) --------------------------------------------------
    // Signaling rendezvous: "host:port" of the signaling server (the VPS, or a
    // local test server). Both peers connect OUTBOUND -- no host port-forward.
    std::string signalingUrl;
    // Shared bearer token the signaling server requires in the greeting (gates
    // the open internet out of the rendezvous channel). Distributed with the
    // lobby; master-server-issued per-session tokens come later (Stage 6).
    std::string signalingToken;
    // There is NO `localIdentity` field any more (RULE 2, 2026-08-29). This peer's
    // own signaling identity IS its durable public key -- one value, installed
    // process-wide by peer_identity::InstallInto before any socket exists, read
    // back with peer_identity::LocalIdentityString(). A config-carried second
    // identity was how the master's per-session mint came to overwrite the durable
    // one on the P2P lane; see that function's comment.
    //
    // The HOST's identity the client dials, as the host published it to the master
    // (`gen:<64 hex>`). Client-only. Contract: it must equal the host's
    // peer_identity::LocalIdentityString(), which is now a fact about the host's
    // key rather than a convention two config files have to agree on.
    std::string hostIdentity;
    // ICE candidate sources. stunList = rung 2 (hole-punch); turn* = rung 3
    // (coturn relay, short-lived REST creds). Empty string disables that rung.
    std::string stunList;    // "host:port,host2:port"
    std::string turnList;    // "turn:host:port,..."
    std::string turnUser;    // parallel to turnList
    std::string turnPass;    // parallel to turnList
    // ICE candidate policy: "" / "all" (default, share host+reflexive+relay),
    // "relay" (force the TURN relay path -- privacy, or to validate coturn),
    // "disable" (no ICE), "default" (leave GNS's default). Mapped to IceEnable
    // in StartP2P.
    std::string iceMode;
    // sessionId / joinSecret (per-lobby uniqueness + the app-layer auth
    // challenge) arrive with the master-server + auth stage -- not needed for
    // the raw ICE transport. See the connectivity-ladder design doc s10.

    // LAN-ONLY host mode (2026-08-29, user: the host picks a connection type
    // and "LAN makes the game LAN-ONLY"). Host-side, LanDirect topology only:
    // the session never touched the master (session_manager skips the announce
    // whole), and the accept edge refuses any remote address that is not
    // loopback / RFC1918 / link-local -- so a forwarded port cannot quietly
    // turn a LAN party into an internet host.
    bool lanOnly = false;

    int sendHz = 60;
};

// Forward decl: Session holds a SignalingClient (P2P only). Defined in
// coop/net/signaling_client.h, which pulls in the GNS public API -- kept out of
// this header so the broad set of session.h includers don't see GNS. Held via
// shared_ptr: per-connection signaling objects co-own the transport, and
// shared_ptr's type-erased deleter means this incomplete-type member needs no
// special handling in the (header-inline) default ctor / out-of-line dtor.
class SignalingClient;

class Session {
public:
    // Fixed-size inline payload (no heap alloc on net thread per receive).
    // senderPeerSlot is the coop::players::Registry slot of the peer that
    // originated this message (-1 if unknown). Drainers route per-sender
    // (e.g. respond TeleportClient back to the requester) via this field.
    struct ReliableMessage {
        ReliableKind kind;
        int senderPeerSlot = -1;
        uint16_t payloadLen = 0;
        uint8_t payload[kMaxReliablePayload];
    };

    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Start(const Config& cfg);
    void Stop();

    bool running() const { return running_.load(); }
    // Aggregate connection state (any peer connected -> Connected).
    ConnState state() const { return state_.load(); }
    bool connected() const { return state_.load() == ConnState::Connected; }
    Role role() const { return cfg_.role; }

    // Game thread: publish our local player's pose -- net thread fan-outs to all
    // connected peers each sendHz tick.
    void SetLocalPose(const PoseSnapshot& pose);
    void SetLocalPropPose(bool set, const PropPoseSnapshot& pose);
    // v22: publish our local ragdoll's pelvis physics (transform + velocity) while
    // we are ragdolling. `set` is true each sendHz frame the local player is
    // ragdolled, false on the recover edge (mirrors SetLocalPropPose's held/release
    // gate). Net thread fan-outs to all peers only while set.
    void SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose);
    // v109: publish the local R-HOLD hand item's live view-relative transform.
    // `set` true each game tick WHILE holding (measured fresh), false on the
    // hand-empty edge -- the SetLocalPropPose held/release gate shape. Net
    // thread fan-outs a HandPose datagram at sendHz only while set.
    void SetLocalHandPose(bool set, const HandPoseSnapshot& pose);

    // v109: publish the local coords-panel LIVE cursor (viewCoordinate). `set`
    // true each pump tick WHILE the desk is claimed + the cursor moved, false on
    // the release / still edge -- same held/release gate shape as SetLocalHandPose.
    // Net thread fan-outs a DeskCursorPose datagram at sendHz only while set.
    void SetLocalDeskCursor(bool set, const DeskCursorPoseSnapshot& pose);

    // v37: HOST publishes the current NPC pose batch (one EntityPoseSnapshot per live NPC);
    // the net thread fan-outs ONE EntityPose datagram to all peers each sendHz tick. Called
    // every game tick by npc_sync::TickPoseStream with the current set; an EMPTY batch clears
    // it (NPCs gone -> stop sending). Game thread. Takes the batch by const-ref + COPIES into
    // localNpcBatch_ (reusing its capacity) so the caller can hand a reused scratch vector --
    // no per-tick heap alloc on either side.
    void SetLocalNpcPoseBatch(const std::vector<EntityPoseSnapshot>& batch);

    // v80 (B3b): HOST publishes the current WorldActor pose batch (one WorldActorPoseSnapshot per live
    // non-Character event actor); the net thread fan-outs ONE WorldActorPose datagram to all peers each
    // sendHz tick. Sibling of SetLocalNpcPoseBatch -- same copy-into-localWorldActorBatch_ shape (no
    // per-tick heap alloc), an EMPTY batch clears it (actors gone -> stop sending). Game thread.
    void SetLocalWorldActorPoseBatch(const std::vector<WorldActorPoseSnapshot>& batch);

    // v85 (Increment 2): HOST publishes the carried-trash-clump pose batch (one TrashClumpPoseSnapshot
    // per host-driven client-grabbed clump -- carry AND throw-flight). The net thread fan-outs ONE
    // TrashCarryPose datagram to all peers each sendHz tick. Sibling of SetLocalWorldActorPoseBatch; an
    // EMPTY batch clears it (no clump carried -> stop sending). Game thread.
    void SetLocalTrashCarryBatch(const std::vector<TrashClumpPoseSnapshot>& batch);

    // Per-peer accessors. peerSlot is the coop::players::Registry slot
    // (0 = host, 1..kMaxPeers-1 = clients). Returns false if peerSlot is
    // out of range, that slot has no remote pose yet, or aggregate state
    // is not Connected.
    bool TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew = nullptr);
    bool TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew = nullptr);
    // v22: per-peer ragdoll pelvis physics. Returns false unless that slot has a
    // fresh ragdoll pose AND aggregate state is Connected. outIsNew distinguishes a
    // newly-arrived packet (apply the velocity) from a re-read of the last one.
    bool TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew = nullptr);
    // v109: per-peer hand-item view-relative transform (hand_item::TickMirrors
    // consumes; newest-wins). Same contract shape as TryGetRemoteRagdollPose.
    bool TryGetRemoteHandPose(int peerSlot, HandPoseSnapshot& out, bool* outIsNew = nullptr);
    // v109: per-peer coords-panel live cursor (desk_cursor_sync consumes; newest-wins).
    // Same contract shape as TryGetRemoteHandPose.
    bool TryGetRemoteDeskCursor(int peerSlot, DeskCursorPoseSnapshot& out, bool* outIsNew = nullptr);

    // v109 (design F): HOST publishes the current world-clock snapshot each game tick
    // (time_sync::Tick); the net thread fan-outs ONE unreliable ClockPose datagram to all
    // peers on a ~500 ms throttle (independent of the pose sendHz). `set` false clears it.
    // Host-only producer; a client call is a harmless no-op store. Game thread.
    void SetHostClock(bool set, const TimeSyncPayload& clock);
    // v109 (design F, CLIENT game thread): move out the latest received host clock + report
    // whether it is newly arrived (apply only on isNew -- between arrivals the frozen mirror
    // holds). Returns false until the first ClockPose lands (the reliable connect-edge
    // TimeSync seeds the initial value before then). newest-wins by header seq.
    bool TryGetHostClock(TimeSyncPayload& out, bool* outIsNew = nullptr);

    // v111: HOST publishes the download-SIM output vector each desk_sim_sync::Tick; the net thread
    // fan-outs ONE unreliable DeskSimPose datagram to all peers on a ~100 ms (10 Hz) throttle. `set`
    // false clears it. Host-only producer; a client call is a harmless no-op store. Game thread.
    void SetHostDeskSim(bool set, const DeskSimSnapshot& sim);
    // v111 (CLIENT game thread): move out the latest received host download-sim vector + report
    // whether it newly arrived (apply on isNew; interpolate between). Returns false until the first
    // DeskSimPose lands. newest-wins by header seq.
    bool TryGetHostDeskSim(DeskSimSnapshot& out, bool* outIsNew = nullptr);

    // v113 (L4): HOST publishes ONE dish-pose row batch (the 4 Hz movers sweep or a
    // settle-tail full-24 sweep -- the GAME-THREAD sweep owns the cadence); the net
    // thread fan-outs ONE unreliable DishPose datagram per publish (dirty one-shot,
    // not interval-driven). Host-only producer; a client call is a harmless no-op.
    void SetHostDishPose(const DishPoseBody& body);
    // v113 (CLIENT game thread): move out the latest received dish-pose batch +
    // report whether it newly arrived (apply on isNew). newest-wins by header seq.
    bool TryGetHostDishPose(DishPoseBody& out, bool* outIsNew = nullptr);

    // v114 (L7): HOST publishes the tape-caddy reel corrector (the GT sweep owns the ~1 Hz
    // cadence; dirty one-shot -- the net thread sends ONE unreliable ReelPose datagram per
    // publish). Host-only producer; a client call is a harmless no-op.
    void SetHostReelPose(const ReelPosePayload& body);
    // v114 (CLIENT game thread): latest received reel corrector + isNew (newest-wins by header seq).
    bool TryGetHostReelPose(ReelPosePayload& out, bool* outIsNew = nullptr);

    // v37 (CLIENT game thread): move out the latest received NPC pose batch + clear the new-data
    // flag (consume-once -- a tick with no new batch returns false, the interp Tick covers between-
    // packet motion). Returns false if no new batch since the last take. Net thread fills it.
    bool TakeRemoteNpcBatch(std::vector<EntityPoseSnapshot>& out);

    // v80 (B3b, CLIENT game thread): move out the latest received WorldActor pose batch + clear the
    // new-data flag (consume-once, same as TakeRemoteNpcBatch). Returns false if no new batch since
    // the last take. Net thread fills it.
    bool TakeRemoteWorldActorBatch(std::vector<WorldActorPoseSnapshot>& out);

    // v85 (Increment 2, CLIENT game thread): move out the latest received trash-clump carry batch +
    // clear the new-data flag (consume-once, same as TakeRemoteWorldActorBatch). Net thread fills it.
    bool TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out);

    // Game thread: queue a reliable message. Host fan-outs to all connected
    // clients; client sends to host. Returns false on payload-too-large or
    // when no peers are connected.
    bool SendReliable(ReliableKind kind, const void* payload, int len);

    // Single-target reliable send. peerSlot is the coop::players::
    // Registry slot index (host=0, clients 1..kMaxPeers-1). Returns
    // false if peerSlot is out of range, the slot is not connected, or
    // the payload doesn't fit. Used by AssignPeerSlot (host -> specific
    // newly-connected client) and per-slot connect-edge replay (each
    // late-joiner gets caught up without re-broadcasting to peers that
    // already have the state).
    //
    // `senderSlot` (v18 host-relay): the LOGICAL origin slot stamped into
    // the header. Defaults to 0 (the sender's own identity -- correct for
    // host->client AssignPeerSlot/Join/own-state replay). The late-joiner
    // PEER-state replay (T2-4) passes the existing peer's slot so the new
    // client routes the replayed action to that peer's puppet (and the
    // receiver's eid-range trust check sees the right role).
    // R-4b DELIVERY CONTRACT: true = the message WILL be delivered -- it entered
    // the GNS stream or the session's send backlog (drained on the net thread;
    // queued-until-sent or connection-fatal, never silently dropped). false =
    // it never will be: pre-world gate, dead/dying slot, or invalid length.
    // The save-stream family (SaveTransferBegin/Chunk) is the ONE exemption and
    // is routed to TrySendReliableToSlot below (its pump owns pacing + retry;
    // mixing it with the backlog would let a bypassing chunk overtake a queued
    // Begin in the same lane).
    bool SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                            int len, uint8_t senderSlot = 0);

    // The PACING lane (R-4b D2): one direct GNS attempt, NO backlog. Returns
    // false on send-buffer backpressure (rc=-25) -- that false IS the caller's
    // pacing signal (retry next tick). Sole intended caller: the save-transfer
    // pump (Begin + Chunk, both success-gated there). Everything else uses
    // SendReliableToSlot's guarantee.
    bool TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                               int len, uint8_t senderSlot = 0);

    // v56 save-transfer bulk sink: SaveTransferChunk payloads (~56KB, far over the
    // fixed inbox slot) are handed to this callback on the NET THREAD instead of
    // entering the ReliableMessage ring. coop/save_transfer registers its heap
    // assembler at Install; the sink must be thread-safe and never touch the
    // engine. Keeps the net core feature-agnostic (principle 7 layering).
    using BulkSinkFn = void (*)(int senderPeerSlot, const uint8_t* data, int len);
    void SetBulkSink(BulkSinkFn sink) { bulkSink_.store(sink, std::memory_order_release); }

    // SECURITY (W3, docs/security/PLAN_02_WIRE_HARDENING.md): SaveTransferBegin is diverted to the
    // net thread for the SAME reason as the chunks, and it must be the SAME thread as the chunks.
    // Begin fits the inbox, so it used to be drained on the GAME thread while chunks landed on the
    // net thread -- and that lag, not any wire reordering, is what let bytes accumulate with no
    // announced size. Latching it here puts announce and payload on one in-order lane on one thread,
    // so Begin is always processed before chunk 0 and "chunks with no Begin" becomes rejectable
    // outright instead of needing a guessed pre-Begin buffer cap. Same contract as the bulk sink:
    // thread-safe, never touches the engine.
    void SetSaveBeginSink(BulkSinkFn sink) { saveBeginSink_.store(sink, std::memory_order_release); }

    // v56 per-slot world-ready send gate (host side): until event_feed marks a
    // joining slot world-ready (ClientWorldReady), the send paths will drop
    // world-mutating kinds to it (IsPreWorldSendableKind allowlist in
    // session_lanes.h) -- the world-ready connect replay reconstructs that state
    // by design. STAGE-1 NOTE (2026-06-10): accessors land with the dormant
    // save_transfer module; the send-path gate + the net_pump/event_feed callers
    // wire in stage 2 together (no half-armed gate that would drop traffic for
    // the current always-in-world flows).
    void MarkSlotWorldReady(int peerSlot, bool ready) {
        if (peerSlot >= 0 && peerSlot < kMaxPeers)
            slotWorldReady_[peerSlot].store(ready, std::memory_order_release);
    }
    bool IsSlotWorldReady(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers &&
               slotWorldReady_[peerSlot].load(std::memory_order_acquire);
    }
    // Seeds arc: net-thread view of the same event (relay-eligible since the
    // ClientWorldReady RECEIPT for exactly this hConn -- see relayEligible_).
    bool IsRelayEligible(int peerSlot, uint32_t hConn) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && hConn != 0 &&
               relayEligible_[peerSlot].load(std::memory_order_acquire) == hConn;
    }

    // Seeds arc: is a remote connection registered in this slot (any state)?
    bool HasPeerConn(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && peerConns_[peerSlot].load() != 0;
    }

    // Seeds arc: does ANY live remote peer pass the world-ready send gate? A
    // fan-out that "failed" with zero eligible receivers is a VACUOUS success
    // for a save+seed-covered lane (absent peers are owed save+seed, not wire)
    // -- without this test the retry-until-heard idiom re-broadcasts a row a
    // future joiner already has in its save (the pre-existing duplicate,
    // design doc par.2.6). Game or net thread.
    bool AnyWorldReadyPeer() const {
        for (int i = 0; i < kMaxPeers; ++i)
            if (peerConns_[i].load() != 0 &&
                slotWorldReady_[i].load(std::memory_order_acquire)) return true;
        return false;
    }

    // Seeds arc: a receive-side lane park at its flood bound flips this to
    // pause the client's inbox drain (pause-not-drop; see NetThread). REFCOUNTED
    // (audit F-1): each lane edge-latches locally and contributes +1/-1 here, so
    // one lane draining cannot cancel another lane's still-standing escalation.
    // Callers MUST call only on their own local edge (the lanes' g_parkBackpressure
    // latches guarantee that).
    void SetApplyBackpressure(bool on) {
        applyBackpressureCount_.fetch_add(on ? 1 : -1, std::memory_order_acq_rel);
    }

    // Game thread: pop a delivered reliable message. Inbox is shared across
    // peers (FIFO of arrivals); the kind-typed payload tells the drainer what
    // happened, not who sent it. Future work could add a sender peerSlot to
    // ReliableMessage if per-sender routing is needed.
    bool TryGetReliable(ReliableMessage& out);

    // v66 voice: send one VoiceFrame datagram (kVoiceFrameHeadBytes +
    // frame.opusLen body bytes) to every world-ready peer (client: the host).
    // Game thread (the GNS send API is thread-safe -- SendReliable's calling
    // convention). Fire-and-forget: per-frame send failures drop silently,
    // the receiver's jitter buffer + PLC cover gaps.
    bool SendVoiceFrame(const VoiceFramePayload& frame);

    // v66 voice inbox: one received frame. A per-sender FIFO STREAM, not the
    // newest-wins pose model -- every arrival is queued; ordering and loss
    // live in the per-payload voice seq handled by the jitter buffer.
    // senderSlot is the relay-rewritten logical origin.
    struct VoiceFrameMsg {
        int8_t senderSlot = -1;
        VoiceFramePayload frame{};
    };
    // Ring depth per sender slot (~320 ms of one speaker). Public: it sizes the
    // caller's drain buffer (kMaxPeers * kVoiceRingPerSlot covers a full inbox).
    static constexpr int kVoiceRingPerSlot = 16;
    // Drain every queued voice frame in one call (game thread; ONE lock per
    // tick -- audit I-3/M-3). Returns the count written to out[0..maxCount).
    int DrainVoiceFrames(VoiceFrameMsg* out, int maxCount);

    bool SendPropRelease(const WireKey& key,
                         float linVelX, float linVelY, float linVelZ,
                         float angVelX, float angVelY, float angVelZ,
                         uint32_t elementId = 0,  // v82: trash-entity eid (0 = keyed Aprop, routed by key)
                         uint8_t ctx = 0);        // v82: trash-entity sync-time-context (0 = non-trash, no enforcement)
    bool SendPropSpawn(const PropSpawnPayload& payload);
    bool SendPropDestroy(const PropDestroyPayload& payload);
    bool SendEntitySpawn(const EntitySpawnPayload& payload);
    bool SendEntityDestroy(uint32_t elementId);

    // Diagnostics. The counters themselves live in coop::net::net_stats (the one
    // owner -- bytes + packets, counted at the GNS choke points; the ui net-stats
    // panel reads the same source). These delegates keep the existing callers.
    uint64_t packetsSent() const { return net_stats::PacketsSent(); }
    uint64_t packetsRecv() const { return net_stats::PacketsRecv(); }
    // Per-slot RTT in ms (the GNS link ping to peer `slot`), or -1 if that slot has
    // no live connection / not yet sampled. Sampled ~1 Hz on the net thread. 0 is a
    // real value on LAN (sub-millisecond RTT).
    //
    // ONE CONSUMER, and keep it that way (v131): roster_ledger::RefreshLinkFacts,
    // which publishes the value on RosterRow so every board reads the HOST's
    // measurement. Both previous readers are gone -- the nameplate's per-tick
    // fan-out through RemotePlayer::SetPing and roster::Refresh's per-row read --
    // because a client owns only peerConns_[0] and so could only ever measure its
    // link to the host, which is a different question from "how is this player
    // connected to the session". A new caller here is a second derivation.
    int rttMsForSlot(int slot) const {
        return (slot >= 0 && slot < kMaxPeers) ? rttMsBySlot_[slot].load() : -1;
    }
    // How `peerSlot`'s LIVE connection carries that player's traffic, MEASURED
    // from the connection itself: the GNS relay flag and the remote address.
    // Never derived from cfg_.topology -- that is how the connection was
    // ESTABLISHED, not how the peer is connected, and asserting it labelled a
    // port-forwarded WAN peer "LAN". LinkKind::Unknown without a live conn.
    //
    // The HOST calls this; the answer is published on RosterRow so every board
    // renders the same value for the same player (v131). Any thread (the GNS API
    // is thread-safe), but see roster_ledger::RefreshLinkFacts for the cadence --
    // GetConnectionInfo takes a GNS lock, so this is NOT a per-tick call.
    LinkKind LinkKindForSlot(int peerSlot) const;
    // Count of currently-connected peers (0..kMaxPeers-1).
    int connectedPeerCount() const;
    // True if the given slot has an active GNS connection (handle set).
    // Used by the harness for per-slot connect/disconnect edge detection.
    // NOT a "ready for app traffic" signal -- see IsSlotReady below.
    bool IsSlotConnected(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerConns_[peerSlot].load() != 0;
    }

    // True only after the slot's GNS Connected callback ran AND
    // ConfigureLanesForPeer succeeded. IsSlotConnected flips true in
    // the earlier Connecting callback (peerConns_[slot] is needed there
    // for host AcceptConnection routing) -- using it as the gate for
    // app-traffic sends would queue messages on the default lane 0
    // because the per-kind lane mapping hasn't been applied to the
    // connection yet. Snapshot drain and connect-edge replay must
    // gate on IsSlotReady, not IsSlotConnected, so PR-3's HOL-block
    // mitigation actually applies under reconnect races.
    bool IsSlotReady(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerLanesConfigured_[peerSlot].load(std::memory_order_acquire);
    }

    // The slot's current OCCUPANCY GENERATION (arc A, T3): a host-minted,
    // never-reused, non-zero token identifying WHO occupies `peerSlot` right
    // now. 0 = the slot is empty. A CHANGE between two reads is a REPLACEMENT
    // (person X left and person Y took the slot), which is the transition a
    // connected-boolean cannot express -- lowest-free slot reuse means X -> Y
    // carries no empty moment.
    //
    // Only the HOST mints (a client's slots 1..3 are permanently 0; its roster
    // is entirely wire-driven), so this is host-side authority, never a
    // client-side occupancy test. The generation NEVER goes on the wire.
    //
    // The point of reading it: a destructive slot-addressed action (kick/ban)
    // validates the token it captured against THIS live value atomically with
    // resolving the target's address, so a stale capture fails CLOSED instead of
    // landing on the slot's successor. Any thread.
    uint32_t peerGenerationForSlot(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return 0;
        return peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    }

    // --- Phase 2 moderation (host-only host-admin actions) ------------------

    // Accept filter: a predicate the host runs against an incoming connection's
    // remote IP (dotted-decimal, port excluded) BEFORE AcceptConnection. Return
    // true to allow, false to reject (the host closes the connection with a
    // "banned" reason). Wired by the harness to coop::ban_list::IsBanned. Set
    // ONCE at boot, before Start() spawns the net thread (read on the net thread
    // without a lock -- the thread-creation in Start() is the happens-before).
    // MTA precedent: the join-time ban check in CGame::Packet_PlayerJoinData
    // (reference/mtasa-blue/.../CGame.cpp:1973).
    using AcceptFilterFn = bool (*)(const char* remoteIp);
    void SetAcceptFilter(AcceptFilterFn fn) { acceptFilter_ = fn; }

    // Host: forcibly disconnect the client at peerSlot (1..kMaxPeers-1). Closes
    // the GNS connection (no linger -- an admin kick drops immediately; `reason`
    // is delivered to the peer's status callback m_szEndDebug so it can show
    // WHY) and runs the SAME per-slot teardown the ClosedByPeer path does (GNS
    // delivers no status callback for a connection WE close). Returns false if
    // the slot is out of range (or 0, the host self) or not connected. Thread-
    // safe (GNS API calls are; the bookkeeping uses the same atomics/mutexes the
    // net thread does). MTA precedent: CGame::QuitPlayer(QUIT_KICK).
    bool Kick(int peerSlot, const char* reason);

    // Kick, but only if `peerSlot` is STILL occupied by the person whose
    // occupancy generation is `expectedGeneration`. Returns false (doing nothing)
    // otherwise.
    //
    // WHY THIS EXISTS. A slot-addressed destructive action captures its target at
    // one moment and executes at another -- an admin opens the ban modal, types a
    // reason, and presses Ban some seconds later. Slots recycle in between. With a
    // bare slot number, a permanent IP ban can land on the SUCCESSOR: a different
    // person, banned by name of a seat they merely inherited.
    //
    // The check works because it compares a value from the LAGGING MIRROR (the
    // ledger row, captured when the modal opened) against the LIVE AUTHORITY
    // (this array), so a stale capture fails CLOSED. Comparing the mirror against
    // itself would fail OPEN and merely narrow the window to one tick.
    //
    // The handle CAS is what makes the claim atomic, and it is sound because of
    // the accept path's store ORDER: the generation is minted BEFORE peerConns_,
    // so a successor that got far enough to change the generation also changed the
    // handle, and the CAS fails. Thread-safe.
    bool KickWithToken(int peerSlot, uint32_t expectedGeneration, const char* reason);

    // Resolve `peerSlot`'s remote IP, but only while the slot is still occupied by
    // `expectedGeneration`'s owner. Same reasoning as KickWithToken: the BAN path
    // must not read the successor's address and write it to the permanent banlist.
    bool GetPeerAddressWithToken(int peerSlot, uint32_t expectedGeneration,
                                 char* out, int outLen) const;

  private:
    // Shared teardown for a slot the caller has ALREADY claimed (peerConns_
    // exchanged/CAS'd to 0). Kick claims blind; KickWithToken claims by handle.
    bool KickClaimed(int peerSlot, uint32_t hConn, const char* reason);

  public:

    // Query the remote IP (dotted-decimal, port excluded) of the connection at
    // peerSlot into `out` (recommend >= 48 bytes, SteamNetworkingIPAddr::
    // k_cchMaxString). Returns false (and out[0]='\0') if the slot isn't
    // connected or GNS has no remote address yet. Used by the BAN action to
    // capture the IP BEFORE the kick zeroes the slot.
    bool GetPeerAddress(int peerSlot, char* out, int outLen) const;

    // Client-only: when the HOST closes our connection (kick / ban / host quit /
    // host crash), GNS reports a human-readable reason -- the m_szEndDebug string
    // the host passed to CloseConnection (e.g. "kicked by host" / "banned by
    // host"). net_pump reads this ONCE on the client's disconnect edge to log WHY
    // before fleeing to the main menu. Returns the reason and clears it (empty if
    // none pending). Thread-safe (set on the net thread, taken on the game thread).
    std::string TakeHostCloseReason();

    // GNS C-callback adapter -- public so the file-local trampoline in
    // session.cpp can forward to it.
    static void OnConnStatusChanged(void* info);

private:
    // Topology dispatch helpers, called by Start() after the common GNS init +
    // global-callback registration. Each branches on role internally (host
    // listen / client connect) and returns false on any failure (Start clears
    // g_session + returns false). Defined in session_start.cpp.
    bool StartLanDirect();  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress
    bool StartP2P();        // rungs 1-3: signaling + CreateListenSocketP2P / Connect

    void NetThread();
    // Per-peer message dispatch. peerSlot identifies which peer the message
    // came from; on host that's read from msg->m_nConnUserData (set via
    // SetConnectionUserData at AcceptConnection time); on client peerSlot=0.
    void HandleMessage(int peerSlot, const void* data, int len);
    // Everything a PENDING (unadmitted) connection sends arrives here instead.
    // NET THREAD ONLY, from the single drain site. Security A2/A57.
    void HandlePendingMessage(int pendIdx, uint32_t hConn, const void* data, int len);
    // v37 NPC pose batch send/receive (defined in session_npc.cpp). Serialize
    // builds the body (EntityPoseBatchHeader + entries) of the live local batch
    // into `buf` (>= kNpcPoseDatagramMax) leaving the leading PacketHeader for the
    // per-peer send loop to stamp; returns the datagram length or 0 if empty
    // (takes localMutex_). Store parses one received datagram + newest-wins-stores
    // it for the game thread (takes remoteMutex_).
    int  SerializeLocalNpcBatch(uint8_t* buf);
    void StoreRemoteNpcBatch(const void* data, int len, uint32_t seq);
    // v80 (B3b) WorldActor pose batch send/receive (defined in session_worldactor.cpp -- the
    // session_npc.cpp clone). Same contract: Serialize builds the body after the leading PacketHeader
    // (returns 0 when empty / on a client -- only the host populates the local batch); Store parses +
    // newest-wins-stores one received datagram for the game thread. localMutex_ / remoteMutex_ as NPC.
    int  SerializeLocalWorldActorBatch(uint8_t* buf);
    void StoreRemoteWorldActorBatch(const void* data, int len, uint32_t seq);
    // v85 (Increment 2) trash-clump carry pose batch send/receive (session_trashcarry.cpp -- the
    // session_worldactor.cpp clone). Same contract: Serialize builds the body after the leading
    // PacketHeader (0 when empty / on a client); Store newest-wins-stores one received datagram.
    int  SerializeLocalTrashCarryBatch(uint8_t* buf);
    void StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq);
    // v66 voice receive-side store (defined in session_voice.cpp): validate one
    // VoiceFrame datagram, queue it on the voice inbox, host-relay. Net thread.
    void StoreVoiceFrame(int routeSlot, int peerSlot, const void* data, int len);
    // The 9 SCALAR stream channels' receive-store (defined in session_streams.cpp,
    // extracted 2026-07-18 per the 800-LOC cap -- bodies verbatim from the old
    // inline HandleMessage cases). Called from HandleMessage's grouped scalar case
    // labels AFTER the header parse / epoch latch / routeSlot derivation. Net thread.
    void StoreStreamPacket(MsgType type, int routeSlot, int peerSlot,
                           const void* data, int len, uint32_t seq);
    // NetThread step 3 (defined in session_streams.cpp): the per-sendHz-tick
    // stream fan-out -- scalar channels + the npc/worldactor/trashcarry batch
    // stamps (their Serialize* stay in their TUs). `now` is the shell's single
    // per-iteration timestamp (step-4 net-diag shares it); the cadence
    // time_points are NetThread locals advanced here by reference. Net thread.
    void SendStreamsTick(std::chrono::steady_clock::time_point now,
                         std::chrono::milliseconds sendInterval,
                         std::chrono::steady_clock::time_point& nextSend,
                         std::chrono::steady_clock::time_point& nextClockSend,
                         std::chrono::steady_clock::time_point& nextDeskSimSend,
                         uint64_t& sendFails);
    void HandleConnStatusChanged(void* info);
    // Host-only: find the lowest empty slot in [1..kMaxPeers-1]. Returns -1
    // if all client slots are taken (host is full).
    int FindFreePeerSlotForClient();
    // Find which peer slot owns hConn. Returns -1 if not found.
    int FindPeerSlotForConn(uint32_t hConn);
    // Per-peer reset (on slot disconnect). Caller holds remoteMutex_.
    void ResetPeerRemoteState(int peerSlot);

    // Host-relay topology (PR-FOUNDATION Tier 2 T2-2): forward an unreliable
    // packet the host just received from `originSlot` to every OTHER connected
    // client. Copies the datagram, rewrites the header's senderEpoch to the
    // host's own epoch (so the receiving client's connection-keyed epoch latch
    // passes) and senderSlot to `originSlot` (so the client routes the pose to
    // the right puppet). seq + body are preserved (per-origin-slot seq
    // monotonicity holds on the receiver). No-op unless role == Host. Net
    // thread only (called from HandleMessage). `data`/`len` is the full
    // received datagram (PacketHeader + body).
    void RelayUnreliableToOtherClients(int originSlot, const void* data, int len);

    // Host-relay topology (PR-FOUNDATION Tier 2 T2-3): forward a RELIABLE
    // datagram the host just received from `originSlot` to every OTHER
    // connected client, on the reliable channel + the kind's priority lane.
    // Same header rewrite as the unreliable relay (epoch -> host's,
    // senderSlot -> originSlot). Caller must have already verified the kind
    // is client-relayable (peer-originated gameplay) -- handshake +
    // host-authoritative kinds are NOT relayed. No-op unless role == Host.
    // Net thread only.
    void RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                     const void* data, int len);

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

    // GNS handles. uint32_t aliases for HSteamNetConnection / HSteamListenSocket /
    // HSteamNetPollGroup so this header doesn't include the GNS public API.
    std::atomic<uint32_t> hListen_{0};     // host only
    std::atomic<uint32_t> hPollGroup_{0};  // host only; receives all client msgs
    // Per-peer connection handles. On host: peerConns_[0] = unused (host self),
    // peerConns_[1..kMaxPeers-1] = accepted client connections (slot assigned
    // by AcceptConnection in FindFreePeerSlotForClient order). On client:
    // peerConns_[0] = the host's connection, rest unused.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerConns_{};

    // --- PENDING (UNADMITTED) CONNECTIONS -- security A2/A57, 2026-08-26 ------
    // A connection that has been accepted by GNS but has NOT yet proved it may
    // be here holds NO PLAYER SEAT. It lives here instead, and its
    // SetConnectionUserData carries `kPendingTag | index` -- deliberately
    // OUTSIDE [1, kMaxPeers), which is the range the single drain site
    // (session.cpp:667-674) validates before narrowing to a slot.
    //
    // WHY A SEPARATE BAND AND NOT A per-slot `admitted_` FLAG. `[V]` kMaxPeers
    // is 4, so there are exactly THREE client seats. If an unadmitted peer held
    // a seat, three sockets that say nothing would hold the entire lobby until a
    // timeout and re-take it instantly -- a lockout of the host's real friends
    // that no deadline fixes. Holding no seat makes that impossible rather than
    // time-bounded.
    //
    // WHAT IT BUYS FOR FREE: `[V]` roster_ledger.cpp:310 births the roster row
    // (and mints the player number, and drives the ~20-subsystem person fan-out)
    // on `IsSlotReady(slot)` alone, and net_pump.cpp:511 reads the same
    // predicate for the puppet edge. A peer with NO SLOT is never ready for any
    // slot, so all ~20 IsSlotReady consumers inherit this gate with ZERO edits.
    // That is why this is a band and not a new predicate at ~20 call sites.
    static constexpr int64_t  kPendingTag   = 0x7000'0000LL;

public:
    // Public because coop/net/peer_admission.cpp sizes its per-pending exchange
    // rows to it, and a silent mismatch there would drop the last band entry's
    // challenge state rather than fail to build.
    static constexpr int      kMaxPending   = 8;  // sockets, not seats
    // How long an UN-PROVED socket may sit before the sweep closes it. Generous
    // on purpose: the cost of a pending entry is one socket -- no seat, no lanes,
    // no parser, no roster row -- while the real bound on the band is
    // ParkPending's evict-the-oldest. It must clear the slowest legitimate
    // handshake, and on P2P that includes the whole ICE negotiation between the
    // host's park edge (Connecting) and the client's own Connected edge, which is
    // where the client sends its Hello.
    static constexpr uint64_t kPendingDeadlineMs = 30'000;

private:
    std::array<std::atomic<uint32_t>, kMaxPending> pendingConns_{};
    // Monotonic ms stamp of when each pending entry was accepted; 0 = free.
    // Bounds how long an un-proved socket may sit, which is a RESOURCE bound --
    // it is deliberately NOT the security control (holding no seat is).
    // READ, at last, by SweepPending(): it was written at three sites and read at
    // NONE until 2026-08-29, while the sentence above already claimed it bounded
    // something -- a control that existed only in a comment.
    std::array<std::atomic<uint64_t>, kMaxPending> pendingSinceMs_{};

    // True when a drained message's m_nConnUserData names a pending connection
    // rather than a player seat. The drain site asks this BEFORE its existing
    // `ud < 1 || ud >= kMaxPeers` drop, so a pending peer's traffic reaches the
    // admission handler and everything else keeps falling into that drop.
    static bool IsPendingUserData(int64_t ud) {
        return (ud & kPendingTag) == kPendingTag &&
               (ud & ~kPendingTag) >= 0 && (ud & ~kPendingTag) < kMaxPending;
    }
    static int  PendingIndexOf(int64_t ud) { return static_cast<int>(ud & ~kPendingTag); }

public:
    // Park a freshly-accepted connection in the pending band. Returns the
    // pending index, or -1 when the band is full (caller closes the connection).
    // NET THREAD ONLY (both accept edges).
    int  ParkPending(uint32_t hConn);
    // Promote a pending connection to a real player seat: allocates the lowest
    // free slot, re-points the connection's user data at it, mints the occupancy
    // generation and configures lanes -- i.e. everything the accept edge used to
    // do eagerly. Returns the slot, or -1 if the lobby is full (caller closes).
    // NET THREAD ONLY.
    int  AdmitPending(int pendingIdx, uint32_t hConn);

private:
    // Lanes + send-buffer mirror + the lanes-configured flag + (host) the
    // AssignPeerSlot send. TWO callers: the Connected callback for a client's
    // link to the host, and AdmitPending for a host's newly-seated client.
    void FinishPeerConnected(int slot, uint32_t hConn);
public:
    // Close every pending entry that has sat longer than kPendingDeadlineMs. Net
    // thread, once per pump pass; 8 relaxed loads on the idle path.
    void SweepPending();

    // Send ONE reliable message straight to a connection handle, with no slot, no
    // lane mapping and no world gate. This is the admission exchange's only send
    // path and it must stay that way: at exchange time the peer holds no seat, so
    // SendReliableToSlot cannot address it, and the pre-world gate it applies is
    // an allowlist keyed on kinds that presuppose a seated peer. Net thread only.
    bool SendRawReliableToConn(uint32_t hConn, ReliableKind kind,
                               const void* payload, int len);

    // CLIENT: the host admitted us (its AssignPeerSlot arrived after we proved it),
    // so finish the link -- lanes, the send-buffer mirror, and the flag every
    // IsSlotReady consumer waits on. Deliberately NOT done at the Connected edge
    // any more: an un-proved link that is already "ready" would let the joiner ask
    // for the world before it knows who is answering. No-op unless `hConn` is
    // still the connection in slot 0. Net thread only.
    void FinishClientLink(uint32_t hConn);

    // The guid of the peer in `slot`, DERIVED from the public key that peer proved
    // it holds at admission (`hex(SHA-256(pub)[0..16])`). Empty when the slot is
    // free or the occupant never proved one. The game thread reads this into the
    // roster row: the guid is no longer a field on the Join packet, because a
    // value the peer chooses cannot name whose stored inventory it is
    // (`[[lesson-a-gate-anchored-on-a-client-authored-value]]`).
    void        SetProvedGuidForSlot(int slot, const std::string& guid);
    std::string ProvedGuidForSlot(int slot) const;

    // Drop a pending entry (its connection closed, or it was refused).
    void ReleasePending(uint32_t hConn);
    // Is this connection already parked? NET THREAD ONLY.
    bool IsPendingConn(uint32_t hConn) const {
        if (hConn == 0) return false;
        for (int i = 0; i < kMaxPending; ++i)
            if (pendingConns_[i].load(std::memory_order_acquire) == hConn) return true;
        return false;
    }
    // The pending index for a connection, or -1. NET THREAD ONLY.
    int PendingIndexForConn(uint32_t hConn) const {
        if (hConn == 0) return -1;
        for (int i = 0; i < kMaxPending; ++i)
            if (pendingConns_[i].load(std::memory_order_acquire) == hConn) return i;
        return -1;
    }

private:
    // Set in the Connected callback after ConfigureLanesForPeer; cleared
    // when peerConns_[slot] is zeroed on disconnect. See IsSlotReady().
    std::array<std::atomic<bool>, kMaxPeers> peerLanesConfigured_{};

    // Per-slot PROVED identity guid (see SetProvedGuidForSlot). Its own mutex
    // rather than remoteMutex_: that one is taken at 60 Hz by every pose store
    // and this is written once per admission and read once per Join, so sharing
    // it would put a rare write behind the hottest lock in the file.
    mutable std::mutex                     provedGuidMutex_;
    std::array<std::string, kMaxPeers>     provedGuidBySlot_;

    // --- Per-slot OCCUPANCY GENERATION (arc A, T3) --------------------------
    // The authoritative "who is in this slot right now" token, minted HERE (the
    // net layer) because slots recycle: FindFreePeerSlotForClient hands out the
    // lowest free slot, so slot 2 can go from person X to person Y with no empty
    // moment in between, and a polled boolean edge misses that transition
    // entirely. A generation CHANGE is a replacement; 0 means the slot is empty.
    //
    // Deliberately NOT ownEpoch_: that value is minted in the SENDER's process
    // (session_start.cpp Start()) and rides the packet header, so it is
    // peer-DECLARED -- a rejoining peer could re-declare its prior epoch and show
    // the host no token change at all. Authority must never rest on a value the
    // peer chooses.
    //
    // Ownership: the NET layer is the only writer (mint at accept, clear on
    // close). The game-thread ledger only READS it. A GT write here would break
    // that single-writer claim.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerGenBySlot_{};
    std::atomic<uint32_t> peerGenCounter_{0};
    // Mint the next non-zero generation. Net thread (accept paths).
    uint32_t MintPeerGeneration() {
        uint32_t g;
        do { g = peerGenCounter_.fetch_add(1, std::memory_order_relaxed) + 1; } while (g == 0);
        return g;
    }

    // P2P only (cfg_.topology == Topology::P2P): the signaling-server transport.
    // The out-of-band channel that carries opaque ICE rendezvous blobs between
    // peers. Created in Start() before the net thread spawns; Poll()'d on the net
    // thread each loop; reset in Stop() AFTER the net thread joins and the
    // connections finish lingering (closing a P2P connection may need to send a
    // final signal). nullptr for LanDirect. shared_ptr because per-connection
    // signaling objects co-own it (they may outlive our reset() until GNS
    // Release()s them). The net thread reads it without a lock -- set before the
    // thread spawns, reset only after it joins (same discipline as ownEpoch_).
    std::shared_ptr<SignalingClient> signaling_;

    // Local pose slot (game thread writes, net thread reads + fan-outs).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    // v141 (A52): the monotonic time at which localPose_ was SAMPLED, not at which it is sent. The
    // net thread stamps this into the header rather than "now", because a game-thread hitch would
    // otherwise pair an old position with a fresh stamp and the receiver's interval would
    // under-report the motion that actually happened.
    uint32_t     localPoseStateMs_ = 0;
    bool hasLocal_ = false;
    PropPoseSnapshot localPropPose_{};
    bool hasLocalProp_ = false;
    uint32_t lastLocalPropSeq_ = 0;
    // v22: local ragdoll pelvis physics (game thread writes while ragdolling, net
    // thread reads + fan-outs). Same held/release shape as localPropPose_.
    RagdollPoseSnapshot localRagdollPose_{};
    bool hasLocalRagdoll_ = false;
    // v109: local hand-item view-relative transform (game thread writes while
    // holding, net thread reads + fan-outs). Same held/release shape as prop/ragdoll.
    HandPoseSnapshot localHandPose_{};
    bool hasLocalHand_ = false;
    // v109: local coords-panel live cursor (game thread writes while desk-claimed +
    // moving, net thread reads + fan-outs). Same held/release shape as localHandPose_.
    DeskCursorPoseSnapshot localDeskCursor_{};
    bool hasLocalDeskCursor_ = false;
    // v37: host NPC pose batch (game thread writes via SetLocalNpcPoseBatch, net thread reads
    // + fan-outs ONE EntityPose datagram). Empty vector = nothing to send (NPCs gone).
    std::vector<EntityPoseSnapshot> localNpcBatch_;
    bool hasLocalNpcBatch_ = false;
    // v80 (B3b): host WorldActor pose batch (game thread writes via SetLocalWorldActorPoseBatch, net
    // thread reads + fan-outs ONE WorldActorPose datagram). Empty vector = nothing to send.
    std::vector<WorldActorPoseSnapshot> localWorldActorBatch_;
    bool hasLocalWorldActorBatch_ = false;
    // v85 (Increment 2): host carried-trash-clump pose batch (game thread writes via
    // SetLocalTrashCarryBatch, net thread reads + fan-outs ONE TrashCarryPose datagram). Empty = none.
    std::vector<TrashClumpPoseSnapshot> localTrashCarryBatch_;
    bool hasLocalTrashCarryBatch_ = false;
    // v109 (design F): host world-clock snapshot (game thread writes via SetHostClock each
    // tick, net thread reads + fan-outs ONE unreliable ClockPose datagram on its own ~500 ms
    // throttle -- independent of the pose sendHz). Single value (host-authoritative, same to all).
    TimeSyncPayload localHostClock_{};
    bool hasLocalHostClock_ = false;
    // v111: host download-SIM output vector (game thread writes via SetHostDeskSim each desk_sim_sync
    // Tick, net thread fan-outs ONE unreliable DeskSimPose datagram on its own ~100 ms / 10 Hz
    // throttle). Single value (host-authoritative, same to all).
    DeskSimSnapshot localDeskSim_{};
    bool hasLocalDeskSim_ = false;
    // v113 (L4): host dish-pose row batch (game thread writes via SetHostDishPose; the net
    // thread sends ONE unreliable DishPose datagram per publish -- dirty one-shot, the GT
    // sweep owns the cadence).
    DishPoseBody localDishPose_{};
    bool dishPoseDirty_ = false;
    // v114 (L7): host reel-corrector value (game thread writes via SetHostReelPose; the net
    // thread sends ONE unreliable ReelPose datagram per publish -- dirty one-shot).
    ReelPosePayload localReelPose_{};
    bool reelPoseDirty_ = false;

    // Per-peer remote pose slots. Net thread writes (under remoteMutex_) on
    // receive; game thread reads via TryGetRemotePose(...).
    std::mutex remoteMutex_;
    std::array<PoseSnapshot, kMaxPeers> remotePoses_{};
    std::array<bool, kMaxPeers> hasRemote_{};
    std::array<uint32_t, kMaxPeers> lastRemoteSeq_{};
    std::array<uint64_t, kMaxPeers> remoteStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadStamp_{};
    std::array<PropPoseSnapshot, kMaxPeers> remotePropPoses_{};
    std::array<bool, kMaxPeers> hasRemoteProp_{};
    std::array<uint32_t, kMaxPeers> lastRemotePropSeq_{};
    std::array<uint64_t, kMaxPeers> remotePropStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadPropStamp_{};
    // v22: per-peer ragdoll pelvis physics (same per-slot stamp/seq shape as prop).
    std::array<RagdollPoseSnapshot, kMaxPeers> remoteRagdollPoses_{};
    std::array<bool, kMaxPeers> hasRemoteRagdoll_{};
    std::array<uint32_t, kMaxPeers> lastRemoteRagdollSeq_{};
    std::array<uint64_t, kMaxPeers> remoteRagdollStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadRagdollStamp_{};
    // v109: per-peer hand-item view-relative transform (same per-slot shape).
    std::array<HandPoseSnapshot, kMaxPeers> remoteHandPoses_{};
    std::array<bool, kMaxPeers> hasRemoteHand_{};
    std::array<uint32_t, kMaxPeers> lastRemoteHandSeq_{};
    std::array<uint64_t, kMaxPeers> remoteHandStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadHandStamp_{};
    // v109: per-peer coords-panel live cursor (same per-slot shape as hand).
    std::array<DeskCursorPoseSnapshot, kMaxPeers> remoteDeskCursors_{};
    std::array<bool, kMaxPeers> hasRemoteDeskCursor_{};
    std::array<uint32_t, kMaxPeers> lastRemoteDeskCursorSeq_{};
    std::array<uint64_t, kMaxPeers> remoteDeskCursorStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadDeskCursorStamp_{};
    // v109 (design F): latest received host world-clock snapshot (host->client; ONE slot, not
    // per-peer -- the host is the only sender). Net thread stores under remoteMutex_; client game
    // thread drains via TryGetHostClock (newest-wins by header seq; apply on isNew).
    TimeSyncPayload remoteHostClock_{};
    bool hasRemoteHostClock_ = false;
    uint32_t lastRemoteHostClockSeq_ = 0;
    uint64_t remoteHostClockStamp_ = 0;
    uint64_t lastReadHostClockStamp_ = 0;
    // v111: latest received host download-SIM output vector (host->client; ONE slot, host is the
    // only sender). Net thread stores under remoteMutex_; client game thread drains via
    // TryGetHostDeskSim (newest-wins by header seq; interpolate + overwrite).
    DeskSimSnapshot remoteDeskSim_{};
    bool hasRemoteDeskSim_ = false;
    uint32_t lastRemoteDeskSimSeq_ = 0;
    uint64_t remoteDeskSimStamp_ = 0;
    uint64_t lastReadDeskSimStamp_ = 0;
    // v113 (L4): latest received dish-pose batch (host->client; ONE slot, host is the only
    // sender). Net thread stores under remoteMutex_; client game thread drains via
    // TryGetHostDishPose (newest-wins by header seq; apply on isNew).
    DishPoseBody remoteDishPose_{};
    bool hasRemoteDishPose_ = false;
    uint32_t lastRemoteDishPoseSeq_ = 0;
    uint64_t remoteDishPoseStamp_ = 0;
    uint64_t lastReadDishPoseStamp_ = 0;
    // v114 (L7): latest received reel corrector (host->client; ONE slot, host is the only
    // sender). Net thread stores under remoteMutex_; client drains via TryGetHostReelPose.
    ReelPosePayload remoteReelPose_{};
    bool hasRemoteReelPose_ = false;
    uint32_t lastRemoteReelPoseSeq_ = 0;
    uint64_t remoteReelPoseStamp_ = 0;
    uint64_t lastReadReelPoseStamp_ = 0;
    // v37: latest received NPC pose batch (host->client; ONE slot, not per-peer -- the host is the
    // only sender). Net thread stores under remoteMutex_; game thread drains via TakeRemoteNpcBatch.
    std::vector<EntityPoseSnapshot> remoteNpcBatch_;
    bool     hasRemoteNpcBatch_ = false;
    uint32_t lastRemoteNpcSeq_  = 0;
    // v80 (B3b): latest received WorldActor pose batch (host->client; ONE slot, host is the only
    // sender). Net thread stores under remoteMutex_; game thread drains via TakeRemoteWorldActorBatch.
    std::vector<WorldActorPoseSnapshot> remoteWorldActorBatch_;
    bool     hasRemoteWorldActorBatch_ = false;
    uint32_t lastRemoteWorldActorSeq_  = 0;
    // v85 (Increment 2): latest received trash-clump carry batch (host->client; ONE slot, host is the
    // only sender). Net thread stores under remoteMutex_; game thread drains via TakeRemoteTrashCarryBatch.
    std::vector<TrashClumpPoseSnapshot> remoteTrashCarryBatch_;
    bool     hasRemoteTrashCarryBatch_ = false;
    uint32_t lastRemoteTrashCarrySeq_  = 0;
    // Per-slot expected senderEpoch latched from the first packet arriving
    // from that slot (PR-FOUNDATION-1b v16). Zero == not yet latched;
    // subsequent packets whose header epoch doesn't match are dropped at
    // HandleMessage entry. Cleared on disconnect in ResetPeerRemoteState
    // so the next connection on the same slot re-latches. Protected by
    // remoteMutex_ -- updated under the same lock as remoteStamp_ /
    // hasRemote_ on receive paths.
    std::array<uint32_t, kMaxPeers> expectedEpoch_{};

    // Reliable inbox (shared across peers; FIFO of arrival order).
    std::mutex reliableInboxMutex_;
    std::deque<ReliableMessage> reliableInbox_;
    // Exact high-water of reliableInbox_ depth since the last ~1 Hz net-diag sample,
    // stamped at the ENQUEUE site (under reliableInboxMutex_, so it sees every push --
    // sampling it in the drain loop would only observe the 200 Hz pre-receive value and
    // miss a spike the game thread drained between iterations). Reported and reset by the
    // net-diag block. It exists because the W10 pause is otherwise INVISIBLE: without the
    // depth that armed it, "no pause fired" cannot be told apart from "the depth never got
    // there", and the instrument would pass blind. Net thread writes, net thread reads.
    std::atomic<uint32_t> reliableInboxPeak_{0};

    // v66 voice inbox (audit I-3): per-SLOT fixed rings -- zero net-thread heap
    // alloc (the old shared deque allocated per frame) and per-sender fairness
    // (a spammer overwrites only their own oldest audio, not everyone's).
    // Drop-oldest per slot on overflow; the jitter buffer treats the loss as
    // ordinary gaps. head/tail are monotonic (tail - head <= kVoiceRingPerSlot).
    struct VoiceSlotRing {
        VoiceFrameMsg ring[kVoiceRingPerSlot];
        uint32_t head = 0, tail = 0;
    };
    std::mutex voiceInboxMutex_;
    VoiceSlotRing voiceRings_[kMaxPeers];

    // v56 bulk-chunk diversion target (see SetBulkSink). Written once at install
    // (game thread), read per-message on the net thread.
    std::atomic<BulkSinkFn> bulkSink_{nullptr};
    // W3: SaveTransferBegin diversion target (see SetSaveBeginSink). Same write-once-at-install
    // lifetime as bulkSink_.
    std::atomic<BulkSinkFn> saveBeginSink_{nullptr};

    // v56 per-slot world-ready flags (see MarkSlotWorldReady).
    std::array<std::atomic<bool>, kMaxPeers> slotWorldReady_{};
    // Seeds arc: receive-side apply backpressure refcount (see SetApplyBackpressure).
    std::atomic<int> applyBackpressureCount_{0};
    // Seeds arc (2026-08-23): per-slot relay-eligibility stamp = the hConn that
    // announced ClientWorldReady, set on the NET thread at receipt (the GT
    // world-ready flag lags by one drain; the relay consults this so a reliable
    // received in that gap is neither skipped-and-lost nor double-delivered --
    // session.cpp receive path + session_relay.cpp). hConn-stamped so a recycled
    // slot can never inherit eligibility. Cleared beside backlog_.FreeSlot.
    std::array<std::atomic<uint32_t>, kMaxPeers> relayEligible_{};

    std::atomic<uint32_t> sendSeq_{0};

    // R-4b: the reliable-send delivery guarantee (per-(slot,lane) backlogs;
    // see send_backlog.h). Drained on the net thread each loop pass; freed at
    // every peerConns_ zeroing site. sendBufBytes_ mirrors the per-connection
    // SendBufferSize actually configured (knob or default) -- the drain's D8
    // reserve gate is computed against it.
    SendBacklog backlog_;
    int sendBufBytes_ = 512 * 1024;
    // Fatal-backlog close (D3): host -> Kick(slot); client -> claim + the same
    // KickClaimed teardown on its host connection (GNS delivers no callback
    // for a connection we close ourselves). Net thread.
    void FatalCloseSlot(int slot, const char* reason);
    // Per-slot RTT (ms), sampled ~1 Hz on the net thread from GNS m_nPing. 0-init;
    // the sampler sets -1 for a slot with no live connection and the real ping for a
    // connected one. Replaces the old aggregate lastRttMs_ (RULE 2: event_feed now
    // fans the PER-PEER ping to each puppet, not one min shared by all).
    std::array<std::atomic<int>, kMaxPeers> rttMsBySlot_{};

    // Phase 2: the host's accept predicate (ban filter). nullptr = accept all.
    // Set before Start() spawns the net thread; read on the net thread.
    AcceptFilterFn acceptFilter_ = nullptr;

    // Client-only: the reason GNS reported when the host closed our connection
    // (m_szEndDebug). Written on the net thread in the slot-0 ClosedByPeer/
    // ProblemDetectedLocally branch; taken (move + clear) on the game thread by
    // net_pump via TakeHostCloseReason. Tiny dedicated mutex (rare path, no lock-
    // order entanglement with remoteMutex_ / reliableInboxMutex_).
    std::mutex  hostCloseMutex_;
    std::string hostCloseReason_;

    // This peer's own per-process session epoch (PR-FOUNDATION-1b v16).
    // Minted non-zero via std::random_device at Start(); stamped on the
    // header of every outbound packet (WriteHeader's senderEpoch arg).
    // Read on the net thread without a lock -- Start() completes before
    // the net thread is spawned and the value never changes afterward,
    // so it's effectively const for the session's lifetime.
    uint32_t ownEpoch_ = 0;
};

}  // namespace coop::net

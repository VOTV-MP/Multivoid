// coop/net/session_receive.cpp -- the datagram receive switch for Session: the admission handler
// an unproved connection is routed to, and the per-message dispatch every seated peer's datagram
// goes through (the header checks, the stream stores, the reliable inbox, the relay fan-out).
// The send paths and the net thread's loop stay in session.cpp.

#include "coop/net/session.h"

#include "coop/dev/wire_census.h"
#include "coop/net/connect_history.h"  // Floods -- the source a flood ended stays refused
#include "coop/net/net_clock.h"        // NowMs -- the probe's echo is timed on the net layer's clock
#include "coop/net/peer_admission.h"   // the identity challenge every peer passes
#include "coop/net/peer_identity.h"    // GuidForPublicKey -- the proved storage name
#include "client_authorship.h"  // co-located private header: which kinds a client may author
#include "session_lanes.h"      // co-located private header: the admission and relayable kind lists
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <cstring>

namespace coop::net {

namespace {
// A kind the host relays from one client to the others must be one a client may author: the edge below would
// otherwise refuse what the relay table forwards. Every byte value is checked, so a kind added to the relay
// table without its authorship row fails the build here.
constexpr bool RelayedKindsAreClientAuthored() {
    for (int v = 0; v < 256; ++v) {
        const auto k = static_cast<ReliableKind>(v);
        if (IsClientRelayableReliableKind(k) && AuthorshipOf(k) != ClientAuthorship::Client) return false;
    }
    return true;
}
static_assert(RelayedKindsAreClientAuthored(), "a relayable kind that is not Client in client_authorship.h");
}  // namespace

// ---- admission: what an unadmitted connection may do ----
// A parked connection holds no seat, so it cannot reach any handler that takes a senderSlot,
// which is every handler; this is the only code an unproved peer can run. The admission test is
// the identity challenge (coop/net/peer_admission.h): a peer is seated when it has signed our
// nonce with the private key its GNS identity names. The gate needs its own wire pair upstream of
// the save transfer, because a joining client is in menu mode and its first message is
// SaveTransferRequest; "send a Join first" deadlocks every honest join.
void Session::HandlePendingMessage(int pendIdx, uint32_t hConn, const void* data, int len) {
    // The band is the authority on whether this index is still live, and the guard makes a refusal
    // O(1): GNS hands the drain up to 256 messages carrying the user data they had at receipt, so
    // after a refusal every message of that connection's already in the batch still routes here,
    // and without this each re-ran the refusal (a CloseConnection and a WARN, whose fflush is a
    // disk sync).
    if (pendIdx < 0 || pendIdx >= kMaxPending) return;
    if (pendingConns_[pendIdx].load(std::memory_order_acquire) != hConn) return;
    MsgType type;
    uint32_t seq, senderEpoch;
    uint8_t headerSenderSlot;
    if (!ParseHeader(data, len, type, seq, senderEpoch, headerSenderSlot)) {
        // A protocol mismatch is worth saying to an unadmitted peer too; the silent alternative is
        // a connection that stays "Connected" forever with every packet dropped.
        const uint16_t peerVer = PeekProtocolVersion(data, len);
        if (peerVer != 0 && peerVer != kProtocolVersion) {
            char reason[64];
            std::snprintf(reason, sizeof(reason), "protocol mismatch: peer=v%u, ours=v%u",
                          static_cast<unsigned>(peerVer), static_cast<unsigned>(kProtocolVersion));
            UE_LOGW("net: %s -- closing PENDING %d", reason, pendIdx);
            RetirePending(pendIdx, hConn, EndReason::BuildMismatch, reason);
        }
        return;
    }
    if (type != MsgType::Reliable) return;
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
    ReliableHeader rh;
    std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
    const int payloadLen = len - static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader));
    const void* payload = static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                          sizeof(ReliableHeader);
    // The declared length must match what arrived before anything reads the body: this is the one
    // parser an unauthenticated peer can reach.
    if (payloadLen < 0 || rh.payloadLen != static_cast<uint16_t>(payloadLen)) {
        UE_LOGW("net: PENDING %d sent a length-inconsistent packet -- closing [%s]", pendIdx,
                Describe(EndReason::MalformedPacket).id);
        RetirePending(pendIdx, hConn, EndReason::MalformedPacket, "malformed packet");
        return;
    }

    const auto res = peer_admission::HostOnPendingReliable(
        *this, pendIdx, hConn, static_cast<ReliableKind>(rh.kind), payload, payloadLen);
    if (res.verdict == peer_admission::Verdict::Continue) return;
    if (res.verdict == peer_admission::Verdict::Refuse) {
        UE_LOGW("net: PENDING %d REFUSED on kind=%u -- [%s] %s",
                pendIdx, static_cast<unsigned>(rh.kind), Describe(res.code).id, res.reason);
        RetirePending(pendIdx, hConn, res.code, res.reason);
        return;
    }

    const std::string guid = peer_identity::GuidForPublicKey(res.provedKey);

    // The ban check, MTA's shape (CGame::Packet_PlayerJoinData: the serial at CGame.cpp:1956, then
    // the IP at :1973; MTA supplants the old player first, CGame.cpp:1892-1907, ours refuses
    // first so a banned joiner kicks nobody), asked here because the proof is the first place
    // every transport holds an id and a route: the proved id, and the connection's own address
    // (empty on a relayed path or when GNS has none). Before the supersession kick and the seat:
    // a banned joiner costs no seat and kicks nobody.
    if (banCheck_) {
        char addr[SteamNetworkingIPAddr::k_cchMaxString] = {};
        SteamNetConnectionInfo_t ci{};
        auto* sockets = SteamNetworkingSockets();
        if (sockets && sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &ci) &&
            !(ci.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) &&
            !ci.m_addrRemote.IsIPv6AllZeros()) {
            ci.m_addrRemote.ToString(addr, sizeof(addr), /*bWithPort*/false);
        }
        char why[96] = {};
        if (banCheck_(guid.c_str(), addr, why, sizeof(why))) {
            UE_LOGI("net: PENDING %d refused -- identity %.8s... or its address is banned (%s) [%s]",
                    pendIdx, guid.c_str(), why[0] ? why : "no reason recorded",
                    Describe(EndReason::Banned).id);
            RetirePending(pendIdx, hConn, EndReason::Banned, why[0] ? why : nullptr);
            return;
        }
    }

    // Supersession, one identity one seat: a peer that proved the key already sitting in a slot is
    // that person, so the older connection goes and the new one is seated, here, before the seat is
    // asked for. The trigger is a dropped link, not an attack: GNS takes seconds to time out a dead
    // connection, a player rejoining inside that window would otherwise take a second seat, and the
    // stored inventory is keyed by guid, so both seats would persist it (last writer wins).
    for (int s = 1; s < kMaxPeers; ++s) {
        if (peerConns_[s].load() == 0) continue;
        if (ProvedGuidForSlot(s) != guid) continue;
        UE_LOGW("net: slot %d already holds identity %s -- superseding it with the "
                "new connection (the holder proved the same key)", s, guid.c_str());
        Kick(s, EndReason::Superseded, "superseded by a new connection from your own identity");
        break;  // one seat per identity is the invariant; there cannot be a second
    }

    const int slot = AdmitPending(pendIdx, hConn);
    if (slot < 0) {
        UE_LOGW("net: lobby full of admitted players -- refusing PENDING %d [%s]", pendIdx,
                Describe(EndReason::HostFull).id);
        RetirePending(pendIdx, hConn, EndReason::HostFull, "host full");
        return;
    }
    // The peer's storage guid is derived from the key it proved and published here, on the net
    // thread into a net-owned store, because the roster row is game-thread-only; the Join handler
    // reads it back there. The guid naming a player's stored inventory is a fact about a key, not a
    // string the peer asked to be called.
    SetProvedGuidForSlot(slot, peerGenerationForSlot(slot), guid);
    UE_LOGI("net: PENDING %d ADMITTED -> slot %d (identity-bound, guid %s)",
            pendIdx, slot, guid.c_str());
    peer_admission::HostForgetPending(pendIdx);
}

void Session::HandleMessage(int peerSlot, uint32_t hConn, const void* data, int len) {
    MsgType type;
    uint32_t seq;
    uint32_t senderEpoch;
    uint8_t headerSenderSlot;
    if (!ParseHeader(data, len, type, seq, senderEpoch, headerSenderSlot)) {
        // Counted by the host's budget like any packet a seated connection sends (TakeBudget below), as a message.
        if (cfg_.role == Role::Host && peerSlot >= 1 && peerSlot < kMaxPeers &&
            !TakeBudget(peerSlot, hConn, MsgType{}, data, len))
            return;
        // Distinguish garbage (a silent drop) from a peer on another protocol version (a clean
        // close with a readable reason, so both ends see why): the silent alternative is a
        // connection that stays "Connected" with every packet dropped.
        const uint16_t peerVer = PeekProtocolVersion(data, len);
        if (peerVer != 0 && peerVer != kProtocolVersion &&
            peerSlot >= 0 && peerSlot < kMaxPeers) {
            char reason[64];
            std::snprintf(reason, sizeof(reason),
                          "protocol mismatch: peer=v%u, ours=v%u",
                          static_cast<unsigned>(peerVer),
                          static_cast<unsigned>(kProtocolVersion));
            // Through the teardown funnels, never a bare close: a host frees the seat, a client
            // records the code and drops its state so the join fails on screen instead of
            // sitting on the cover until the failsafe.
            if (cfg_.role == Role::Host) {
                UE_LOGW("net: %s -- kicking peer slot %d [%s]", reason, peerSlot,
                        Describe(EndReason::BuildMismatch).id);
                Kick(peerSlot, EndReason::BuildMismatch, reason);
            } else if (peerSlot == 0) {
                const EndReason code = peerVer > kProtocolVersion ? EndReason::HostNewer
                                                                  : EndReason::HostOlder;
                UE_LOGW("net: %s -- leaving the host [%s]", reason, Describe(code).id);
                LeaveHost(code, reason);
            }
        }
        return;
    }
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return;
    net_stats::AddRecv(static_cast<uint32_t>(len));

    // The host's bound on what one client sends (receive_budget.h, TakeBudget below), counted on every packet a seated
    // connection sends, one whose header does not parse included (above), ahead of the epoch latch and every check
    // after it, so a packet any of them drops costs the sender as much as one they pass.
    if (cfg_.role == Role::Host && !TakeBudget(peerSlot, hConn, type, data, len)) return;

    // The per-peer epoch latch: the slot's first packet sets the expected senderEpoch, later
    // packets must match or are dropped; ResetPeerRemoteState clears it on disconnect so the next
    // occupant re-latches. senderEpoch 0 is never latched (a sender that forgot to mint). Checked
    // under remoteMutex_, the lock every per-peer store below takes, so the latch is atomic with
    // the reset's clear. A packet it drops is said once and the rest summed per window
    // (a tally per cause): a sender could otherwise make the log the cost, one line per packet.
    {
        std::lock_guard<std::mutex> lk(remoteMutex_);
        if (senderEpoch == 0) {
            zeroEpochRefusals_.Note(static_cast<std::uint8_t>(peerSlot),
                                    "its senderEpoch is 0, a sender that never minted one",
                                    RefusalTally::Clock::now());
            return;
        }
        const uint32_t expected = expectedEpoch_[peerSlot];
        if (expected == 0) {
            expectedEpoch_[peerSlot] = senderEpoch;
            UE_LOGI("net: latched senderEpoch=0x%08x for peer slot %d",
                    static_cast<unsigned>(senderEpoch), peerSlot);
        } else if (expected != senderEpoch) {
            // The common cause is a reconnect race (in-flight packets from the old connection arriving
            // after the new one re-latched), benign and self-correcting.
            staleEpochRefusals_.Note(static_cast<std::uint8_t>(peerSlot),
                                     "its senderEpoch is not the one this slot latched (an earlier connection's)",
                                     RefusalTally::Clock::now());
            return;
        }
    }

    // The logical origin slot that routes pose data into the per-puppet store, distinct from the
    // connection slot the epoch latch used: on the host the connection is the origin
    // (GNS-authenticated user data; the header's senderSlot is spoofable and ignored), on a client
    // every packet arrives on the one host connection, so the host-stamped header senderSlot
    // routes.
    int routeSlot = peerSlot;
    if (cfg_.role == Role::Client) {
        routeSlot = static_cast<int>(headerSenderSlot);
        if (routeSlot < 0 || routeSlot >= kMaxPeers) {
            UE_LOGW("net: client received packet with out-of-range senderSlot=%d "
                    "-- dropping", routeSlot);
            return;
        }
    }

    // The dev wire census (VOTVCOOP_WIRE_CENSUS=1): every non-reliable inbound counted by logical
    // origin; reliables are counted once the kind is parsed.
    if (type != MsgType::Reliable && dev::wire_census::Enabled())
        dev::wire_census::NoteStream(routeSlot, static_cast<unsigned>(type));

    switch (type) {
    case MsgType::PoseSnapshot:
    case MsgType::PropPose:
    case MsgType::RagdollPose:
    case MsgType::HandPose:
    case MsgType::DeskCursorPose:
    case MsgType::ClockPose:
    case MsgType::DeskSimPose:
    case MsgType::DishPose:
    case MsgType::ReelPose:
        // The nine scalar stream channels (session_streams.cpp): validate, newest-wins store, host
        // relay.
        StoreStreamPacket(type, routeSlot, peerSlot, data, len, seq);
        break;
    case MsgType::EntityPose:
        StoreRemoteNpcBatch(data, len, seq);  // -> session_npc.cpp (parse + newest-wins store)
        break;
    case MsgType::WorldActorPose:
        StoreRemoteWorldActorBatch(data, len, seq);  // session_worldactor.cpp (parse + newest-wins store)
        break;
    case MsgType::TrashCarryPose:
        StoreRemoteTrashCarryBatch(data, len, seq);  // session_trashcarry.cpp (parse + newest-wins store)
        break;
    case MsgType::PropDrivePose:
        StoreRemotePropDriveBatch(routeSlot, data, len, seq);  // session_propdrive.cpp (parse + merge by eid)
        break;
    case MsgType::VoiceFrame:
        // Voice is a stream: every arrival is queued (no header-seq stale drop; the per-payload
        // voice seq orders at the jitter buffer). Store and relay in session_voice.cpp.
        StoreVoiceFrame(routeSlot, peerSlot, data, len);
        break;
    case MsgType::Reliable: {
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
        ReliableHeader rh;
        std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
        // The receiving end of the sender's own queued-byte count, per LINK (peerSlot, not the
        // relayed origin): the same wire packet counted at both ends, so a sender's goodput estimate
        // is checkable against a second measurement instead of against itself. The admission kinds
        // are excluded because the sending end cannot count them -- they travel before a slot exists
        // -- and a pair of totals is only comparable over the same set of bytes.
        if (!IsAdmissionKind(static_cast<ReliableKind>(rh.kind)))
            rateControl_.NoteReliableReceived(peerSlot, len);
        if (dev::wire_census::Enabled())
            dev::wire_census::NoteReliable(routeSlot, static_cast<unsigned>(rh.kind));
        // Default deny at the edge (client_authorship.h): a kind no client may author, or a byte that names no
        // kind, is dropped here, counted by the budget ahead of the epoch latch and acted on by nothing below --
        // not diverted, echoed, inboxed or relayed. The first of each value in a session is said with its number,
        // the rest summed per window: MTA logs each refused event, and rate-limits only an unknown one, one line
        // per gap for the whole server (CGame.cpp:2683-2707); the fold here is ours, the net thread's log being one
        // a client could fill.
        if (cfg_.role == Role::Host) {
            const ClientAuthorship who = AuthorshipOf(static_cast<ReliableKind>(rh.kind));
            if (who == ClientAuthorship::Host || who == ClientAuthorship::NotAKind) {
                const char* why = who == ClientAuthorship::Host ? "only the host authors it" : "it names no kind";
                if (!saidHostKind_[rh.kind]) {
                    saidHostKind_[rh.kind] = true;
                    UE_LOGW("net: refused kind %u from slot %d -- %s (client_authorship.h); later ones are "
                            "summed", static_cast<unsigned>(rh.kind), peerSlot, why);
                } else {
                    hostKindRefusals_.Note(static_cast<std::uint8_t>(peerSlot), why,
                                           RefusalTally::Clock::now());
                }
                return;
            }
        }
        // payloadLen is a uint16, so only the upper bound is a real guard.
        const int payloadLen = static_cast<int>(rh.payloadLen);
        // The save-blob chunk exceeds the inbox payload by design: diverted whole to the bulk sink
        // (save_transfer's heap assembler) on the net thread; it never enters the inbox and is
        // never relayed (host to one client only).
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::SaveTransferChunk) {
            if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
            if (BulkSinkFn sink = bulkSink_.load(std::memory_order_acquire)) {
                sink(static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                     payloadLen);
            }
            return;
        }
        if (payloadLen > kMaxReliablePayload) return;
        if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;
        // --- admission, client side ---
        // On the net thread, not through the inbox: a joining client is in menu mode and its game
        // thread may be inside a multi-second save load, so an exchange that waited on the game
        // tick would stall behind the very thing it must precede. It touches no engine object,
        // which makes that possible.
        if (cfg_.role == Role::Client && peerSlot == 0) {
            const uint32_t hostConn = peerConns_[0].load();
            const void* body = static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                               sizeof(ReliableHeader);
            const char* closeWhy = nullptr;
            EndReason closeCode = EndReason::None;
            if (peer_admission::ClientOnReliable(*this, hostConn,
                                                 static_cast<ReliableKind>(rh.kind),
                                                 body, payloadLen, &closeWhy, &closeCode)) {
                if (closeWhy) {
                    UE_LOGE("net: leaving this host -- [%s] %s", Describe(closeCode).id,
                            closeWhy);
                    LeaveHost(closeCode, closeWhy);
                }
                return;  // consumed by the exchange; never reaches the game thread
            }
            // The host's AssignPeerSlot is the admission signal, peeked rather than consumed (it
            // still carries the slot and hostElementId the game thread needs). FinishPeerConnected
            // sends it only from AdmitPending, so its arrival means we were seated. Refused before
            // the host has proved itself, or an impostor could skip the challenge and seat us.
            if (static_cast<ReliableKind>(rh.kind) == ReliableKind::AssignPeerSlot) {
                if (!peer_admission::ClientProvedHost()) {
                    static const char* kWhy =
                        "the host tried to seat us without proving its identity";
                    UE_LOGE("net: %s -- leaving", kWhy);
                    LeaveHost(EndReason::HostNotProved, kWhy);
                    return;
                }
                FinishClientLink(hostConn);
            }
        }
        // The admission kinds are net-thread-terminal in both directions; one arriving here is an
        // already-admitted peer replaying it, with no consumer, and letting it into the inbox costs
        // an "unknown ReliableKind" warning per copy.
        if (IsAdmissionKind(static_cast<ReliableKind>(rh.kind))) return;
        // The link probe pair is net-thread-terminal in both directions: an echo built on the game
        // thread would carry that thread's frame time, and on a joiner loading a world that is tens
        // of seconds, so the reading would be of the wrong queue entirely. The reply is the
        // request's bytes returned unchanged, so answering costs no state.
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::LinkProbe) {
            if (payloadLen == static_cast<int>(sizeof(LinkProbePayload))) {
                LinkProbePayload p{};
                std::memcpy(&p, static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                                    sizeof(ReliableHeader), sizeof(p));
                // A refused echo and a lost probe are the same thing to the prober, so the side that
                // knows the difference records it.
                if (!TrySendReliableToSlot(peerSlot, ReliableKind::LinkProbeReply, &p, sizeof(p)))
                    rateControl_.NoteEchoRefused(peerSlot);
            }
            return;
        }
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::LinkProbeReply) {
            if (payloadLen == static_cast<int>(sizeof(LinkProbePayload))) {
                LinkProbePayload p{};
                std::memcpy(&p, static_cast<const uint8_t*>(data) + sizeof(PacketHeader) +
                                    sizeof(ReliableHeader), sizeof(p));
                rateControl_.NoteProbeReply(peerSlot, p, NowMs());
            }
            return;
        }
        // The save-blob announce is diverted to the net thread too, so it lands on the same thread
        // and in the same lane order as the chunks: diverted for ordering, not size, and the sole
        // Begin path (two paths for one message is what created the window).
        if (static_cast<ReliableKind>(rh.kind) == ReliableKind::SaveTransferBegin) {
            if (BulkSinkFn sink = saveBeginSink_.load(std::memory_order_acquire)) {
                sink(static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                     payloadLen);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            // Only while the slot still holds the connection this arrived on, read under the
            // inbox's own mutex: an off-thread kick zeroes the handle before it takes this mutex
            // to erase the slot's entries, so a message that locked first is erased by it and one
            // that locks after is dropped here -- none is left for the game thread to dispatch
            // after the slot's teardown. The departed peer's message goes no further (no relay).
            if (peerConns_[peerSlot].load() != hConn) return;
            // The join gate opens before the announce is visible to the game thread, which runs the
            // connect replay on it: no send of that replay, and no relay reaching the slot after
            // this, passes ungated. The snapshot's completion closes it (prop_snapshot.cpp). Lock
            // order: the inbox mutex, then the slot's; nothing takes them the other way round.
            if (cfg_.role == Role::Host &&
                static_cast<ReliableKind>(rh.kind) == ReliableKind::ClientWorldReady &&
                peerSlot >= 1 && peerSlot < kMaxPeers) {
                backlog_.OpenJoinGate(peerSlot);
            }
            // No hard cap here: a silent drop on an in-order reliable lane is permanent state
            // divergence. Growth is bounded upstream by the NetThread pause both roles share, which
            // stops receiving at kReliableInboxSoftPause while GNS buffers losslessly beneath; one
            // 256-wide batch per iteration bounds the overshoot, and HandleMessage has exactly one
            // caller. emplace + memcpy avoids a per-receive allocation; senderPeerSlot = routeSlot
            // so drainers route per sender.
            reliableInbox_.emplace_back();
            ReliableMessage& m = reliableInbox_.back();
            m.kind = static_cast<ReliableKind>(rh.kind);
            m.senderPeerSlot = routeSlot;
            m.payloadLen = static_cast<uint16_t>(payloadLen);
            std::memcpy(m.payload,
                        static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                        static_cast<size_t>(payloadLen));
            // Exact depth high-water for the net-diag sample (see the member's comment).
            const auto depth = static_cast<uint32_t>(reliableInbox_.size());
            if (depth > reliableInboxPeak_.load(std::memory_order_relaxed))
                reliableInboxPeak_.store(depth, std::memory_order_relaxed);
        }
        // Stamp the slot relay-eligible at the net-thread receipt of ClientWorldReady,
        // hConn-stamped, after the inbox accepted the announce. The game-thread flip lags this by
        // one drain, and a peer reliable received in that gap would otherwise be skipped for the
        // joiner and applied to the host after the ready-edge seed's read (lost); with the stamp,
        // rows received after relay directly and rows received before ride the seed. Exactly once,
        // by ordering.
        if (cfg_.role == Role::Host &&
            static_cast<ReliableKind>(rh.kind) == ReliableKind::ClientWorldReady &&
            peerSlot >= 1 && peerSlot < kMaxPeers) {
            relayEligible_[peerSlot].store(peerConns_[peerSlot].load(),
                                           std::memory_order_release);
        }
        // Host relay: peer-originated gameplay reliables go to every other client.
        // Host-authoritative kinds (weather, sky, entities) and handshake kinds are not relayed;
        // the host also processes the reliable locally, so its own view of the origin's puppet
        // updates.
        if (cfg_.role == Role::Host &&
            IsClientRelayableReliableKind(static_cast<ReliableKind>(rh.kind))) {
            RelayReliableToOtherClients(peerSlot,
                                        static_cast<ReliableKind>(rh.kind),
                                        data, len);
        }
        break;
    }
    default:
        break;
    }
}

// The edge's answer to one packet. The kick tears the slot down here, on the net thread; the receive loop's connection
// check then drops the rest of that connection's batch.
bool Session::TakeBudget(int peerSlot, uint32_t hConn, MsgType type, const void* data, int len) {
    switch (receiveBudget_.TakePacket(peerSlot, hConn, type, NowMs())) {
    case ReceiveBudget::Take::Ok:
        return true;
    case ReceiveBudget::Take::StreamOver:
        streamRateRefusals_.Note(static_cast<std::uint8_t>(peerSlot), "a stream sent faster than twice the send rate",
                                 RefusalTally::Clock::now());
        return false;
    case ReceiveBudget::Take::Emptied: {
        // The kind of the message that emptied it, for the log, when it is a reliable long enough to name one.
        int kind = -1;
        if (type == MsgType::Reliable && len >= static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) {
            ReliableHeader rh;
            std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
            kind = rh.kind;
        }
        UE_LOGW("net: slot %d sent more than %.0f messages at once and %.0f a second, the last of type %u kind %d -- "
                "ending its link [%s]", peerSlot, receiveBudget_.Budget().burst, receiveBudget_.Budget().perSecond,
                static_cast<unsigned>(type), kind, Describe(EndReason::ReliableFlood).id);
        RecordFlood(peerSlot, hConn);
        Kick(peerSlot, EndReason::ReliableFlood, "this machine sent more than a player can");
        return false;
    }
    case ReceiveBudget::Take::Spent:
        return false;
    }
    return false;
}

void Session::RecordFlood(int slot, uint32_t hConn) {
    const uint64_t nowMs = NowMs();
    connect_history::Key key;
    if (connect_history::KeyFromAddress(hConn, key)) connect_history::Floods().Ban(key, nowMs);
    if (connect_history::KeyFromProvedGuid(ProvedGuidForSlot(slot), key)) connect_history::Floods().Ban(key, nowMs);
}

}  // namespace coop::net

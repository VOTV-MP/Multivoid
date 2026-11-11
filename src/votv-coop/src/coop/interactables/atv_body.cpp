// coop/interactables/atv_body.cpp -- this machine's own body against the host's ATV table. It owns the local seat
// and carry reads (AtvEntry::localSeated / localCarrying), the requests a client sends for them (AtvSeat,
// AtvTakeover, one request counter), the host's own player judged against the same table without the reach test, the
// yields that move this machine to what the table says, and the host's answer to a client's request (OnAnswer). The
// table itself is the host's (atv_authority.cpp), the stream and its stamp are atv_sync.cpp's. Game thread only.

#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_passenger.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey (shared)
#include "coop/player/players_registry.h"   // Registry::Local / LocalPeerId

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"          // ReleaseMainPlayerGrabIfHolding
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <string>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
namespace AU = coop::atv_authority;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using detail::g_atvs;

uint8_t  g_lastSeq      = 0;      // this client's one request counter (1..255, wrapping to 1)
uint8_t  g_passengerSeq = 0;      // the passenger request sent and not yet answered (0 = none)

// True if this peer's player is seated in `actor`, per the local engine.
bool IsLocalOccupant(void* actor, void* localPlayer) {
    return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;
}

// The next request number: one counter per client for its sits, leaves and takeovers.
uint8_t NextSeq() {
    g_lastSeq = g_lastSeq >= 255 ? uint8_t{1} : static_cast<uint8_t>(g_lastSeq + 1);
    return g_lastSeq;
}

// This machine's own body against `e`, the one place the local seat and carry are written. A rising carry
// edge owes a takeover (a falling one sends nothing: the carrier stays the syncer until the ATV settles).
void ReadLocalBody(AtvEntry& e, void* localPlayer) {
    const bool wasCarrying = e.localCarrying;
    e.localSeated = IsLocalOccupant(e.actor, localPlayer);
    e.localCarrying = !e.localSeated && IsLocalGrabber(e.actor, localPlayer);   // seated or carrying, never both
    if (e.localCarrying && !wasCarrying) e.takeoverDue = true;
    else if (!e.localCarrying) e.takeoverDue = false;
}

// The game's own dismount for a seat the host did not give this machine; no leave follows it (it was never the
// host's occupant). A dismount that cannot run is tried again once a second per entry and said once.
void YieldSeat(AtvEntry& e, const std::wstring& key, const char* why) {
    const uint64_t nowMs = NowMs();
    if (nowMs < e.yieldRetryMs) return;
    if (!A::Dismount(e.actor)) {
        e.yieldRetryMs = nowMs + kRetryMs;
        static bool sSaid = false;
        if (!sSaid) { sSaid = true;
            UE_LOGW("atv: the ATV's own dismount did not run for '%ls' (%s) -- the seat stays (said once)",
                    key.c_str(), why); }
        return;
    }
    e.yieldRetryMs = 0;
    e.sentSeated = false;
    UE_LOGI("atv: seat on '%ls' yielded -- %s", key.c_str(), why);
}

// The grav hand's grab of an ATV the host gave to another machine. A release that leaves the grab in place is
// tried again once a second per entry and said once.
void YieldCarry(AtvEntry& e, void* localPlayer, const std::wstring& key, const char* why) {
    const uint64_t nowMs = NowMs();
    if (nowMs < e.yieldRetryMs) return;
    if (ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, e.actor)) {
        e.yieldRetryMs = 0;
        UE_LOGI("atv: carry of '%ls' dropped -- %s", key.c_str(), why);
    } else if (IsLocalGrabber(e.actor, localPlayer)) {
        e.yieldRetryMs = nowMs + kRetryMs;
        static bool sSaid = false;
        if (!sSaid) { sSaid = true;
            UE_LOGW("atv: the grav hand's release did not run for '%ls' (%s) -- the carry stays (said once)",
                    key.c_str(), why); }
    }
}

// CLIENT: one request to the host, its seq the latest. The state moves only when the send did, so a refused
// send is retried by the next tick's edge.
void SendSeat(coop::net::Session& s, AtvEntry& e, const std::wstring& key, bool sit) {
    coop::net::AtvSeatPayload p{};
    WireKeyFromString(key, p.key);
    p.action = sit ? 1 : 0;
    p.seq = NextSeq();
    if (!s.SendReliable(coop::net::ReliableKind::AtvSeat, &p, sizeof(p))) return;
    e.pendingSeq = p.seq;
    e.pendingAction = sit ? AU::AtvRequest::Sit : AU::AtvRequest::Leave;
    e.sentSeated = sit;
}

void SendTakeover(coop::net::Session& s, AtvEntry& e, const std::wstring& key) {
    coop::net::AtvTakeoverPayload p{};
    WireKeyFromString(key, p.key);
    p.seq = NextSeq();
    if (!s.SendReliable(coop::net::ReliableKind::AtvTakeover, &p, sizeof(p))) return;
    e.pendingSeq = p.seq;
    e.pendingAction = AU::AtvRequest::Takeover;
    e.takeoverDue = false;
}

}  // namespace

void ResetRequestSeq() { g_lastSeq = 0; g_passengerSeq = 0; }

PassengerAsk AskPassenger(void* actor, bool sit) {
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.actor != actor || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        coop::net::Session* s = LaneSession();
        if (!s) return PassengerAsk::Failed;
        if (s->role() == coop::net::Role::Host)
            return AU::JudgeLocalPassenger(e, sit) ? PassengerAsk::Granted : PassengerAsk::Refused;
        coop::net::AtvSeatPayload p{};
        WireKeyFromString(kv.first, p.key);
        p.seat = 1;
        p.action = sit ? 1 : 0;
        p.seq = NextSeq();
        if (!s->SendReliable(coop::net::ReliableKind::AtvSeat, &p, sizeof(p))) return PassengerAsk::Failed;
        g_passengerSeq = p.seq;
        return PassengerAsk::Sent;
    }
    return PassengerAsk::Failed;
}

// HOST: the host's own body against the table, each step over every entry before the next (the map is
// unordered, so a host that leaves X and sits in Y inside one pump interval is judged leave-first). The host
// Tick runs before the pump's receive drain: a client request already in the table wins, one in the same
// frame's drain meets the host's verdict.
void JudgeHostBody(void* localPlayer) {
    for (auto& kv : g_atvs)                                  // (1) the body's seat and carry
        if (R::IsLiveByIndex(kv.second.actor, kv.second.idx)) ReadLocalBody(kv.second, localPlayer);
    for (auto& kv : g_atvs) {                                // (2) falling seat, rising seat, rising carry
        AtvEntry& e = kv.second;
        if (R::IsLiveByIndex(e.actor, e.idx) && !e.localSeated && e.sentSeated) {
            AU::JudgeLocal(e, AU::AtvRequest::Leave);
            e.sentSeated = false;
        }
    }
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (R::IsLiveByIndex(e.actor, e.idx) && e.localSeated && !e.sentSeated) {
            AU::JudgeLocal(e, AU::AtvRequest::Sit);
            e.sentSeated = true;
        }
    }
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx) || !e.takeoverDue) continue;
        e.takeoverDue = false;
        if (e.localCarrying) AU::JudgeLocal(e, AU::AtvRequest::Takeover);
    }
    for (auto& kv : g_atvs) {                                // (3) the host's yield on its own table
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        if (e.localSeated && e.occupantSlot != 0) YieldSeat(e, kv.first, "the seat is another's");
        if (e.localCarrying && e.syncerSlot != 0) YieldCarry(e, localPlayer, kv.first, "another syncs it");
    }
}

// CLIENT: this client's own body against `e`: read it, ask the host for what it did once its own world is
// ready (a level, so an edge before the gate is sent at it and never lost), then yield to what the host decided.
// The yield reads only a request this client has SENT, and only outside the window of an unanswered one, where
// only the answer moves it: a sit or grab made before the gate is neither yielded nor streamed.
void TickClientBody(coop::net::Session& s, AtvEntry& e, const std::wstring& key, void* localPlayer,
                    uint8_t localSlot, bool gateOpen) {
    ReadLocalBody(e, localPlayer);
    if (gateOpen) {
        if (e.localSeated && !e.sentSeated) SendSeat(s, e, key, true);
        // A death leaves the seat on this edge; MTA clears it server-side at PlayerWasted (CGame.cpp:2141-2165).
        else if (!e.localSeated && e.sentSeated) SendSeat(s, e, key, false);
        if (e.takeoverDue) SendTakeover(s, e, key);
    }
    if (e.pendingSeq != 0) return;
    if (e.sentSeated && e.localSeated && e.occupantSlot != localSlot)
        YieldSeat(e, key, "the host's stamp names another occupant");
    else if (e.localCarrying && !e.takeoverDue && e.syncerSlot != localSlot)
        YieldCarry(e, localPlayer, key, "the host's stamp names another syncer");
}

void OnAnswer(const coop::net::AtvAnswerPayload& a) {
    const std::wstring key = StringFromWireKey(a.key);
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) {
        UE_LOGI("atv: answer for '%ls' (seq %u) with no entry -- older, dropped", key.c_str(),
                static_cast<unsigned>(a.seq));
        return;
    }
    AtvEntry& e = it->second;
    if (a.seq != 0 && a.seq == g_passengerSeq) {
        // The answer to this client's passenger request: the table, then the passenger lane's own step.
        g_passengerSeq = 0;
        if (!R::IsLiveByIndex(e.actor, e.idx)) return;
        e.occupantSlot = a.occupantSlot;
        e.syncerSlot = a.syncerSlot;
        e.passengerSlot = a.passengerSlot;
        coop::atv_passenger::OnAnswer(e.actor, a.admitted != 0, a.reason);
        return;
    }
    if (a.seq != e.pendingSeq) {
        UE_LOGI("atv: answer for '%ls' seq %u is not the latest request (%u) -- older, logged only", key.c_str(),
                static_cast<unsigned>(a.seq), static_cast<unsigned>(e.pendingSeq));
        return;
    }
    e.pendingSeq = 0;
    // No engine call before the actor is known live: on a dead entry the window only closes, and the
    // successor reset finishes the rest.
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    e.occupantSlot = a.occupantSlot;
    e.syncerSlot   = a.syncerSlot;
    e.passengerSlot = a.passengerSlot;
    UE_LOGI("atv: host %s '%ls' seq %u (reason %u) -- seat %u, syncer %u", a.admitted ? "granted" : "refused",
            key.c_str(), static_cast<unsigned>(a.seq), static_cast<unsigned>(a.reason),
            static_cast<unsigned>(a.occupantSlot), static_cast<unsigned>(a.syncerSlot));
    switch (e.pendingAction) {
    case AU::AtvRequest::Sit:
        if (e.localSeated && a.occupantSlot != localSlot) YieldSeat(e, key, "the host refused the sit");
        break;
    case AU::AtvRequest::Takeover:
        if (a.syncerSlot != localSlot)
            YieldCarry(e, coop::players::Registry::Get().Local(), key, "the host refused the takeover");
        break;
    case AU::AtvRequest::Leave:
        break;
    }
}

}  // namespace coop::atv_sync

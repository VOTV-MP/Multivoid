// coop/interactables/atv_authority.cpp -- see coop/interactables/atv_authority.h. The host's seat and syncer table
// for the ATVs of the lane's index, the judgement of a client's AtvSeat and AtvTakeover, the admission of a syncer's
// blocks and the once-a-second pass that takes an ATV back to the host. MTA files named below are
// reference/mtasa-blue/Server/mods/deathmatch/logic/ (CGame.cpp, CUnoccupiedVehicleSync.cpp, CElement.cpp).

#include "coop/interactables/atv_authority.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::atv_authority {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
namespace EL = coop::element;
using coop::atv_sync::AtvEntry;
using coop::atv_sync::detail::g_atvs;
using coop::net::AtvRefusal;
using coop::net::IntentBudget;
using ue_wrap::FRotator;
using ue_wrap::FVector;

namespace {

// The buckets are the host's per-slot paces: a client past one costs the host no work and no answer beyond the
// refusal. A leave takes no token. The stream bucket is per entry (atv_sync_internal.h).
// The takeover pace is per sender; MTA's is per vehicle, one push per 1500 ms (CUnoccupiedVehicleSync.cpp:490).
constexpr IntentBudget kSeatBudget{4.f, 1.f};
constexpr IntentBudget kTakeoverBudget{2.f, 1.f};
constexpr IntentBudget kStreamBudget{16.f, 40.f};   // twice the 20 Hz a syncer sends

// Twice mainPlayer.armLength; the authority adds the ATV's bounds and its lag allowance.
constexpr float kReachUU = 400.f;
constexpr uint64_t kSyncerSilenceMs = 5000;   // no applied block this long: the host takes the ATV
constexpr float kSyncerDistance = 13000.f;    // MTA's default unoccupied syncer distance, 130 m
constexpr uint64_t kRestMs = 1000;            // at rest this long, uncarried: the host takes the ATV
constexpr float kRestCmS = 5.f;               // a block's linear speed under this is at rest
constexpr float kRestMovedCm = 1.f;           // and its position moved under this since the last block
constexpr uint8_t kNoSeat = 0xFF;

coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_seatBuckets;
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_takeoverBuckets;
Counters g_counters;
coop::net::RefusalTally g_refusals{"atv_authority"};

const char* ReasonName(AtvRefusal r) {
    switch (r) {
    case AtvRefusal::Admitted:      return "admitted";
    case AtvRefusal::NotWorldReady: return "not_world_ready";
    case AtvRefusal::Malformed:     return "malformed";
    case AtvRefusal::UnknownAtv:    return "unknown_atv";
    case AtvRefusal::NoSeat:        return "no_seat";
    case AtvRefusal::NotOccupant:   return "not_occupant";
    case AtvRefusal::SenderDead:    return "sender_dead";
    case AtvRefusal::SeatPaced:     return "seat_paced";
    case AtvRefusal::AlreadySeated: return "already_seated";
    case AtvRefusal::Occupied:      return "occupied";
    case AtvRefusal::NoBody:        return "no_body";
    case AtvRefusal::OutOfReach:    return "out_of_reach";
    case AtvRefusal::NoTarget:      return "no_target";
    case AtvRefusal::Seated:        return "seated";
    case AtvRefusal::Synced:        return "synced";
    case AtvRefusal::TakeoverPaced: return "takeover_paced";
    case AtvRefusal::NoDriver:      return "no_driver";
    }
    return "unknown";
}

// One refusal of a request from `slot`: counted, and said once per burst.
void Count(uint8_t slot, AtvRefusal r) {
    ++g_counters.refused[static_cast<unsigned>(r) & 31u];
    g_refusals.Note(slot, ReasonName(r), std::chrono::steady_clock::now());
}

// The verdict to its sender, with the seat and the syncer the host now holds for that ATV (0xFF and 0 without one).
// Only the requester is answered; MTA broadcasts its confirm (CGame.cpp:3196-3197), a refusal going to the sender
// alone (:3358-3366), and here the stamp tells the other clients.
void Answer(coop::net::Session& s, uint8_t slot, const coop::net::WireKey& key, uint8_t seq, AtvRefusal why,
            const AtvEntry* e) {
    coop::net::AtvAnswerPayload a{};
    a.key = key;
    a.seq = seq;
    a.admitted = why == AtvRefusal::Admitted ? 1 : 0;
    a.occupantSlot = e ? e->occupantSlot : kNoSeat;
    a.syncerSlot = e ? e->syncerSlot : uint8_t{0};
    a.passengerSlot = e ? e->passengerSlot : kNoSeat;
    a.reason = static_cast<uint8_t>(why);
    if (why == AtvRefusal::Admitted) ++g_counters.admitted; else Count(slot, why);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::AtvAnswer, &a, sizeof(a));
}

// The ATV under `key`, when the index is this world's and its actor lives and resolves as an ATV.
AtvEntry* FindLive(const std::wstring& key) {
    if (!A::EnsureResolved() || !coop::atv_sync::IndexCurrent()) return nullptr;
    auto it = g_atvs.find(key);
    if (it == g_atvs.end() || !R::IsLiveByIndex(it->second.actor, it->second.idx)) return nullptr;
    return &it->second;
}

// The tests every request meets first. Null when the request was refused (and answered, but for a sender whose
// world is not ready: a client sends none before its own, so only a forged send meets that refusal, unanswered).
AtvEntry* Common(coop::net::Session& s, uint8_t slot, const coop::net::WireKey& wk, uint8_t seq, bool illFormed) {
    if (!s.IsSlotWorldReady(slot)) { Count(slot, AtvRefusal::NotWorldReady); return nullptr; }
    const std::wstring key = coop::net::StringFromWireKey(wk);
    if (illFormed || key.empty()) { Answer(s, slot, wk, seq, AtvRefusal::Malformed, nullptr); return nullptr; }
    AtvEntry* e = FindLive(key);
    if (!e) { Answer(s, slot, wk, seq, AtvRefusal::UnknownAtv, nullptr); return nullptr; }
    return e;
}

// The sender's puppet within reach of the ATV: the reach test of a request, mapped to its refusal.
AtvRefusal ReachTest(coop::net::Session& s, uint8_t slot, void* actor) {
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(actor);
    switch (subject.outcome) {
    case EL::IntentOutcome::Ok:         return AtvRefusal::Admitted;
    case EL::IntentOutcome::NoBody:     return AtvRefusal::NoBody;
    case EL::IntentOutcome::OutOfReach: return AtvRefusal::OutOfReach;
    default:                            return AtvRefusal::NoTarget;
    }
}

// Whether `slot` sits in any ATV of the index, at the handlebars or behind them.
bool SeatedAnywhere(uint8_t slot) {
    for (const auto& kv : g_atvs)
        if ((kv.second.occupantSlot == slot || kv.second.passengerSlot == slot) &&
            R::IsLiveByIndex(kv.second.actor, kv.second.idx))
            return true;
    return false;
}

// The table's tests for a passenger sit by `who`: one seat at a time, the spot free, and another player at the
// handlebars. The spot is the riding kerfur's; a kerfur there gets off for the player (atv_riders.cpp).
AtvRefusal PassengerTests(const AtvEntry& e, uint8_t who) {
    if (SeatedAnywhere(who)) return AtvRefusal::AlreadySeated;
    if (e.passengerSlot != kNoSeat) return AtvRefusal::Occupied;
    if (e.occupantSlot == kNoSeat || e.occupantSlot == who) return AtvRefusal::NoDriver;
    return AtvRefusal::Admitted;
}

// The table's tests for a sit by `who`: one seat at a time, and the seat free. `already_seated` comes first, as
// MTA's one-vehicle-per-player does (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3111, refused
// FAIL_IN_VEHICLE :3347). MTA lets a player jack an occupied vehicle (:3130-3132); VOTV's sit has no jack, so an
// occupied seat is refused.
AtvRefusal SitTests(const AtvEntry& e, uint8_t who) {
    // The developer red arm of the ATV health drill: the occupied-seat test is skipped, so the drill's `judge` phase
    // must fail. Resolved once.
    static const bool s_redOccupied =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "occupied";
    if (SeatedAnywhere(who)) return AtvRefusal::AlreadySeated;
    if (!s_redOccupied && e.occupantSlot != kNoSeat) return AtvRefusal::Occupied;
    return AtvRefusal::Admitted;
}

// The table's tests for a takeover by `who`: nobody sits in the ATV (a carrier never takes a driven one, and a
// sit takes a carried one), and nobody else carries it: another client whose last block had the grabbed bit, or the
// host's own player. No carry is stolen; a settling or parked ATV yields to a grab. A takeover is a grab, not
// MTA's push by contact (CUnoccupiedVehicleSync.cpp:480-504), and that push steals from any syncer that is not
// persistent (:499-502).
AtvRefusal TakeoverTests(const AtvEntry& e, uint8_t who) {
    // The developer red arm of the ATV health drill: the host-carry clause alone is skipped, so the drill's `judge`
    // phase must fail. Resolved once.
    static const bool s_redTakeover =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "takeover";
    // The developer red arm of the ATV health drill's synced_peer: the client-carry clause alone is skipped, so the
    // second client's takeover of a carried ATV is granted and the drill's `judge` phase must fail. Resolved once.
    static const bool s_redPeerCarry =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "peer_carry";
    if (e.occupantSlot != kNoSeat) return AtvRefusal::Seated;
    if (!s_redPeerCarry && e.syncerSlot != 0 && e.syncerSlot != who && e.lastGrabbed) return AtvRefusal::Synced;
    if (!s_redTakeover && who != 0 &&
        coop::atv_sync::IsLocalGrabber(e.actor, coop::players::Registry::Get().Local()))
        return AtvRefusal::Synced;
    return AtvRefusal::Admitted;
}

// A grant: the syncer becomes `who` (and the occupant too for a sit), with the clocks of a fresh syncer, so neither
// the silence sweep nor the hand-back can undo it before its first block.
void Grant(AtvEntry& e, uint8_t who, bool seat, bool carried, uint64_t nowMs) {
    if (seat) e.occupantSlot = who;
    e.syncerSlot = who;
    e.lastBlockMs = nowMs;
    e.restSinceMs = 0;
    e.haveRestPos = false;
    e.lastGrabbed = carried;
    e.streamBucket.Reset();
    e.haveSyncerHealth = false;
    e.stampDue = true;
}

// A syncer change to the host, by range, rest or silence: `prev` is who held it.
void HandToHost(AtvEntry& e, uint8_t prev, uint64_t nowMs) {
    e.prevSyncerSlot = prev;
    e.handBackMs = nowMs;
    e.syncerSlot = 0;
    e.streamBucket.Reset();
    e.haveSyncerHealth = false;
    e.stampDue = true;
}

}  // namespace

void OnSeat(const coop::net::AtvSeatPayload& p, uint8_t slot) {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    if (!s) return;
    AtvEntry* e = Common(*s, slot, p.key, p.seq, p.action > 1);
    if (!e) return;
    if (p.seat > 1) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NoSeat, e);
    const uint64_t nowMs = coop::atv_sync::NowMs();
    if (p.seat == 1 && p.action == 0) {
        // A passenger's leave, as the driver's: no token, and nothing else moves.
        if (e->passengerSlot != slot) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NotOccupant, e);
        e->passengerSlot = kNoSeat;
        e->stampDue = true;
        return Answer(*s, slot, p.key, p.seq, AtvRefusal::Admitted, e);
    }
    if (p.action == 0) {
        // A leave from the occupant clears the seat and takes no token; the syncer keeps its place.
        if (e->occupantSlot != slot) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NotOccupant, e);
        e->occupantSlot = kNoSeat;
        e->stampDue = true;
        return Answer(*s, slot, p.key, p.seq, AtvRefusal::Admitted, e);
    }
    // MTA refuses an entry request from a player that is not spawned
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3095); no puppet is no body to measure from.
    const coop::players::PuppetLife life = coop::players::Registry::Get().LifeOf(slot);
    if (life == coop::players::PuppetLife::Dead) return Answer(*s, slot, p.key, p.seq, AtvRefusal::SenderDead, e);
    if (life == coop::players::PuppetLife::None) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NoBody, e);
    if (!g_seatBuckets[slot].Take(kSeatBudget, nowMs))
        return Answer(*s, slot, p.key, p.seq, AtvRefusal::SeatPaced, e);
    if (p.seat == 1) {
        // A passenger takes the spot alone: the driver keeps the seat and the syncer.
        AtvRefusal why = PassengerTests(*e, slot);
        if (why == AtvRefusal::Admitted) why = ReachTest(*s, slot, e->actor);
        if (why == AtvRefusal::Admitted) { e->passengerSlot = slot; e->stampDue = true; }
        return Answer(*s, slot, p.key, p.seq, why, e);
    }
    AtvRefusal why = SitTests(*e, slot);
    if (why == AtvRefusal::Admitted) why = ReachTest(*s, slot, e->actor);
    if (why == AtvRefusal::Admitted) Grant(*e, slot, /*seat*/true, /*carried*/false, nowMs);
    Answer(*s, slot, p.key, p.seq, why, e);
}

void OnTakeover(const coop::net::AtvTakeoverPayload& p, uint8_t slot) {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    if (!s) return;
    AtvEntry* e = Common(*s, slot, p.key, p.seq, false);
    if (!e) return;
    const uint64_t nowMs = coop::atv_sync::NowMs();
    AtvRefusal why = TakeoverTests(*e, slot);
    if (why == AtvRefusal::Admitted) {
        if (e->syncerSlot == slot) {
            // The syncer's own grab: admitted with no token, the rest clock restarted and the ATV marked carried,
            // so a grab racing a hand-back is never lost.
            e->restSinceMs = 0;
            e->lastGrabbed = true;
        } else if (!g_takeoverBuckets[slot].Take(kTakeoverBudget, nowMs)) {
            why = AtvRefusal::TakeoverPaced;
        } else {
            why = ReachTest(*s, slot, e->actor);
            if (why == AtvRefusal::Admitted) Grant(*e, slot, /*seat*/false, /*carried*/true, nowMs);
        }
    }
    Answer(*s, slot, p.key, p.seq, why, e);
}

bool JudgeLocal(AtvEntry& e, AtvRequest req) {
    const uint64_t nowMs = coop::atv_sync::NowMs();
    switch (req) {
    case AtvRequest::Leave:
        if (e.occupantSlot != 0) return false;
        e.occupantSlot = kNoSeat;
        e.stampDue = true;
        return true;
    case AtvRequest::Sit:
        if (SitTests(e, 0) != AtvRefusal::Admitted) return false;
        Grant(e, 0, /*seat*/true, /*carried*/false, nowMs);
        return true;
    case AtvRequest::Takeover:
        if (TakeoverTests(e, 0) != AtvRefusal::Admitted) return false;
        if (e.syncerSlot == 0) {
            e.restSinceMs = 0;
            e.lastGrabbed = true;
        } else {
            Grant(e, 0, /*seat*/false, /*carried*/true, nowMs);
        }
        return true;
    }
    return false;
}

bool JudgeLocalPassenger(AtvEntry& e, bool sit) {
    if (!sit) {
        if (e.passengerSlot != 0) return false;
        e.passengerSlot = kNoSeat;
        e.stampDue = true;
        return true;
    }
    if (PassengerTests(e, 0) != AtvRefusal::Admitted) return false;
    e.passengerSlot = 0;
    e.stampDue = true;
    return true;
}

bool IsLateSyncer(const AtvEntry& e, uint8_t slot, uint64_t nowMs) {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    const int rtt = s ? s->rttMsForSlot(slot) : -1;
    const uint64_t window = static_cast<uint64_t>(rtt > 0 ? rtt : 0) + 2 * coop::atv_sync::kDriveSendMs;
    return e.handBackMs != 0 && slot == e.prevSyncerSlot && nowMs - e.handBackMs <= window;
}

bool AdmitBlock(AtvEntry& e, const coop::net::AtvStatePayload& p, uint8_t senderSlot, uint64_t nowMs) {
    // No time context: the lane is FIFO and no host code repositions a client-synced ATV. MTA's context guards a
    // server move against a syncer's packet in flight (CUnoccupiedVehicleSync.cpp:285, CElement.cpp:1281-1305).
    // The developer red arm of the hostile drill's atv_state_forge: the sender test is skipped, so a forged block is
    // not counted `not_syncer` and applies, and the drill's table must fail. Resolved once.
    static const bool s_redNotSyncer =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "not_syncer";
    if (!s_redNotSyncer && (senderSlot == 0 || senderSlot != e.syncerSlot)) {
        // The honest race: the block of the syncer a hand-back just replaced, sent before the stamp reached it.
        if (IsLateSyncer(e, senderSlot, nowMs))
            ++g_counters.lateSyncer;
        else
            ++g_counters.notSyncer;
        return false;
    }
    if (!e.streamBucket.Take(kStreamBudget, nowMs)) {
        ++g_counters.streamPaced;
        return false;
    }
    e.lastBlockMs = nowMs;
    // The rest test reads the pose that moved and the velocity: a block whose velocity did not read carries zeros,
    // so the pose is the one that decides.
    const FVector pos{p.x, p.y, p.z};
    const FVector lin{p.linVelX, p.linVelY, p.linVelZ};
    const bool grabbedBit = (p.stateBits & 0x4) != 0;
    // The developer red arm of the ATV health drill's carry_still: the grab bit is ignored, so a carry standing still
    // is handed back while held and the drill's `held` phase must fail. It clears the bit for the rest test AND for
    // lastGrabbed below, since the hand-back holds two redundant grab terms (the rest test and !lastGrabbed in
    // RunPass); clearing one would leave the other blocking it. Resolved once.
    static const bool s_redCarryBack =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "carry_back";
    const bool grabbed = grabbedBit && !s_redCarryBack;
    const float speed = coop::atv_sync::Len(lin);
    bool motionStill = false;
    if (e.haveRestPos) {
        const FVector d{pos.X - e.lastRestPos.X, pos.Y - e.lastRestPos.Y, pos.Z - e.lastRestPos.Z};
        motionStill = speed < kRestCmS && coop::atv_sync::Len(d) < kRestMovedCm;
    }
    const bool still = motionStill && !grabbed;
    if (motionStill && grabbedBit) ++g_counters.carriedStillBlocks;
    e.lastBlockSpeed = speed;
    e.restSinceMs = !still ? 0 : (e.restSinceMs != 0 ? e.restSinceMs : nowMs);
    e.lastRestPos = pos;
    e.haveRestPos = true;
    e.lastGrabbed = grabbed;
    if (e.occupantSlot == kNoSeat && !e.lastGrabbed) ++g_counters.idleStreamBlocks;
    return true;
}

void RunPass(uint64_t nowMs) {
    // The developer red arm of the ATV health drill: the at-rest hand-back is skipped (range and silence stay), so
    // the drill's `back` phase must fail. `silence` skips the silence rule alone, so the `silenced` phase must fail.
    // Each resolved once.
    static const bool s_redBack =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "back";
    static const bool s_redSilence =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "silence";
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.syncerSlot == 0 || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        const uint8_t syncer = e.syncerSlot;
        // 1. silent: the host takes the ATV, and the seat with it when that client held it. MTA's syncer sends only
        // on change (CUnoccupiedVehicleSync.cpp:459), so silence is normal there; here a syncer streams.
        if (!s_redSilence && nowMs - e.lastBlockMs >= kSyncerSilenceMs) {
            if (e.occupantSlot == syncer) e.occupantSlot = kNoSeat;
            HandToHost(e, syncer, nowMs);
            ++g_counters.silenced;
            UE_LOGI("atv_authority: slot %u sent no block for %llu ms -- '%ls' is the host's",
                    static_cast<unsigned>(syncer), static_cast<unsigned long long>(kSyncerSilenceMs),
                    kv.first.c_str());
            continue;
        }
        // 2. seated: the syncer is the occupant.
        if (e.occupantSlot != kNoSeat) continue;
        // 3. out of range, or at rest and uncarried: the host takes the ATV. MTA keeps an in-range syncer and hands an
        // out-of-range vehicle to the nearest player (CUnoccupiedVehicleSync.cpp:127-146, :171-192); here the host's
        // world is always loaded, and an ATV at rest returns to it, which MTA has no rule for.
        FVector atvAt; FRotator rot;
        if (!A::GetRootTransform(e.actor, atvAt, rot)) continue;
        FVector at{};
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(syncer);
        const bool located = rp && rp->TryGetLocation(at);
        const FVector d{at.X - atvAt.X, at.Y - atvAt.Y, at.Z - atvAt.Z};
        // A puppet whose location does not read is not out of range: departure and silence cover a gone client.
        const bool outOfRange = located && !(coop::atv_sync::Len(d) <= kSyncerDistance);
        const bool atRest = !s_redBack && e.restSinceMs != 0 && nowMs - e.restSinceMs >= kRestMs && !e.lastGrabbed;
        if (outOfRange || atRest) {
            HandToHost(e, syncer, nowMs);
            ++g_counters.handedBack;
            UE_LOGI("atv_authority: '%ls' is the host's again (slot %u: %s)", kv.first.c_str(),
                    static_cast<unsigned>(syncer), outOfRange ? "out of range" : "at rest");
        }
    }
    g_refusals.Flush(std::chrono::steady_clock::now());
}

void ClearTable(AtvEntry& e) {
    e.occupantSlot = kNoSeat;
    e.passengerSlot = kNoSeat;
    e.syncerSlot = 0;
    e.lastBlockMs = 0;
    e.restSinceMs = 0;
    e.handBackMs = 0;
    e.prevSyncerSlot = 0xFF;
    e.haveRestPos = false;
    e.streamBucket.Reset();
    e.hostTrap = false;
    e.stampTrap = false;
    e.haveStampTrap = false;
    e.copyTrapSeen = false;
    e.blown = false;
    e.blownBy = 0xFF;
    e.haveSyncerHealth = false;
    e.stampDue = true;
}

void ResetRequests(AtvEntry& e) {
    // sentSeated stays: it is what the host believes of this seat, so a client no longer seated sends its leave.
    e.pendingSeq = 0;
    e.takeoverDue = false;
}

Counters ReadCounters() { return g_counters; }

void LogSummary() {
    unsigned long long refusedTotal = 0;
    char byReason[320] = {};
    size_t used = 0;
    for (unsigned i = 1; i < 32; ++i) {
        const unsigned long long n = g_counters.refused[i];
        if (n == 0) continue;
        refusedTotal += n;
        const int w = std::snprintf(byReason + used, sizeof(byReason) - used, used ? " %s=%llu" : "%s=%llu",
                                    ReasonName(static_cast<AtvRefusal>(i)), n);
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(byReason)) used += static_cast<size_t>(w);
    }
    if (g_counters.admitted == 0 && refusedTotal == 0 && g_counters.notSyncer == 0 && g_counters.lateSyncer == 0 &&
        g_counters.streamPaced == 0 && g_counters.handedBack == 0 && g_counters.silenced == 0 &&
        g_counters.idleStreamBlocks == 0 && g_counters.carriedStillBlocks == 0)
        return;
    UE_LOGI("atv_authority: session -- %llu admitted, %llu refused (%s); blocks: %llu not_syncer / %llu late_syncer / "
            "%llu stream_paced / %llu idle_stream / %llu carried_still; taken back: %llu handed_back / %llu silenced",
            g_counters.admitted, refusedTotal, byReason, g_counters.notSyncer, g_counters.lateSyncer,
            g_counters.streamPaced, g_counters.idleStreamBlocks, g_counters.carriedStillBlocks, g_counters.handedBack,
            g_counters.silenced);
}

void Reset() {
    g_counters = Counters{};
    for (int i = 0; i < g_seatBuckets.size(); ++i) g_seatBuckets[i].Reset();
    for (int i = 0; i < g_takeoverBuckets.size(); ++i) g_takeoverBuckets[i].Reset();
    g_refusals.Clear();
}

}  // namespace coop::atv_authority

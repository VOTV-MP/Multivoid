// coop/interactables/atv_authority.cpp -- see coop/interactables/atv_authority.h. The host's seat and syncer table
// for the ATVs of the lane's index, the judgement of a client's AtvSeat and AtvTakeover, the admission of a syncer's
// blocks and the once-a-second pass that takes an ATV back to the host. MTA files named below are
// reference/mtasa-blue/Server/mods/deathmatch/logic/ (CGame.cpp, CUnoccupiedVehicleSync.cpp, CElement.cpp).

#include "coop/interactables/atv_authority.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/movement_ledger.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_passenger.h"

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
constexpr IntentBudget kPassengerBudget{4.f, 1.f};   // a board takes a token, a leave none
constexpr IntentBudget kStreamBudget{16.f, 40.f};   // twice the 20 Hz a syncer sends

// Twice mainPlayer.armLength; the authority adds the ATV's bounds and its lag allowance.
constexpr float kReachUU = 400.f;
constexpr uint64_t kSyncerSilenceMs = 5000;   // no applied block this long: the host takes the ATV
constexpr float kSyncerDistance = 13000.f;    // MTA's default unoccupied syncer distance, 130 m
constexpr uint64_t kRestMs = 1000;            // at rest this long, uncarried: the host takes the ATV
constexpr float kRestCmS = 5.f;               // a block's linear speed under this is at rest
constexpr float kRestMovedCm = 1.f;           // and its position moved under this since the last block
constexpr uint8_t kNoSeat = 0xFF;

// The passenger seats: the table's width is the wire's and the engine wrapper's, and one more than the other players of a full session.
constexpr int kPassengerSeats = coop::net::kAtvPassengerSeats;
static_assert(kPassengerSeats <= coop::players::kMaxPeers - 1, "the passenger seats never outnumber the other players of a full session; a request beyond them is refused Full");
static_assert(kPassengerSeats == ue_wrap::atv_passenger::kSeats, "the wire and the wrapper agree on the seat count");
static_assert(sizeof(coop::atv_sync::AtvEntry::passengerSlot) == kPassengerSeats, "the entry's table is the wire's width");
// A rider whose claimed position is this far from its seat for this long is not on the ATV any more: the host takes it
// out. A client's ATV copy trails the host's by the corrector's bound and a rider's pose is its own copy's, so a seat
// may be a few metres from where the host's copy puts it while the rig is at speed; a teleport the rider's own machine
// saw has already sent its leave, so this is the backstop for a rider that could not.
constexpr float kRiderStrayUU = 1500.f;
constexpr uint64_t kRiderStrayMs = 2000;

coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_seatBuckets;
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_takeoverBuckets;
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_passengerBuckets;
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
    }
    return "unknown";
}

const char* PassengerReasonName(coop::net::AtvPassengerRefusal r) {
    using R = coop::net::AtvPassengerRefusal;
    switch (r) {
    case R::Admitted:      return "admitted";
    case R::NotWorldReady: return "not_world_ready";
    case R::Malformed:     return "malformed";
    case R::UnknownAtv:    return "unknown_atv";
    case R::SenderDead:    return "sender_dead";
    case R::NoBody:        return "no_body";
    case R::Paced:         return "paced";
    case R::AlreadySeated: return "already_seated";
    case R::Full:          return "full";
    case R::OutOfReach:    return "out_of_reach";
    case R::NoTarget:      return "no_target";
    case R::NotPassenger:  return "not_passenger";
    case R::Count:         break;
    }
    return "unknown";
}

// One refusal of a request from `slot`: counted, and said once per burst.
void Count(uint8_t slot, AtvRefusal r) {
    ++g_counters.refused[static_cast<unsigned>(r) & 15u];
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
    a.occupantSlot = e ? e->seatOccupant : kNoSeat;
    a.syncerSlot = e ? e->seatSyncer : uint8_t{0};
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

// Whether `slot` sits in any ATV of the index, in its driver's seat or a passenger's.
bool SeatedAnywhere(uint8_t slot) {
    for (const auto& kv : g_atvs) {
        if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx)) continue;
        if (kv.second.seatOccupant == slot || PassengerSeatOf(kv.second, slot) >= 0) return true;
    }
    return false;
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
    if (!s_redOccupied && e.seatOccupant != kNoSeat) return AtvRefusal::Occupied;
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
    if (e.seatOccupant != kNoSeat) return AtvRefusal::Seated;
    if (!s_redPeerCarry && e.seatSyncer != 0 && e.seatSyncer != who && e.lastGrabbed) return AtvRefusal::Synced;
    if (!s_redTakeover && who != 0 &&
        coop::atv_sync::IsLocalGrabber(e.actor, coop::players::Registry::Get().Local()))
        return AtvRefusal::Synced;
    return AtvRefusal::Admitted;
}

// A grant: the syncer becomes `who` (and the occupant too for a sit), with the clocks of a fresh syncer, so neither
// the silence sweep nor the hand-back can undo it before its first block.
void Grant(AtvEntry& e, uint8_t who, bool seat, bool carried, uint64_t nowMs) {
    e.lastBlockMs = nowMs;
    e.restSinceMs = 0;
    e.haveRestPos = false;
    e.lastGrabbed = carried;
    e.streamBucket.Reset();
    e.haveSyncerHealth = false;
    coop::atv_seat_table::SetSeat(e, seat ? who : e.seatOccupant, who, "grant", /*forgetHorn*/true);
    e.stampDue = true;
}

// A syncer change to the host, by range, rest or silence: `prev` is who held it.
void HandToHost(AtvEntry& e, uint8_t prev, uint64_t nowMs) {
    e.prevSyncerSlot = prev;
    e.handBackMs = nowMs;
    e.streamBucket.Reset();
    e.haveSyncerHealth = false;
    coop::atv_seat_table::SetSeat(e, e.seatOccupant, 0, "hand back", /*forgetHorn*/true);
}

}  // namespace

void OnSeat(const coop::net::AtvSeatPayload& p, uint8_t slot) {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    if (!s) return;
    AtvEntry* e = Common(*s, slot, p.key, p.seq, p.action > 1);
    if (!e) return;
    if (p.seat != 0) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NoSeat, e);
    const uint64_t nowMs = coop::atv_sync::NowMs();
    if (p.action == 0) {
        // A leave from the occupant clears the seat and takes no token; the syncer keeps its place.
        if (e->seatOccupant != slot) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NotOccupant, e);
        coop::atv_seat_table::SetSeat(*e, kNoSeat, e->seatSyncer, "leave", /*forgetHorn*/false);
        return Answer(*s, slot, p.key, p.seq, AtvRefusal::Admitted, e);
    }
    // MTA refuses an entry request from a player that is not spawned
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3095); no puppet is no body to measure from.
    const coop::players::PuppetLife life = coop::players::Registry::Get().LifeOf(slot);
    if (life == coop::players::PuppetLife::Dead) return Answer(*s, slot, p.key, p.seq, AtvRefusal::SenderDead, e);
    if (life == coop::players::PuppetLife::None) return Answer(*s, slot, p.key, p.seq, AtvRefusal::NoBody, e);
    if (!g_seatBuckets[slot].Take(kSeatBudget, nowMs))
        return Answer(*s, slot, p.key, p.seq, AtvRefusal::SeatPaced, e);
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
        if (e->seatSyncer == slot) {
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

int PassengerSeatOf(const AtvEntry& e, uint8_t slot) {
    if (slot == kNoSeat) return -1;
    for (int i = 0; i < kPassengerSeats; ++i)
        if (e.passengerSlot[i] == slot) return i;
    return -1;
}

int ClearPassenger(uint8_t slot) {
    int cleared = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        for (int i = 0; i < kPassengerSeats; ++i) {
            if (e.passengerSlot[i] != slot) continue;
            e.passengerSlot[i] = kNoSeat;
            e.passengerAwayMs[i] = 0;
            e.passengerDue = true;
            ++cleared;
        }
    }
    return cleared;
}

namespace {

// The verdict to its sender, with the seat it holds on that ATV after it (0xFF = none).
void PassengerAnswer(coop::net::Session& s, uint8_t slot, const coop::net::WireKey& key, uint8_t seq,
                     coop::net::AtvPassengerRefusal why, const AtvEntry* e) {
    using Refusal = coop::net::AtvPassengerRefusal;
    coop::net::AtvPassengerAnswerPayload a{};
    a.key = key;
    a.seq = seq;
    a.admitted = why == Refusal::Admitted ? 1 : 0;
    const int held = e ? PassengerSeatOf(*e, slot) : -1;
    a.seat = held >= 0 ? static_cast<uint8_t>(held) : kNoSeat;
    a.reason = static_cast<uint8_t>(why);
    if (why == Refusal::Admitted) {
        ++g_counters.passengerAdmitted;
    } else {
        ++g_counters.passengerRefused[static_cast<unsigned>(why) & 15u];
        g_refusals.Note(slot, PassengerReasonName(why), std::chrono::steady_clock::now());
    }
    s.SendReliableToSlot(slot, coop::net::ReliableKind::AtvPassengerAnswer, &a, sizeof(a));
}

int FirstFreeSeat(const AtvEntry& e) {
    for (int i = 0; i < kPassengerSeats; ++i)
        if (e.passengerSlot[i] == kNoSeat) return i;
    return -1;
}

// A board by `who`: one seat at a time anywhere, then a free passenger seat. The driver's seat is no test: a passenger
// may board an empty ATV, and a driver's leaving ejects nobody.
coop::net::AtvPassengerRefusal BoardTests(const AtvEntry& e, uint8_t who, int* seat) {
    using Refusal = coop::net::AtvPassengerRefusal;
    if (SeatedAnywhere(who)) return Refusal::AlreadySeated;
    *seat = FirstFreeSeat(e);
    return *seat >= 0 ? Refusal::Admitted : Refusal::Full;
}

// HOST, from RunPass: a rider whose body is dead is out, and one whose claimed position stays far from its seat (the
// ledger's accepted position, which a puppet pinned to the seat cannot say) is out after a while: its machine has left
// the ATV without being able to say so. The host's own player is its own machine's to leave. Each change is marked for a
// send.
void SweepPassengers(uint64_t nowMs) {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    if (!s) return;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        for (int i = 0; i < kPassengerSeats; ++i) {
            const uint8_t who = e.passengerSlot[i];
            if (who == kNoSeat || who == 0) continue;
            const char* why = nullptr;
            if (coop::players::Registry::Get().LifeOf(who) == coop::players::PuppetLife::Dead) {
                why = "dead";
            } else {
                FVector seatAt{}, claimed{};
                if (ue_wrap::atv_passenger::SeatWorldLocation(e.actor, i, seatAt) &&
                    coop::movement_ledger::TryGetAcceptedPosition(*s, who, claimed)) {
                    const FVector d{claimed.X - seatAt.X, claimed.Y - seatAt.Y, claimed.Z - seatAt.Z};
                    if (coop::atv_sync::Len(d) > kRiderStrayUU) {
                        if (e.passengerAwayMs[i] == 0) e.passengerAwayMs[i] = nowMs;
                        else if (nowMs - e.passengerAwayMs[i] >= kRiderStrayMs) why = "far from its seat";
                    } else {
                        e.passengerAwayMs[i] = 0;
                    }
                }
            }
            if (!why) continue;
            e.passengerSlot[i] = kNoSeat;
            e.passengerAwayMs[i] = 0;
            e.passengerDue = true;
            ++g_counters.passengerSwept;
            UE_LOGI("atv_authority: slot %u is out of passenger seat %d of '%ls' (%s)", static_cast<unsigned>(who), i,
                    kv.first.c_str(), why);
        }
    }
}

}  // namespace

void OnPassengerRequest(const coop::net::AtvPassengerRequestPayload& p, uint8_t slot) {
    using Refusal = coop::net::AtvPassengerRefusal;
    coop::net::Session* s = coop::atv_sync::LaneSession();
    if (!s) return;
    if (!s->IsSlotWorldReady(slot)) {
        ++g_counters.passengerRefused[static_cast<unsigned>(Refusal::NotWorldReady)];
        g_refusals.Note(slot, PassengerReasonName(Refusal::NotWorldReady), std::chrono::steady_clock::now());
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.action > 1 || key.empty()) return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::Malformed, nullptr);
    AtvEntry* e = FindLive(key);
    if (!e) return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::UnknownAtv, nullptr);
    if (p.action == 0) {
        // A leave from a rider frees its seat and takes no token; the driver and the syncer keep their places.
        const int held = PassengerSeatOf(*e, slot);
        if (held < 0) return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::NotPassenger, e);
        e->passengerSlot[held] = kNoSeat;
        e->passengerAwayMs[held] = 0;
        e->passengerDue = true;
        return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::Admitted, e);
    }
    const coop::players::PuppetLife life = coop::players::Registry::Get().LifeOf(slot);
    if (life == coop::players::PuppetLife::Dead) return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::SenderDead, e);
    if (life == coop::players::PuppetLife::None) return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::NoBody, e);
    if (!g_passengerBuckets[slot].Take(kPassengerBudget, coop::atv_sync::NowMs()))
        return PassengerAnswer(*s, slot, p.key, p.seq, Refusal::Paced, e);
    int seat = -1;
    Refusal why = BoardTests(*e, slot, &seat);
    if (why == Refusal::Admitted) {
        switch (ReachTest(*s, slot, e->actor)) {
        case AtvRefusal::Admitted:   break;
        case AtvRefusal::NoBody:     why = Refusal::NoBody; break;
        case AtvRefusal::OutOfReach: why = Refusal::OutOfReach; break;
        default:                     why = Refusal::NoTarget; break;
        }
    }
    if (why == Refusal::Admitted) {
        e->passengerSlot[seat] = slot;
        e->passengerAwayMs[seat] = 0;
        e->passengerDue = true;
    }
    PassengerAnswer(*s, slot, p.key, p.seq, why, e);
}

bool JudgePassengerLocal(AtvEntry& e, bool board, int* seat) {
    if (!board) {
        const int held = PassengerSeatOf(e, 0);
        if (held < 0) return false;
        e.passengerSlot[held] = kNoSeat;
        e.passengerAwayMs[held] = 0;
        e.passengerDue = true;
        return true;
    }
    int got = -1;
    if (BoardTests(e, 0, &got) != coop::net::AtvPassengerRefusal::Admitted) return false;
    e.passengerSlot[got] = 0;
    e.passengerAwayMs[got] = 0;
    e.passengerDue = true;
    if (seat) *seat = got;
    return true;
}

void DrillSetPassengerGhosts(const std::wstring& key, bool on) {
    AtvEntry* e = FindLive(key);
    if (!e) return;
    static const uint8_t kGhosts[kPassengerSeats] = {2, 3, 0};
    for (int i = 0; i < kPassengerSeats; ++i) {
        if (on && e->passengerSlot[i] == kNoSeat) e->passengerSlot[i] = kGhosts[i];
        else if (!on && e->passengerSlot[i] == kGhosts[i]) e->passengerSlot[i] = kNoSeat;
    }
}

bool JudgeLocal(AtvEntry& e, AtvRequest req) {
    const uint64_t nowMs = coop::atv_sync::NowMs();
    switch (req) {
    case AtvRequest::Leave:
        if (e.seatOccupant != 0) return false;
        coop::atv_seat_table::SetSeat(e, kNoSeat, e.seatSyncer, "host leave", /*forgetHorn*/false);
        return true;
    case AtvRequest::Sit:
        if (SitTests(e, 0) != AtvRefusal::Admitted) return false;
        Grant(e, 0, /*seat*/true, /*carried*/false, nowMs);
        return true;
    case AtvRequest::Takeover:
        if (TakeoverTests(e, 0) != AtvRefusal::Admitted) return false;
        if (e.seatSyncer == 0) {
            e.restSinceMs = 0;
            e.lastGrabbed = true;
        } else {
            Grant(e, 0, /*seat*/false, /*carried*/true, nowMs);
        }
        return true;
    }
    return false;
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
    if (!s_redNotSyncer && (senderSlot == 0 || senderSlot != e.seatSyncer)) {
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
    if (e.seatOccupant == kNoSeat && !e.lastGrabbed) ++g_counters.idleStreamBlocks;
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
    SweepPassengers(nowMs);
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.seatSyncer == 0 || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        const uint8_t syncer = e.seatSyncer;
        // 1. silent: the host takes the ATV, and the seat with it when that client held it. MTA's syncer sends only
        // on change (CUnoccupiedVehicleSync.cpp:459), so silence is normal there; here a syncer streams.
        const bool silent = nowMs - e.lastBlockMs >= kSyncerSilenceMs;
        if (silent) ++g_counters.silentSeen;
        if (!s_redSilence && silent) {
            if (e.seatOccupant == syncer)
                coop::atv_seat_table::SetSeat(e, kNoSeat, e.seatSyncer, "silent syncer", /*forgetHorn*/false);
            HandToHost(e, syncer, nowMs);
            ++g_counters.silenced;
            UE_LOGI("atv_authority: slot %u sent no block for %llu ms -- '%ls' is the host's",
                    static_cast<unsigned>(syncer), static_cast<unsigned long long>(kSyncerSilenceMs),
                    kv.first.c_str());
            continue;
        }
        // 2. seated: the syncer is the occupant.
        if (e.seatOccupant != kNoSeat) continue;
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
    coop::atv_seat_table::SetSeat(e, kNoSeat, 0, "table cleared", /*forgetHorn*/true);
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
    for (unsigned i = 1; i < 16; ++i) {
        const unsigned long long n = g_counters.refused[i];
        if (n == 0) continue;
        refusedTotal += n;
        const int w = std::snprintf(byReason + used, sizeof(byReason) - used, used ? " %s=%llu" : "%s=%llu",
                                    ReasonName(static_cast<AtvRefusal>(i)), n);
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(byReason)) used += static_cast<size_t>(w);
    }
    unsigned long long passRefused = 0;
    char passBy[240] = {};
    size_t passUsed = 0;
    for (unsigned i = 1; i < 16; ++i) {
        const unsigned long long n = g_counters.passengerRefused[i];
        if (n == 0) continue;
        passRefused += n;
        const int w = std::snprintf(passBy + passUsed, sizeof(passBy) - passUsed, passUsed ? " %s=%llu" : "%s=%llu",
                                    PassengerReasonName(static_cast<coop::net::AtvPassengerRefusal>(i)), n);
        if (w > 0 && passUsed + static_cast<size_t>(w) < sizeof(passBy)) passUsed += static_cast<size_t>(w);
    }
    if (g_counters.passengerAdmitted != 0 || passRefused != 0 || g_counters.passengerSwept != 0)
        UE_LOGI("atv_authority: passengers -- %llu admitted, %llu refused (%s), %llu swept", g_counters.passengerAdmitted,
                passRefused, passBy, g_counters.passengerSwept);
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
    for (int i = 0; i < g_passengerBuckets.size(); ++i) g_passengerBuckets[i].Reset();
    g_refusals.Clear();
}

}  // namespace coop::atv_authority

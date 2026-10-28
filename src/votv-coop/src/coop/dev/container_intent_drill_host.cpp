// coop/dev/container_intent_drill_host.cpp -- the HOST's half of the container intent drill; see
// coop/dev/container_intent_drill.h.

#include "container_intent_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_truth.h"
#include "coop/props/container_record_edit.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace AH = coop::props::container_act_host;
namespace AS = coop::props::container_act_state;
namespace CV = ue_wrap::container_view;
namespace CRE = coop::props::container_record_edit;
namespace CC = coop::props::container_truth;
namespace DC = coop::dev::drill_chat;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FVector;

constexpr uint64_t kHostFailMs   = 90000;   // the host sees the act land within this of its count first moving
constexpr uint64_t kHostReadyMs  = 300000;  // or, if the count never moves, within this of the first world-ready peer
constexpr uint64_t kHostQuietMs  = 3000;    // the act's end state stands this long, with no hold open
constexpr uint64_t kHoldsEveryMs = 1000;
constexpr uint64_t kRestoreMinMs = 29000;   // deadline: the host's restore comes this long after its grant, at least
constexpr uint64_t kRestoreMaxMs = 36000;   // and this long at most
constexpr uint64_t kOpenFailMs   = 10000;   // race: the screen shows the container within this after openPropInv
constexpr float    kRoomNearCm   = 500.f;   // room: the props of R's class counted around the client's puppet

// The red: the grant leaves the record in the container (container_act_no_escrow).
bool NoEscrowRed() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::container_act_no_escrow);
    return on;
}

// The host.
uint64_t g_readyMs = 0;     // the first tick with a world-ready peer
int      g_lastCount = -1;
uint64_t g_changeMs = 0;    // the count first moved
uint64_t g_fallMs = 0;      // the count first fell below the start
uint64_t g_restoreMs = 0;   // and first came back to it
uint64_t g_quietMs = 0;     // the act's end state has stood since
uint64_t g_nextHoldsMs = 0;
std::string g_lastHolds;

// The act holds and the host's tallies as one line; printed when it changes.
std::string HoldsLine() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "holds=%zu", AS::Live().Count());
    std::string line = buf;
    for (int i = 0; i < static_cast<int>(AH::Tally::kCount); ++i) {
        const uint32_t n = AH::TallyOf(static_cast<AH::Tally>(i));
        if (!n) continue;
        std::snprintf(buf, sizeof(buf), " %s=%u", AH::TallyName(static_cast<AH::Tally>(i)), n);
        line += buf;
    }
    return line;
}

int WorldReadyPeers(const coop::net::Session& s) {
    int n = 0;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (s.IsSlotWorldReady(slot)) ++n;
    return n;
}

bool AnyPeerWorldReady(const coop::net::Session& s) { return WorldReadyPeers(s) > 0; }

// Race: a client's grant has the record in escrow; the host then takes slot 0 of the same container by its own press.
// The host's slot 0 is the next record while the escrow holds, and R again when it does not.
enum class RaceStep : uint8_t { WaitEscrow, WaitScreen, Settle };
RaceStep g_raceStep = RaceStep::WaitEscrow;
uint64_t g_raceStepMs = 0;
uint64_t g_raceR = 0;
int      g_raceOwnBefore = -1;
uint64_t g_twoFirstMs = 0;   // two: the first take verdict seen
uint64_t g_departMs = 0;     // depart: the first tick the peer was no longer world-ready

void HostRace(uint64_t now, int count, size_t holds, uint32_t bad) {
    switch (g_raceStep) {
    case RaceStep::WaitEscrow: {
        uint64_t hash = 0;
        if (AH::dev::FirstEscrow(g_eid, hash)) {
            g_raceR = hash;
            UE_LOGI("[CI-DRILL] host race escrow R=%016llx (the pick's R=%016llx class=%ls name=%ls unique=%d)",
                    static_cast<unsigned long long>(hash), static_cast<unsigned long long>(g_r0), g_r0Key.cls.c_str(),
                    g_r0Key.name.c_str(), g_r0Unique ? 1 : 0);
            if (!CV::Open(g_actor)) {
                Fail("openPropInv did not dispatch");
                return;
            }
            g_raceStep = RaceStep::WaitScreen;
            g_raceStepMs = now;
            return;
        }
        if (g_readyMs && now - g_readyMs >= kHostReadyMs) Fail("no escrow appeared on the target (count %d of %d)", count, g_records0);
        return;
    }
    case RaceStep::WaitScreen: {
        if (CV::Viewed() != g_actor) {
            if (now - g_raceStepMs >= kOpenFailMs) Fail("the screen does not show the container 10 s after openPropInv");
            return;
        }
        void* const slot = SlotById(-1);
        CV::Slot s{};
        if (!slot || !CV::ReadSlot(slot, s)) {
            Fail("the open screen has no readable first container slot");
            return;
        }
        g_raceOwnBefore = OwnHolds(g_r0Key);
        const bool pressed = CV::PressSlot(slot);
        UE_LOGI("[CI-DRILL] host race slot id=%d pressed=%d own holds R %d", s.id, pressed ? 1 : 0, g_raceOwnBefore);
        if (!pressed) {
            Fail("the slot's pressButton did not dispatch");
            return;
        }
        UE_LOGI("[CI-DRILL] host race pressed");
        g_raceStep = RaceStep::Settle;
        g_raceStepMs = now;
        g_quietMs = 0;
        return;
    }
    case RaceStep::Settle: {
        if (holds != 0) g_quietMs = 0;
        else if (!g_quietMs) g_quietMs = now;
        if (!g_quietMs || now - g_quietMs < kHostQuietMs) {
            if (now - g_raceStepMs >= kHostFailMs)
                Fail("the race did not end in time (count %d of %d, holds %zu, bad_commit %u)", count, g_records0, holds, bad);
            return;
        }
        const int ownAfter = OwnHolds(g_r0Key);
        const int gained = g_raceOwnBefore >= 0 && ownAfter >= 0 ? ownAfter - g_raceOwnBefore : -1;
        UE_LOGI("[CI-DRILL] host race container %d -> %d own holds R %d -> %d bad_commit=%u", g_records0, count,
                g_raceOwnBefore, ownAfter, bad);
        if (gained > 0) {
            Fail("holds R as well");
        } else if (gained < 0) {
            Fail("the own inventory does not read, or lost records (%d -> %d)", g_raceOwnBefore, ownAfter);
        } else if (count != g_records0 - 2) {
            Fail("the container went %d -> %d, expected two fewer", g_records0, count);
        } else {
            UE_LOGI("[CI-DRILL] host race PASS");
            Finish();
        }
        return;
    }
    }
}

// Sale: the grant's reply is held on the host; once the escrow stands, a writer the act does not see empties the
// container (a raw clear stands in for the drone's sale, which the dronesack drill's arm converts to the player's
// take), and the reply goes out. The record is in the escrow, so the clear must not hold it.
enum class SaleStep : uint8_t { WaitEscrow, Settle };
SaleStep g_saleStep = SaleStep::WaitEscrow;
uint64_t g_saleStepMs = 0;

void HostSale(uint64_t now, int count, size_t holds, uint32_t bad) {
    if (g_saleStep == SaleStep::WaitEscrow) {
        uint64_t hash = 0;
        if (!AH::dev::FirstEscrow(g_eid, hash)) {
            if (g_readyMs && now - g_readyMs >= kHostReadyMs)
                Fail("no escrow appeared on the target (count %d of %d)", count, g_records0);
            return;
        }
        std::vector<uint64_t> hashes;
        const bool read = ContainerHashes(g_actor, hashes);
        const bool among = read && std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
        size_t cleared = 0;
        const CRE::EditResult r = CRE::dev::Clear(g_eid, cleared);
        UE_LOGI("[CI-DRILL] host sale cleared %zu records (R among them: %s)", cleared, among ? "yes" : "no");
        AH::dev::ReleaseReplies();
        if (!read) Fail("the target's records do not read");
        else if (r != CRE::EditResult::Done) Fail("the clear did not write (%s)", CRE::EditResultName(r));
        else if (among) Fail("the cleared R is held as well");
        else {
            g_saleStep = SaleStep::Settle;
            g_saleStepMs = now;
            g_quietMs = 0;
        }
        return;
    }
    if (holds != 0) g_quietMs = 0;
    else if (!g_quietMs) g_quietMs = now;
    if (!g_quietMs || now - g_quietMs < kHostQuietMs) {
        if (now - g_saleStepMs >= kHostFailMs)
            Fail("the sale did not end in time (count %d, holds %zu, bad_commit %u)", count, holds, bad);
        return;
    }
    UE_LOGI("[CI-DRILL] host sale container %d -> %d bad_commit=%u", g_records0, count, bad);
    if (count == 0 && bad == 0) {
        UE_LOGI("[CI-DRILL] host sale PASS");
        Finish();
    } else {
        Fail("the container holds %d and bad_commit is %u, expected 0 and 0", count, bad);
    }
}

// Room: the keyed props of R's class within kRoomNearCm of the client's puppet; -1 when the puppet does not read.
// The count before the grant's take is the base the express seam's client-born spawn is judged against.
int g_roomBase = -1;
uint64_t g_nextRoomMs = 0;

int RoomProps() {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
    void* const pup = rp && rp->valid() ? rp->GetActor() : nullptr;
    FVector at{};
    if (!pup || !E::TryGetActorLocation(pup, at)) return -1;
    static std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int n = 0;
    for (const auto& a : pairs) {
        FVector pos{};
        if (!a.actor || !R::IsLiveByIndex(a.actor, a.internalIdx) || !E::TryGetActorLocation(a.actor, pos)) continue;
        const float dx = pos.X - at.X, dy = pos.Y - at.Y, dz = pos.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz > kRoomNearCm * kRoomNearCm) continue;
        if (R::ClassNameOf(a.actor) == g_r0Key.cls) ++n;
    }
    return n;
}

// Room: the base is read when the escrow appears, before the client's replay can spawn the item.
void RoomNoteEscrow() {
    uint64_t hash = 0;
    if (g_roomBase >= 0 || !AH::dev::FirstEscrow(g_eid, hash)) return;
    g_roomBase = RoomProps();
    if (g_roomBase >= 0)
        UE_LOGI("[CI-DRILL] host room escrow R=%016llx class=%ls, keyed props of it near the puppet: %d",
                static_cast<unsigned long long>(hash), g_r0Key.cls.c_str(), g_roomBase);
}

// Room: whether a keyed prop of R's class has appeared near the puppet since the escrow; true for the other arms.
bool RoomPropAppeared(uint64_t now) {
    if (ArmOf() != Arm::Room) return true;
    if (now < g_nextRoomMs) return false;
    g_nextRoomMs = now + kHoldsEveryMs;
    const int n = RoomProps();
    if (g_roomBase < 0 || n <= g_roomBase) return false;
    UE_LOGI("[CI-DRILL] host room keyed props of class %ls near the client's puppet %d -> %d", g_r0Key.cls.c_str(),
            g_roomBase, n);
    return true;
}

// Two: two clients press the same record. The host answers one grant and one Gone, and the container loses one record.
void HostTwo(uint64_t now, int count, size_t holds) {
    uint32_t grants = 0, gones = 0;
    AH::dev::Verdicts(g_eid, grants, gones);
    if (grants + gones && !g_twoFirstMs) g_twoFirstMs = now;
    const bool answered = grants + gones >= 2;
    if (!answered || holds != 0) g_quietMs = 0;
    else if (!g_quietMs) g_quietMs = now;
    if (g_quietMs && now - g_quietMs >= kHostQuietMs) {
        UE_LOGI("[CI-DRILL] host two grants=%u gones=%u container %d -> %d", grants, gones, g_records0, count);
        if (grants == 1 && gones == 1 && count >= 0 && g_records0 - count == 1) {
            UE_LOGI("[CI-DRILL] host two PASS");
            Finish();
        } else {
            Fail("%u grants, %u gones, the container went %d -> %d", grants, gones, g_records0, count);
        }
        return;
    }
    const bool late = g_twoFirstMs ? now - g_twoFirstMs >= kHostFailMs : g_readyMs && now - g_readyMs >= kHostReadyMs;
    if (late) Fail("the two takers did not both end in time (grants %u, gones %u, holds %zu)", grants, gones, holds);
}

// Forge and unasked: once the client says its appends are done, the slices the host took from it are judged. Forge:
// both forged slices refused as not a birth, the truth sent once, the container unchanged. Unasked: no slice arrived.
void HostAppended(uint64_t now, int count) {
    if (!DC::ChatHasLive(kAppendDoneLine)) {
        if (g_readyMs && now - g_readyMs >= kHostReadyMs) Fail("the client never said its appends were done");
        return;
    }
    const CC::DevClientSlices s = CC::DevClientSlicesOf(1);
    UE_LOGI("[CI-DRILL] host %s slices received=%u refused=%u truths=%u, container %d -> %d", ArmName(), s.received,
            s.refused, s.truths, g_records0, count);
    const bool slices = ArmOf() == Arm::Forge ? s.refused >= 2 && s.received == s.refused && s.truths == 1
                                              : s.received == 0;
    if (slices && count == g_records0) {
        UE_LOGI("[CI-DRILL] host %s PASS", ArmName());
        Finish();
    } else {
        Fail("%u slices received, %u refused, %u truths sent, the container went %d -> %d", s.received, s.refused,
             s.truths, g_records0, count);
    }
}

}  // namespace

void HostTick(const coop::net::Session& session) {
    const uint64_t now = coop::net::NowMs();
    if (NoEscrowRed()) AH::dev::NoEscrow(true);
    if (g_phase == Phase::Picking) {
        if (!g_pickStartMs) g_pickStartMs = now;
        if (now < g_nextPickMs) return;
        g_nextPickMs = now + kPickEveryMs;
        int seen = 0, qualified = 0;
        if (TryPick(seen, qualified)) {
            g_lastCount = g_records0;
            g_phase = Phase::Watching;
            if (ArmOf() == Arm::Sale) {
                AH::dev::HoldReplies(true);
                UE_LOGI("[CI-DRILL] host sale: the grant's reply is held; a raw clear stands in for the drone's sale "
                        "(the dronesack drill's arm)");
            }
            return;
        }
        if (now - g_pickStartMs >= kPickFailMs)
            Fail("no world container holds %d records or more after 60 s (%d candidates of %d containers)",
                 kMinRecords, qualified, seen);
        return;
    }
    if (g_phase != Phase::Watching) return;
    // Two: the bound counts from the moment both clients are world-ready.
    if (!g_readyMs && WorldReadyPeers(session) >= (ArmOf() == Arm::Two ? 2 : 1)) {
        g_readyMs = now;
        if (ArmOf() == Arm::Two)
            UE_LOGI("[CI-DRILL] host two: both clients world-ready at ms=%llu", static_cast<unsigned long long>(now));
    }
    if (ArmOf() == Arm::Depart && g_readyMs && !g_departMs && !AnyPeerWorldReady(session)) {
        g_departMs = now;
        UE_LOGI("[CI-DRILL] host depart: the peer is gone at ms=%llu", static_cast<unsigned long long>(now));
    }
    if (ArmOf() == Arm::Room) RoomNoteEscrow();

    const int count = CountOf(g_eid);
    if (count >= 0 && count != g_lastCount) {
        UE_LOGI("[CI-DRILL] host count eid=%u %d -> %d at ms=%llu", g_eid, g_lastCount, count,
                static_cast<unsigned long long>(now));
        g_lastCount = count;
        if (!g_changeMs) g_changeMs = now;
        if (count < g_records0 && !g_fallMs) g_fallMs = now;
        if (count == g_records0 && g_fallMs && !g_restoreMs) g_restoreMs = now;
    }
    const size_t holds = AS::Live().Count();
    const uint32_t bad = AH::TallyOf(AH::Tally::BadCommit);
    if (now >= g_nextHoldsMs) {
        g_nextHoldsMs = now + kHoldsEveryMs;
        std::string line = HoldsLine();
        if (line != g_lastHolds) {
            UE_LOGI("[CI-DRILL] host acts at ms=%llu: %s", static_cast<unsigned long long>(now), line.c_str());
            g_lastHolds = std::move(line);
        }
    }
    if (ArmOf() == Arm::Race) {
        HostRace(now, count, holds, bad);
        return;
    }
    if (ArmOf() == Arm::Two) {
        HostTwo(now, count, holds);
        return;
    }
    if (ArmOf() == Arm::Sale) {
        HostSale(now, count, holds, bad);
        return;
    }
    if (count >= 0 && count < g_records0 - 1) {
        Fail("the count fell by %d (%d -> %d)", g_records0 - count, g_records0, count);
        return;
    }
    if (count > g_records0) {
        Fail("the count rose above the start (%d -> %d)", g_records0, count);
        return;
    }
    if (ArmOf() == Arm::Forge || ArmOf() == Arm::Unasked) {
        HostAppended(now, count);
        return;
    }
    if (ArmOf() == Arm::Ghost) {
        // The ghost leaves no trace on the host: nothing moves and no hold opens, until the bound.
        if (holds || (count >= 0 && count != g_records0)) {
            Fail("the host moved for a record it never had (count %d of %d, holds %zu)", count, g_records0, holds);
            return;
        }
        if (g_readyMs && now - g_readyMs >= kHostReadyMs) {
            UE_LOGI("[CI-DRILL] host ghost count unchanged at %d, no hold, %llu ms after the peer's world", count,
                    static_cast<unsigned long long>(now - g_readyMs));
            UE_LOGI("[CI-DRILL] host ghost PASS");
            Finish();
        }
        return;
    }
    // The act's end state: a take stays taken; an abort, a double abort and a deadline return the record, the last two
    // once the late abort has been counted a bad commit.
    bool settled = false;
    switch (ArmOf()) {
    case Arm::Take:
    case Arm::Settle:
    case Arm::Held:     settled = g_fallMs && count == g_records0 - 1; break;
    case Arm::Abort:    settled = g_restoreMs && count == g_records0; break;
    case Arm::Double:
    case Arm::Deadline: settled = g_restoreMs && count == g_records0 && bad >= 1; break;
    case Arm::Depart:   settled = g_departMs && g_fallMs && g_restoreMs && count == g_records0; break;
    case Arm::Room:     settled = g_fallMs && count == g_records0 - 1; break;
    default: break;
    }
    settled = settled && holds == 0;
    if (!settled) g_quietMs = 0;
    else if (!g_quietMs) g_quietMs = now;
    if (settled && now - g_quietMs >= kHostQuietMs && RoomPropAppeared(now)) {
        const uint64_t restoreMs = g_restoreMs - g_fallMs;
        if (ArmOf() == Arm::Depart)
            UE_LOGI("[CI-DRILL] host depart restore_ms=%lld after the peer left, bad_commit=%u",
                    static_cast<long long>(g_restoreMs) - static_cast<long long>(g_departMs), bad);
        if (ArmOf() == Arm::Deadline) {
            UE_LOGI("[CI-DRILL] host deadline restore_ms=%llu bad_commit=%u",
                    static_cast<unsigned long long>(restoreMs), bad);
            if (restoreMs < kRestoreMinMs || restoreMs > kRestoreMaxMs) {
                Fail("the host restored %llu ms after its grant, expected %llu..%llu",
                     static_cast<unsigned long long>(restoreMs), static_cast<unsigned long long>(kRestoreMinMs),
                     static_cast<unsigned long long>(kRestoreMaxMs));
                return;
            }
        }
        if ((ArmOf() == Arm::Deadline || ArmOf() == Arm::Double) && bad != 1) {
            Fail("bad_commit is %u, expected 1", bad);
            return;
        }
        if (ArmOf() == Arm::Abort) UE_LOGI("[CI-DRILL] host abort bad_commit=%u", bad);
        UE_LOGI("[CI-DRILL] host %s PASS", ArmName());
        Finish();
        return;
    }
    const bool late = g_changeMs ? now - g_changeMs >= kHostFailMs
                                 : g_readyMs && now - g_readyMs >= kHostReadyMs;
    if (late && ArmOf() == Arm::Room && settled)
        Fail("no keyed prop of class %ls appeared within %.0f cm of the client's puppet (base %d, now %d)",
             g_r0Key.cls.c_str(), kRoomNearCm, g_roomBase, RoomProps());
    else if (late)
        Fail("the act did not end in time (count %d of %d, holds %zu, bad_commit %u)", g_lastCount, g_records0, holds,
             bad);
}

void HostReset() {
    g_readyMs = 0;
    g_lastCount = -1;
    g_changeMs = g_fallMs = g_restoreMs = g_quietMs = g_nextHoldsMs = 0;
    g_lastHolds.clear();
    g_raceStep = RaceStep::WaitEscrow;
    g_raceStepMs = g_raceR = g_twoFirstMs = 0;
    g_raceOwnBefore = -1;
    g_departMs = 0;
    g_saleStep = SaleStep::WaitEscrow;
    g_saleStepMs = 0;
    g_roomBase = -1;
    g_nextRoomMs = 0;
}

}  // namespace coop::dev::container_intent_drill::detail

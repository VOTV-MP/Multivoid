// coop/dev/container_intent_drill_append.cpp -- the client's ghost, forge and unasked arms of the container intent drill:
// a record appended to the mirror's contents, then judged; see coop/dev/container_intent_drill.h.

#include "container_intent_drill_detail.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_writer_probe.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CV = ue_wrap::container_view;
namespace CC = coop::props::container_contents_sync;
namespace AR = coop::props::container_act_request;
namespace DC = coop::dev::drill_chat;
namespace RD = coop::dev::container_route_drill::detail;
using ue_wrap::FVector;

// Each wait below follows the event it waits on (the append, the redraw, the refusal): the forge's second append comes a
// second after the first because that gap is the measurement, and the unasked arm's absence is read once the sweep
// has run. The arm's own phase still ends on its printed line.
constexpr uint64_t kOpenFailMs   = 10000;   // the screen shows the new record's slot within this after the redraw
constexpr uint64_t kCommitFailMs = 15000;   // the refusal arrives within this after the press
constexpr uint64_t kSettleMs     = 1000;    // the counts are read this long after the commit
constexpr uint64_t kBackMs       = 2000;    // a mirror is back at the host's count within this
constexpr int      kGhostTries   = 30;      // nearest props tried as the ghost's record

int      g_ghostSlotId = -1;
uint64_t g_ghostAnsweredMs = 0;   // the host's refusal of the ghost arrived
size_t   g_appendFrom = 0;   // the next nearby prop the ghost, the forge and the unasked append try
int      g_forges = 0;       // forge: the records published so far
uint32_t g_verbsBefore = 0;  // unasked: the verb watch's count for the target before the append

// The nearest keyed world props to the player, in order from candidate `from` (moved past the one used), until one's
// record is appended to the mirror: by a raw write (the ghost; the forge publishes it) or by the game's own addObject.
bool AppendGhost(bool publish, bool viaVerb, size_t& from) {
    static std::vector<coop::element::Registry::ActorIdPair> pairs;
    void* const p = coop::players::Registry::Get().Local();
    FVector at{};
    if (!p || !E::TryGetActorLocation(p, at)) return false;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    std::vector<std::pair<float, void*>> cand;
    for (const auto& a : pairs) {
        FVector pos{};
        if (!a.actor || a.actor == g_actor || !R::IsLiveByIndex(a.actor, a.internalIdx) ||
            !E::TryGetActorLocation(a.actor, pos))
            continue;
        const float dx = pos.X - at.X, dy = pos.Y - at.Y, dz = pos.Z - at.Z;
        cand.emplace_back(dx * dx + dy * dy + dz * dz, a.actor);
    }
    std::sort(cand.begin(), cand.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    for (size_t i = from; i < from + kGhostTries && i < cand.size(); ++i) {
        void* const item = cand[i].second;
        if (viaVerb ? RD::PutIn(g_actor, item) : CC::DevRawAppendActor(g_eid, item, publish)) {
            from = i + 1;
            return true;
        }
    }
    return false;
}

void AppendedTick(uint64_t now) {
    if (ArmOf() == Arm::Forge && g_forges == 1) {
        if (now - g_phaseStartMs < kSettleMs) return;
        if (!AppendGhost(true, false, g_appendFrom)) {
            Fail("no second nearby prop's record could be appended to the mirror");
            return;
        }
        g_forges = 2;
        g_phaseStartMs = now;
        return;
    }
    if (now - g_phaseStartMs < kSettleMs) return;   // the sweep that would ship a slice has run
    // Unasked: no slice going out proves nothing unless the verb watch saw the verb; a deaf watch fails the arm.
    const uint32_t seen = coop::dev::container_writer_probe::ClientVerbsOf(g_eid);
    if (ArmOf() == Arm::Unasked && seen <= g_verbsBefore) {
        Fail("the verb watch did not see the addObject on eid=%u (verbs %u -> %u)", g_eid, g_verbsBefore, seen);
        return;
    }
    UE_LOGI("[CI-DRILL] client %s PASS (appended %d)", ArmName(), g_forges);
    DC::Say(kAppendDoneLine);
    Finish();
}

}  // namespace

// The ghost: a record the host never had is appended to the mirror, the screen redrawn, and the slot that names it
// pressed in the next phase.
void StartGhost(uint64_t now) {
    g_containerBefore = CountOf(g_eid);
    if (g_containerBefore < 0) {
        Fail("the mirror's count does not read");
        return;
    }
    if (!AppendGhost(false, false, g_appendFrom)) {
        Fail("no nearby prop's record could be appended to the mirror");
        return;
    }
    const int grown = CountOf(g_eid);
    UE_LOGI("[CI-DRILL] client ghost appended: mirror %d -> %d", g_containerBefore, grown);
    if (grown != g_containerBefore + 1) {
        Fail("the mirror holds %d after the append, expected %d", grown, g_containerBefore + 1);
        return;
    }
    if (!CV::Redraw(g_actor)) {
        Fail("the screen did not redraw");
        return;
    }
    g_ghostSlotId = g_containerBefore;
    g_phaseStartMs = now;
    g_phase = Phase::GhostFind;
}

// The forge publishes a record of its mirror twice, a second apart; the unasked arm calls the game's addObject on it once.
void StartAppend(uint64_t now) {
    g_containerBefore = CountOf(g_eid);
    g_verbsBefore = coop::dev::container_writer_probe::ClientVerbsOf(g_eid);
    if (g_containerBefore < 0 || !AppendGhost(ArmOf() == Arm::Forge, ArmOf() == Arm::Unasked, g_appendFrom)) {
        Fail("no nearby prop's record could be appended to the mirror (count %d)", g_containerBefore);
        return;
    }
    g_forges = 1;
    g_phaseStartMs = now;
    g_phase = Phase::Appended;
}

void AppendTick(uint64_t now) {
    switch (g_phase) {
    case Phase::GhostFind: {
        void* const slot = CV::SlotById(CV::InventoryScreen(), g_ghostSlotId);
        if (slot) {
            g_takesBefore = AR::dev::LastTake().count;
            g_takePressMs = g_waitFromMs = now;
            if (!PressAndCheck(slot)) return;
            g_phase = Phase::GhostWait;
            return;
        }
        if (now - g_phaseStartMs >= kOpenFailMs) Fail("the screen shows no slot for the new record 10 s after the redraw");
        return;
    }
    case Phase::GhostWait: {
        const uint8_t reason = AR::dev::LastRefusal();
        if (reason) {
            g_committed = reason;   // the reason is kept here
            g_ghostAnsweredMs = now;
            g_phase = Phase::GhostBack;
            return;
        }
        if (AR::dev::LastTake().count != g_takesBefore) {
            Fail("the host granted the ghost (a take committed %u)", AR::dev::LastTake().committed);
            return;
        }
        if (now - g_waitFromMs >= kCommitFailMs)
            Fail("no refusal 15 s after the press (pending id=%u)", static_cast<unsigned>(AR::dev::PendingId()));
        return;
    }
    case Phase::Appended: AppendedTick(now); return;
    case Phase::GhostBack: {
        const int count = CountOf(g_eid);
        if (count != g_containerBefore) {
            if (now - g_ghostAnsweredMs >= kBackMs)
                Fail("the mirror holds %d, the host's %d, %llu ms after the refusal", count, g_containerBefore,
                     static_cast<unsigned long long>(now - g_ghostAnsweredMs));
            return;
        }
        UE_LOGI("[CI-DRILL] client ghost refusal=%u mirror back to %d after %llu ms", g_committed, count,
                static_cast<unsigned long long>(now - g_ghostAnsweredMs));
        if (g_committed != static_cast<uint32_t>(coop::net::ContainerActReason::Gone)) {
            Fail("the ghost was refused with reason %u, expected Gone (%u)", g_committed,
                 static_cast<unsigned>(coop::net::ContainerActReason::Gone));
            return;
        }
        UE_LOGI("[CI-DRILL] client ghost PASS");
        Finish();
        return;
    }
    default:
        return;
    }
}

void AppendReset() {
    g_ghostSlotId = -1;
    g_ghostAnsweredMs = 0;
    g_appendFrom = 0;
    g_forges = 0;
    g_verbsBefore = 0;
}

}  // namespace coop::dev::container_intent_drill::detail

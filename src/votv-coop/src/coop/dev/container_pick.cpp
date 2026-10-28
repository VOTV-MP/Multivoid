// coop/dev/container_pick.cpp -- see coop/dev/container_pick.h.

#include "coop/dev/container_pick.h"

#include "coop/dev/director/routes.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::dev::container_pick {

namespace E  = ue_wrap::engine;
namespace CV = ue_wrap::container_view;
namespace CC = coop::props::container_contents_sync;
using ue_wrap::FVector;

int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, kWalkMaxS); }
bool WalkFits(float routeCm) { return static_cast<int>(routeCm / 100.f) + 60 <= kWalkMaxS; }

namespace {

void Collect(uint32_t eid, void* ctx) { static_cast<std::vector<uint32_t>*>(ctx)->push_back(eid); }

int RecordsOf(uint32_t eid) {
    int32_t n = -1;
    float vol = 0.f;
    return CC::ContentsDigest(eid, n, vol) ? n : -1;
}

}  // namespace

// The containers within kPickNearCm of the local player, holding enough records and not the player's own inventory,
// in element-id order; the preferred ones first. The first of them that a nav route from the local player reaches
// within the walk budget is the target. With none near, or none reached, the nearest qualifying one.
bool PickContainer(const Want& want, Pick& out) {
    struct InReach {
        uint32_t eid;
        void* actor;
        bool preferred;
    };
    static std::vector<uint32_t> eids;
    static std::vector<InReach> inReach;
    eids.clear();
    inReach.clear();
    CC::ForEachWorldContainer(&Collect, &eids);
    out = Pick{};
    out.seen = static_cast<int>(eids.size());
    void* const player = coop::players::Registry::Get().Local();
    FVector me{};
    if (!player || !E::TryGetActorLocation(player, me)) return false;
    uint32_t best = 0, nearest = 0;
    float nearestSq = 0.f, bestLen = 0.f;
    for (const uint32_t eid : eids) {
        if (RecordsOf(eid) < want.minRecords) continue;
        void* const actor = coop::element::LivePropActor(eid);
        FVector pos{};
        if (!actor || CV::IsOwnInventory(actor) || !E::TryGetActorLocation(actor, pos)) continue;
        ++out.qualified;
        const float dx = pos.X - me.X, dy = pos.Y - me.Y, dz = pos.Z - me.Z;
        const float sq = dx * dx + dy * dy + dz * dz;
        if (!nearest || sq < nearestSq) {
            nearest = eid;
            nearestSq = sq;
        }
        if (sq > kPickNearCm * kPickNearCm) continue;
        inReach.push_back(InReach{eid, actor, want.prefers && want.prefers(actor)});
    }
    std::sort(inReach.begin(), inReach.end(), [](const InReach& a, const InReach& b) {
        return a.preferred != b.preferred ? a.preferred : a.eid < b.eid;
    });
    for (const InReach& c : inReach) {
        FVector pos{};
        float len = 0.f;
        if (E::TryGetActorLocation(c.actor, pos) && coop::director::RouteInLegs(player, me, pos, kStandCm, &len) &&
            WalkFits(len)) {
            best = c.eid;
            bestLen = len;
            break;
        }
        UE_LOGI("[container-pick] eid=%u skipped: no route reaches it within the walk budget", c.eid);
    }
    if (!best && nearest) {
        best = nearest;
        out.fallback = true;
        UE_LOGI("[container-pick] target far: %.0f cm", std::sqrt(nearestSq));
    }
    if (!best) return false;
    void* const actor = coop::element::LivePropActor(best);
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    out.eid = best;
    out.actor = actor;
    out.pos = at;
    out.records = RecordsOf(best);
    out.routeCm = bestLen;
    return true;
}

}  // namespace coop::dev::container_pick

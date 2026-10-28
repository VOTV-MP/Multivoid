// coop/dev/container_intent_drill_pick.cpp -- the container intent drill's target and its screen: the container both
// peers pick, the records it holds as hashes and as keys, and the open inventory screen's slots. See
// coop/dev/container_intent_drill.h.

#include "container_intent_drill_detail.h"

#include "coop/dev/director/routes.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::dev::container_intent_drill {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CV = ue_wrap::container_view;
namespace SR = ue_wrap::save_record;
namespace CC = coop::props::container_contents_sync;
namespace ci = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
using ue_wrap::FVector;

namespace detail {

static void Collect(uint32_t eid, void* ctx) { static_cast<std::vector<uint32_t>*>(ctx)->push_back(eid); }

static bool ReadContainer(void* container, std::vector<SR::SaveRecord>& recs) {
    void* const inv = ci::InventoryOf(container);
    return inv && cio::ReadRecords(inv, recs, /*neuterNested=*/true);
}

bool ContainerHashes(void* container, std::vector<uint64_t>& out) {
    out.clear();
    std::vector<SR::SaveRecord> recs;
    if (!ReadContainer(container, recs)) return false;
    for (const SR::SaveRecord& r : recs) out.push_back(coop::props::container_slice_wire::RecordHash(r));
    return true;
}

bool ContainerKeys(void* container, std::vector<ItemKey>& out) {
    out.clear();
    std::vector<SR::SaveRecord> recs;
    if (!ReadContainer(container, recs)) return false;
    for (const SR::SaveRecord& r : recs)
        out.push_back(ItemKey{r.className, !r.names.empty() && !r.names[0].empty() ? r.names[0][0] : std::wstring()});
    return true;
}

int OwnHolds(const ItemKey& key) {
    std::vector<ItemKey> keys;
    if (!ContainerKeys(CV::OwnInventory(), keys)) return -1;
    return static_cast<int>(std::count(keys.begin(), keys.end(), key));
}

// Whether the container's record 0 has a class and name no other record of it has; `hash` is its hash as the
// container holds it.
static bool Record0Unique(void* container, uint64_t& hash, ItemKey& key) {
    static std::vector<uint64_t> hashes;
    static std::vector<ItemKey> keys;
    if (!ContainerHashes(container, hashes) || !ContainerKeys(container, keys) || hashes.empty() || keys.empty())
        return false;
    hash = hashes[0];
    key = keys[0];
    return std::count(keys.begin(), keys.end(), key) == 1;
}

// The containers within kPickNearCm of the local player, holding two records or more and not the player's own
// inventory, in element-id order (the peers spawn together, so they agree); the arms that race for record 0 take those
// whose record 0 is unique in the container first. The first of them that a nav route from the local player reaches
// within the walk budget is the target. With none near, or none reached, the nearest qualifying one. `seen` is the
// containers walked, `qualified` those holding enough.
bool TryPick(int& seen, int& qualified) {
    struct InReach {
        uint32_t eid;
        void* actor;
        bool unique;
    };
    static std::vector<uint32_t> eids;
    static std::vector<InReach> inReach;
    eids.clear();
    inReach.clear();
    CC::ForEachWorldContainer(&Collect, &eids);
    seen = static_cast<int>(eids.size());
    qualified = 0;
    void* const player = coop::players::Registry::Get().Local();
    FVector me{};
    if (!player || !E::TryGetActorLocation(player, me)) return false;
    const bool wantUnique = NamesR();
    uint32_t best = 0, nearest = 0;
    float nearestSq = 0.f;
    for (const uint32_t eid : eids) {
        if (CountOf(eid) < kMinRecords) continue;
        void* const actor = coop::element::LivePropActor(eid);
        FVector pos{};
        if (!actor || CV::IsOwnInventory(actor) || !E::TryGetActorLocation(actor, pos)) continue;
        ++qualified;
        const float dx = pos.X - me.X, dy = pos.Y - me.Y, dz = pos.Z - me.Z;
        const float sq = dx * dx + dy * dy + dz * dz;
        if (!nearest || sq < nearestSq) {
            nearest = eid;
            nearestSq = sq;
        }
        if (sq > kPickNearCm * kPickNearCm) continue;
        uint64_t h = 0;
        ItemKey k;
        inReach.push_back(InReach{eid, actor, wantUnique && Record0Unique(actor, h, k)});
    }
    std::sort(inReach.begin(), inReach.end(), [](const InReach& a, const InReach& b) {
        return a.unique != b.unique ? a.unique : a.eid < b.eid;
    });
    for (const InReach& c : inReach) {
        FVector pos{};
        float len = 0.f;
        if (E::TryGetActorLocation(c.actor, pos) && coop::director::RouteInLegs(player, me, pos, kStandCm, &len) &&
            WalkFits(len)) {
            best = c.eid;
            break;
        }
        UE_LOGI("[CI-DRILL] %s eid=%u skipped: no route reaches it within the walk budget", Role(), c.eid);
    }
    if (!best && nearest) {
        best = nearest;
        UE_LOGI("[CI-DRILL] %s target far: %.0f cm", Role(), std::sqrt(nearestSq));
    }
    if (!best) return false;
    void* const actor = coop::element::LivePropActor(best);
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    g_eid = best;
    g_actor = actor;
    g_actorIdx = R::InternalIndexOf(actor);
    g_pos = at;
    g_records0 = CountOf(best);
    UE_LOGI("[CI-DRILL] %s target eid=%u records=%d class=%ls at (%.0f,%.0f,%.0f)", Role(), best, g_records0,
            R::ClassNameOf(actor).c_str(), at.X, at.Y, at.Z);
    if (wantUnique) {
        g_r0Unique = Record0Unique(actor, g_r0, g_r0Key);
        UE_LOGI("[CI-DRILL] %s %s R=%016llx class=%ls name=%ls unique=%d", Role(), ArmName(),
                static_cast<unsigned long long>(g_r0), g_r0Key.cls.c_str(), g_r0Key.name.c_str(), g_r0Unique ? 1 : 0);
    }
    return true;
}

// The inventory screen: the gamemode's propInventory.
static void* Screen() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"propInventory") : -1;
    void* ui = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + off) : nullptr;
    return ui && R::IsLive(ui) ? ui : nullptr;
}

// The screen's container slot that names record `id`, or the first when `id` is negative (the take probe reads
// slots_prop[0] so).
void* SlotById(int32_t id) {
    void* const screen = Screen();
    const int32_t off = screen ? R::FindPropertyOffset(R::ClassOf(screen), L"slots_prop") : -1;
    if (off < 0) return nullptr;
    const SR::Arr arr = SR::ReadArr(screen, off);
    if (!arr.data) return nullptr;
    for (int32_t i = 0; i < arr.num; ++i) {
        void* slot = nullptr;
        std::memcpy(&slot, static_cast<const uint8_t*>(arr.data) + static_cast<size_t>(i) * sizeof(slot), sizeof(slot));
        if (!slot || !R::IsLive(slot)) continue;
        CV::Slot s{};
        if (id < 0) return slot;
        if (CV::ReadSlot(slot, s) && s.id == id) return slot;
    }
    return nullptr;
}

}  // namespace detail

}  // namespace coop::dev::container_intent_drill

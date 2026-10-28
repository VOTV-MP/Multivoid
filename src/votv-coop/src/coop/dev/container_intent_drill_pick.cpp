// coop/dev/container_intent_drill_pick.cpp -- the container intent drill's target and the records a container holds: the
// container both peers pick (coop/dev/container_pick, with the drill's globals set from it), and a container's records
// as hashes and as keys. See coop/dev/container_intent_drill.h.

#include "container_intent_drill_detail.h"

#include "coop/dev/container_pick.h"

#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coop::dev::container_intent_drill {

namespace R  = ue_wrap::reflection;
namespace CV = ue_wrap::container_view;
namespace SR = ue_wrap::save_record;
namespace ci = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;

namespace detail {

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

static bool PrefersUnique(void* actor) {
    uint64_t h = 0;
    ItemKey k;
    return Record0Unique(actor, h, k);
}

// The drill's target: the shared container pick (coop/dev/container_pick), the arms that race for record 0 preferring
// a container where it is unique. `seen` is the containers walked, `qualified` those holding enough.
bool TryPick(int& seen, int& qualified) {
    const bool wantUnique = NamesR();
    coop::dev::container_pick::Want want;
    want.minRecords = kMinRecords;
    if (wantUnique) want.prefers = &PrefersUnique;
    coop::dev::container_pick::Pick pick;
    const bool ok = coop::dev::container_pick::PickContainer(want, pick);
    seen = pick.seen;
    qualified = pick.qualified;
    if (!ok) return false;
    if (pick.fallback) UE_LOGI("[CI-DRILL] %s target far", Role());
    g_eid = pick.eid;
    g_actor = pick.actor;
    g_actorIdx = R::InternalIndexOf(pick.actor);
    g_pos = pick.pos;
    g_records0 = pick.records;
    UE_LOGI("[CI-DRILL] %s target eid=%u records=%d class=%ls at (%.0f,%.0f,%.0f)", Role(), pick.eid, g_records0,
            R::ClassNameOf(pick.actor).c_str(), pick.pos.X, pick.pos.Y, pick.pos.Z);
    if (wantUnique) {
        g_r0Unique = Record0Unique(pick.actor, g_r0, g_r0Key);
        UE_LOGI("[CI-DRILL] %s %s R=%016llx class=%ls name=%ls unique=%d", Role(), ArmName(),
                static_cast<unsigned long long>(g_r0), g_r0Key.cls.c_str(), g_r0Key.name.c_str(), g_r0Unique ? 1 : 0);
    }
    return true;
}

}  // namespace detail

}  // namespace coop::dev::container_intent_drill

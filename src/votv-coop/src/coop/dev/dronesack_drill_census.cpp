// coop/dev/dronesack_drill_census.cpp -- the drone sack drill's census of every world container, read on each peer
// before the drone is called and after its sale: see coop/dev/dronesack_drill.h (arm 2). Each container's line has
// the GObjStack index its inventory holds, its record count and the hash the wire uses for its contents; one more
// line has the length of the save slot's GObjStack. The diff names what changed between the first and the last read.

#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/registry.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/drone.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::dev::dronesack_drill::detail {
namespace {

namespace CC = coop::props::container_contents_sync;
namespace CI = ue_wrap::container_inventory;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

struct Row {
    uint32_t eid = 0;
    void*    actor = nullptr;
    int32_t  index = -1;
    uint32_t records = 0;
    uint64_t hash = 0;
};

constexpr int kMoments = 3;
std::vector<Row> g_rows[kMoments];
bool             g_taken[kMoments] = {};

const char* MomentName(Moment m) {
    return m == Moment::Before ? "before" : m == Moment::After ? "after" : "after+5s";
}

void OnContainer(uint32_t eid, void* ctx) {
    Row r;
    r.eid = eid;
    r.actor = EL::LivePropActor(eid);
    r.index = ContainerIndex(r.actor);
    uint64_t hash = 0;
    uint32_t records = 0;
    if (CC::ContentHashOf(eid, hash, records)) {
        r.hash = hash;
        r.records = records;
    } else {   // a transfer in progress or an unread slice: the count alone
        int32_t count = -1;
        float vol = 0.f;
        if (CC::ContentsDigest(eid, count, vol) && count >= 0) r.records = static_cast<uint32_t>(count);
    }
    static_cast<std::vector<Row>*>(ctx)->push_back(r);
}

// saveSlot->GObjStack's length, or -1.
int StackLength() {
    void* const save = ue_wrap::inventory::ResolveSaveSlot();
    if (!save) return -1;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(save), L"GObjStack");
    return off < 0 ? -1 : SR::ReadArr(save, off).num;
}

const Row* Find(const std::vector<Row>& rows, uint32_t eid) {
    for (const Row& r : rows)
        if (r.eid == eid) return &r;
    return nullptr;
}

}  // namespace

int32_t ContainerIndex(void* container) {
    void* const inv = CI::InventoryOf(container);
    if (!inv) return -1;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(inv), L"Index");
    if (off < 0) return -1;
    int32_t v = -1;
    std::memcpy(&v, static_cast<const uint8_t*>(inv) + off, sizeof(v));
    return v;
}

bool ContainerInSyncScope(void* container) {
    const EL::ElementId eid = container ? EL::Registry::Get().EidForActor(container) : EL::kInvalidId;
    if (eid == EL::kInvalidId) return false;
    struct Ctx { uint32_t eid; bool found; } ctx{static_cast<uint32_t>(eid), false};
    CC::ForEachWorldContainer([](uint32_t e, void* c) {
        Ctx* x = static_cast<Ctx*>(c);
        if (e == x->eid) x->found = true;
    }, &ctx);
    return ctx.found;
}

void CensusIndexHolders(const char* role, Moment m, int32_t index) {
    if (index < 0) {
        UE_LOGI("[SACK-CENSUS] %s %s the crate's index was not read, so no holder is looked for", role, MomentName(m));
        return;
    }
    unsigned held = 0;
    for (const Row& r : g_rows[static_cast<int>(m)]) {
        if (r.index != index) continue;
        ++held;
        UE_LOGI("[SACK-CENSUS] %s %s index %d is now held by eid=%u records=%u", role, MomentName(m), index, r.eid,
                r.records);
    }
    if (!held) UE_LOGI("[SACK-CENSUS] %s %s index %d is held by no container", role, MomentName(m), index);
}

void CensusTake(const char* role, Moment m) {
    const int k = static_cast<int>(m);
    std::vector<Row>& rows = g_rows[k];
    rows.clear();
    CC::ForEachWorldContainer(&OnContainer, &rows);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.eid < b.eid; });
    g_taken[k] = true;
    for (const Row& r : rows)
        UE_LOGI("[SACK-CENSUS] %s %s eid=%u index=%d records=%u hash=%016llx", role, MomentName(m), r.eid, r.index,
                r.records, static_cast<unsigned long long>(r.hash));
    UE_LOGI("[SACK-CENSUS] %s %s stack=%d", role, MomentName(m), StackLength());
}

void CensusDiff(const char* role, void* sack) {
    const int first = static_cast<int>(Moment::Before), last = static_cast<int>(Moment::AfterLater);
    if (!g_taken[first] || !g_taken[last]) {
        UE_LOGI("[SACK-CENSUS] %s DIFF not made: the %s census was not taken", role, g_taken[first] ? "later" : "first");
        return;
    }
    void* const skipA = ContainerOf(sack);
    void* const skipB = ContainerOf(ue_wrap::drone::Find());
    const std::vector<Row>& before = g_rows[first];
    const std::vector<Row>& later = g_rows[last];
    std::vector<uint32_t> eids;
    for (const Row& r : before) eids.push_back(r.eid);
    for (const Row& r : later)
        if (!Find(before, r.eid)) eids.push_back(r.eid);
    unsigned changed = 0;
    for (const uint32_t eid : eids) {
        const Row* a = Find(before, eid);
        const Row* b = Find(later, eid);
        void* const actor = b ? b->actor : a->actor;
        if (actor && (actor == skipA || actor == skipB)) continue;
        const int32_t ia = a ? a->index : -1, ib = b ? b->index : -1;
        const uint32_t ra = a ? a->records : 0, rb = b ? b->records : 0;
        const uint64_t ha = a ? a->hash : 0, hb = b ? b->hash : 0;
        if (ia == ib && ra == rb && ha == hb) continue;
        ++changed;
        UE_LOGI("[SACK-CENSUS] %s CHANGED eid=%u index %d->%d records %u->%u", role, eid, ia, ib, ra, rb);
    }
    UE_LOGI("[SACK-CENSUS] %s DIFF changed=%u of %zu", role, changed, eids.size());
}

void CensusReset() {
    for (int k = 0; k < kMoments; ++k) {
        g_rows[k].clear();
        g_taken[k] = false;
    }
}

}  // namespace coop::dev::dronesack_drill::detail

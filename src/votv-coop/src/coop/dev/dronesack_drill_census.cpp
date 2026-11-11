// coop/dev/dronesack_drill_census.cpp -- the drone sack drill's census of every world container, read on each peer
// before the drone is called and after its sale: see coop/dev/dronesack_drill.h (arm 2). Each container's line has
// its record count and the hash the wire uses for its contents. The diff names what changed between the first and
// the last read.

#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/registry.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coop::dev::dronesack_drill::detail {
namespace {

namespace CC = coop::props::container_contents_sync;
namespace EL = coop::element;

struct Row {
    uint32_t eid = 0;
    void*    actor = nullptr;
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

const Row* Find(const std::vector<Row>& rows, uint32_t eid) {
    for (const Row& r : rows)
        if (r.eid == eid) return &r;
    return nullptr;
}

}  // namespace

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

void CensusTake(const char* role, Moment m) {
    const int k = static_cast<int>(m);
    std::vector<Row>& rows = g_rows[k];
    rows.clear();
    CC::ForEachWorldContainer(&OnContainer, &rows);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.eid < b.eid; });
    g_taken[k] = true;
    for (const Row& r : rows)
        UE_LOGI("[SACK-CENSUS] %s %s eid=%u records=%u hash=%016llx", role, MomentName(m), r.eid, r.records,
                static_cast<unsigned long long>(r.hash));
}

void CensusDiff(const char* role, void* sack) {
    const int first = static_cast<int>(Moment::Before), last = static_cast<int>(Moment::AfterLater);
    if (!g_taken[first] || !g_taken[last]) {
        UE_LOGI("[SACK-CENSUS] %s DIFF not made: the %s census was not taken", role, g_taken[first] ? "later" : "first");
        return;
    }
    void* const skip = ContainerOf(sack);
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
        if (actor && actor == skip) continue;
        const uint32_t ra = a ? a->records : 0, rb = b ? b->records : 0;
        const uint64_t ha = a ? a->hash : 0, hb = b ? b->hash : 0;
        if (ra == rb && ha == hb) continue;
        ++changed;
        UE_LOGI("[SACK-CENSUS] %s CHANGED eid=%u records %u->%u", role, eid, ra, rb);
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

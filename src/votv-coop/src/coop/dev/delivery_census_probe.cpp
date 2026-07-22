// coop/dev/delivery_census_probe.cpp -- COUNT the delivery-path actors over time.
//
// Diagnostic only, ini-gated OFF by default ([dev] delivery_census=1). Never ships behavior.
// (RULE 2 exempts probes/diagnostics/tools -- [[feedback-rule2-exempts-probes-diagnostics-tools]].)
//
// WHY THIS EXISTS (gate O-1, research/findings/inventory-items/
// votv-order-delivery-pipeline-RE-2026-07-22.md): the v124 R11 take came back RED because the
// lane synced the WRONG actor. Measured: the drone's BeginPlay resolves its container with
// getObjectFromKey(n'droneContainer'), but the world-placed container's save key is
// 'drone_InventoryContainer' -- and 'droneContainer' occurs in exactly ONE cooked asset, the
// drone's own. getObjectFromKey is an exact Array_Find over keyObj_key whose only writer is
// lib_C::assignKey, so that lookup MISSES and the drone spawns its own container. Meanwhile
// compileOrder writes through (drone.container)->propInventory.
//
// Three questions must be settled BEFORE any lane redesign, and each is a COUNT or a POINTER
// COMPARISON -- never a hypothesis confirmation. This probe enumerates BY CLASS
// (FindObjectsByClass) rather than looking anything up by key, because looking up by key is
// precisely the operation that is broken:
//
//   Q-A  HOW MANY containers exist?  One (and I misread its key), or two (saved + spawned),
//        or more? -> the row COUNT per class, per sample.
//   Q-B  WHICH one receives the delivery?  -> the per-row contents count
//        (GObjStack[propInventory.Index].obj.Num) watched across the delivery. The SACK is not
//        assumed to be a third holder: measured, Aprop_dronesack_C::container @0x0380 is a
//        POINTER to an Aprop_inventoryContainer_drone_C, i.e. the sack REFERENCES a container
//        rather than holding contents. So "is the sack a third receiver?" reduces to a pointer
//        comparison: is sack.container == drone.container, == the saved one, or neither?
//   Q-C  Is the receiving container STABLE across deliveries, or does it CHURN per delivery?
//        -> run two consecutive deliveries and compare actor ptr / eid / propInventory.Index
//        between them. This decides whether stable element-key addressing is applicable at all
//        or whether the lane belongs in birth-channel territory (the R14/15/16 shape).
//
// Output discipline: a full census line set is emitted on the FIRST sample and thereafter ONLY
// when the census CHANGES (a hash over the rows). So the log shows EDGES -- a spawn, a destroy,
// a contents delta, an Index repoint -- instead of a 0.5 Hz wall of identical rows.

#include "coop/dev/delivery_census_probe.h"

#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/config/config.h"
#include "ue_wrap/actors/inventory.h"    // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"         // GetInteractableKeyString
#include "ue_wrap/engine/engine.h"       // GetActorLocation
#include "ue_wrap/actors/save_record.h"  // Arr / ReadArr / kMxStride
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::delivery_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

constexpr uint64_t kSampleMs = 2000;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}  // dev probe; a GUObjectArray walk is not free

uint64_t g_nextSample = 0;
uint64_t g_lastHash = 0;
bool     g_first = true;
int      g_sampleNo = 0;

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

int32_t Off(void* cls, const wchar_t* name) {
    return cls ? R::FindPropertyOffset(cls, name) : -1;
}

// One census row -- everything needed to answer Q-A/Q-B/Q-C without inferring anything.
struct Row {
    void*        actor = nullptr;
    std::wstring cls;
    std::wstring key;          // the save Key, or empty
    uint32_t     eid = 0;      // 0 = not enrolled as an Element
    void*        inv = nullptr;
    int32_t      index = -1;   // propInventory.Index -- the GObjStack slot
    float        currVol = 0.f;
    bool         player = false;
    int32_t      contents = -1;  // GObjStack[index].obj.Num, or -1 if unresolvable
    float        x = 0, y = 0, z = 0;
    bool         isDroneContainer = false;  // == drone.container (what compileOrder writes through)
    bool         isSackContainer = false;   // == some sack.container
};

// The contents count for a propInventory, read straight out of the global GObjStack.
int32_t ContentsOf(void* inv, int32_t index) {
    if (!inv || index < 0) return -1;
    void* save = ue_wrap::inventory::ResolveSaveSlot();
    if (!save) return -1;
    const int32_t offStack = Off(R::ClassOf(save), L"GObjStack");
    if (offStack < 0) return -1;
    const SR::Arr stack = SR::ReadArr(save, offStack);
    if (index >= stack.num) return -1;
    const uint8_t* slot = stack.data + static_cast<size_t>(index) * SR::kMxStride;
    return SR::ReadArr(slot, 0).num;  // struct_mObject.obj @ +0
}

void FillInventory(Row& r) {
    const int32_t offInv = Off(R::ClassOf(r.actor), L"propInventory");
    if (offInv < 0) return;
    r.inv = ReadAt<void*>(r.actor, offInv);
    if (!r.inv || !R::IsLive(r.inv)) { r.inv = nullptr; return; }
    void* icls = R::ClassOf(r.inv);
    const int32_t oi = Off(icls, L"Index"), ov = Off(icls, L"currVol"), op = Off(icls, L"Player");
    if (oi >= 0) r.index   = ReadAt<int32_t>(r.inv, oi);
    if (ov >= 0) r.currVol = ReadAt<float>(r.inv, ov);
    if (op >= 0) r.player  = ReadAt<uint8_t>(r.inv, op) != 0;
    r.contents = ContentsOf(r.inv, r.index);
}

void FillCommon(Row& r) {
    r.cls = R::ClassNameOf(r.actor);
    r.key = ue_wrap::prop::GetInteractableKeyString(r.actor);
    const auto id = coop::element::Registry::Get().EidForActor(r.actor);
    r.eid = (id == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(id);
    const auto loc = ue_wrap::engine::GetActorLocation(r.actor);
    r.x = loc.X; r.y = loc.Y; r.z = loc.Z;
}

uint64_t HashRows(const std::vector<Row>& rows) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (const auto& r : rows) {
        mix(reinterpret_cast<uintptr_t>(r.actor));
        mix(static_cast<uint64_t>(r.eid));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(r.index)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(r.contents)));
        mix(r.isDroneContainer ? 1u : 0u);
        mix(r.isSackContainer ? 2u : 0u);
        for (wchar_t c : r.key) mix(static_cast<uint64_t>(c));
    }
    mix(static_cast<uint64_t>(rows.size()));
    return h;
}

}  // namespace

void Tick(bool isHost) {
    static const bool s_enabled = coop::config::IsIniKeyTrue("delivery_census");
    if (!s_enabled) return;
    const uint64_t nowMs = NowMs();
    if (nowMs < g_nextSample) return;
    g_nextSample = nowMs + kSampleMs;

    // The two pointers that DEFINE the answer. Read them first so the rows can be marked.
    void* droneContainer = nullptr;
    std::vector<void*> sackContainers;

    const std::vector<void*> drones = R::FindObjectsByClass(L"drone_C");
    for (void* d : drones) {
        if (!d || !R::IsLive(d)) continue;
        const int32_t off = Off(R::ClassOf(d), L"container");   // Adrone_C::container @0x04F8
        if (off >= 0) droneContainer = ReadAt<void*>(d, off);
    }

    // Q-B: the sack REFERENCES a container (Aprop_dronesack_C::container @0x0380), it does not
    // hold contents. Enumerate sacks so "is the sack a third receiver" becomes a ptr compare.
    const std::vector<void*> sacks = R::FindObjectsByClass(L"prop_dronesack_C");
    for (void* s : sacks) {
        if (!s || !R::IsLive(s)) continue;
        void* scls = R::ClassOf(s);
        const int32_t offC = Off(scls, L"container");
        const int32_t offT = Off(scls, L"takenByDrone");
        void* sc = (offC >= 0) ? ReadAt<void*>(s, offC) : nullptr;
        if (sc) sackContainers.push_back(sc);
        UE_LOGI("[delivery_census] SACK actor=%p taken=%d container=%p", s,
                (offT >= 0) ? (ReadAt<uint8_t>(s, offT) ? 1 : 0) : -1, sc);
    }

    // Q-A: COUNT the containers by CLASS. Not a key lookup -- the key lookup is the broken thing.
    std::vector<Row> rows;
    for (void* a : R::FindObjectsByClass(L"prop_inventoryContainer_drone_C")) {
        if (!a || !R::IsLive(a)) continue;
        Row r;
        r.actor = a;
        FillCommon(r);
        FillInventory(r);
        r.isDroneContainer = (a == droneContainer);
        for (void* sc : sackContainers) if (sc == a) r.isSackContainer = true;
        rows.push_back(std::move(r));
    }

    const uint64_t h = HashRows(rows);
    if (!g_first && h == g_lastHash) return;   // EDGES only -- no identical-row wall
    g_first = false;
    g_lastHash = h;
    ++g_sampleNo;

    UE_LOGI("[delivery_census] #%d %s -- containers=%zu sacks=%zu drone.container=%p",
            g_sampleNo, isHost ? "HOST" : "CLIENT", rows.size(), sacks.size(), droneContainer);
    for (const auto& r : rows) {
        UE_LOGI("[delivery_census]   actor=%p cls='%ls' key='%ls' eid=%u idx=%d contents=%d "
                "vol=%.1f player=%d loc=(%.0f,%.0f,%.0f)%s%s",
                r.actor, r.cls.c_str(), r.key.empty() ? L"<none>" : r.key.c_str(), r.eid,
                r.index, r.contents, r.currVol, r.player ? 1 : 0, r.x, r.y, r.z,
                r.isDroneContainer ? "  <== drone.container (compileOrder writes HERE)" : "",
                r.isSackContainer ? "  <== sack.container" : "");
    }
    if (rows.empty())
        UE_LOGW("[delivery_census]   (no prop_inventoryContainer_drone_C actors alive)");
}

}  // namespace coop::dev::delivery_census

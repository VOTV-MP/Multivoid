// coop/dev/delivery_census_probe.cpp -- COUNT where a delivery's goods are, over time. Diagnostic only, ini-gated
// off by default ([dev] delivery_census=1); it never ships behaviour.
//
// A delivery lands in the drone's own inventory component and leaves it when the player drops the sack: the
// sack is a world container, prop_dronesack_C, that takes the drone's entries with it. The census enumerates
// those holders by class, never by key, and logs the drone's cargo count and each sack's entry count, with the
// sack's element id, its place and whether the drone took it back. A full census is logged on the first sample
// and thereafter only when a hash over the rows changes, so the log shows edges -- a sack appearing, goods
// moving from the drone to a sack and back, a sale emptying the drone -- rather than a wall of identical rows.
// Both peers run it; the host's drone is the truth and a client's is the copy drone_cargo_sync applied.

#include "coop/dev/delivery_census_probe.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"         // GetInteractableKeyString
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine.h"       // TryGetActorLocation

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::delivery_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace D  = ue_wrap::drone;
namespace CI = ue_wrap::container_inventory;

constexpr uint64_t kSampleMs = 2000;
constexpr size_t   kMaxEntries = 4096;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t g_nextSample = 0;
uint64_t g_lastHash = 0;
bool     g_first = true;
int      g_sampleNo = 0;

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// One sack: everything needed to say where its goods are without inferring anything.
struct Row {
    void*        actor = nullptr;
    std::wstring key;          // the save key, or empty
    uint32_t     eid = 0;      // 0 = not enrolled as an element
    int32_t      entries = -1; // non-blank inventory entries, or -1 when they do not read
    int          taken = -1;   // takenByDrone, or -1 when the field does not resolve
    float        x = 0, y = 0, z = 0;
    bool         locRead = false;
};

// The non-blank entries of an inventory component, or -1 when they do not read.
int32_t EntriesOf(void* inv) {
    std::vector<D::CargoItem> items;
    if (!inv || !CI::ReadContents(inv, items, kMaxEntries)) return -1;
    int32_t n = 0;
    for (const D::CargoItem& it : items)
        if (!ue_wrap::inventory_item::IsEmpty(it)) ++n;
    return n;
}

uint64_t HashRows(const std::vector<Row>& rows, int32_t cargo, int hasSack) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (const auto& r : rows) {
        mix(reinterpret_cast<uintptr_t>(r.actor));
        mix(static_cast<uint64_t>(r.eid));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(r.entries)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(r.taken)));
        for (wchar_t c : r.key) mix(static_cast<uint64_t>(c));
    }
    mix(static_cast<uint64_t>(static_cast<uint32_t>(cargo)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(hasSack)));
    mix(static_cast<uint64_t>(rows.size()));
    return h;
}

}  // namespace

void Tick(bool isHost) {
    static const bool s_enabled = coop::config::ResolveFlag(::coop::config_registry::rows::delivery_census);
    if (!s_enabled) return;
    const uint64_t nowMs = NowMs();
    if (nowMs < g_nextSample) return;
    g_nextSample = nowMs + kSampleMs;

    void* const drone = D::Find();
    const int32_t cargo = drone ? EntriesOf(D::InventoryOf(drone)) : -1;
    const int hasSack = drone ? ((D::ReadFxBits(drone) & D::kFxHasSack) ? 1 : 0) : -1;
    const int arrived = drone ? ((D::ReadFxBits(drone) & D::kFxArrived) ? 1 : 0) : -1;

    std::vector<Row> rows;
    for (void* a : R::FindObjectsByClass(L"prop_dronesack_C")) {
        if (!a || !R::IsLive(a)) continue;
        Row r;
        r.actor = a;
        r.key = ue_wrap::prop::GetInteractableKeyString(a);
        const auto id = coop::element::Registry::Get().EidForActor(a);
        r.eid = (id == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(id);
        r.entries = EntriesOf(CI::InventoryOf(a));
        const int32_t offT = R::FindPropertyOffset(R::ClassOf(a), L"takenByDrone");
        if (offT >= 0) r.taken = ReadAt<uint8_t>(a, offT) ? 1 : 0;
        ue_wrap::FVector loc{};
        r.locRead = ue_wrap::engine::TryGetActorLocation(a, loc);
        r.x = loc.X; r.y = loc.Y; r.z = loc.Z;
        rows.push_back(std::move(r));
    }

    const uint64_t h = HashRows(rows, cargo, hasSack);
    if (!g_first && h == g_lastHash) return;   // EDGES only -- no identical-row wall
    g_first = false;
    g_lastHash = h;
    ++g_sampleNo;

    UE_LOGI("[delivery_census] #%d %s -- drone=%p cargo=%d hasSack=%d arrived=%d sacks=%zu", g_sampleNo,
            isHost ? "HOST" : "CLIENT", drone, cargo, hasSack, arrived, rows.size());
    for (const auto& r : rows) {
        UE_LOGI("[delivery_census]   sack=%p key='%ls' eid=%u entries=%d taken=%d loc=(%.0f,%.0f,%.0f)%s", r.actor,
                r.key.empty() ? L"<none>" : r.key.c_str(), r.eid, r.entries, r.taken, r.x, r.y, r.z,
                r.locRead ? "" : " (unread)");
    }
}

}  // namespace coop::dev::delivery_census

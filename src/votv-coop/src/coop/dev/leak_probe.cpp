// coop/dev/leak_probe.cpp -- see header. Dev-only UObject-census leak attribution.

#include "coop/dev/leak_probe.h"

#include "coop/ini_config.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::leak_probe {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

// Armed-state latch (read the ini once -- the key never changes mid-run).
int  g_armed = -1;  // -1 = unresolved, 0 = off, 1 = on
bool Armed() {
    if (g_armed < 0) g_armed = coop::ini_config::IsIniKeyTrue("leak_probe") ? 1 : 0;
    return g_armed == 1;
}

constexpr auto kInterval = std::chrono::seconds(4);
Clock::time_point g_next{};
bool g_haveBaseline = false;

// Previous snapshot: live UObject count per UClass*, plus the grand total. The class
// pointer is a stable identity for the run (UClasses are not GC'd). Bounded by the
// number of distinct loaded classes (~a few thousand) -> small, move-replaced each
// snapshot (the probe must not itself look like a leak).
std::unordered_map<void*, int32_t> g_prevByClass;
int32_t g_prevTotal = 0;
uint64_t g_snap = 0;

}  // namespace

void Tick() {
    if (!Armed()) return;
    const auto now = Clock::now();
    if (g_haveBaseline && now < g_next) return;
    g_next = now + kInterval;

    // One GUObjectArray pass: count live objects, histogram by class pointer.
    std::unordered_map<void*, int32_t> cur;
    cur.reserve(g_prevByClass.empty() ? 4096 : g_prevByClass.size() + 256);
    int32_t total = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        void* cls = R::ClassOf(o);
        if (!cls) continue;
        ++total;
        ++cur[cls];
    }

    if (!g_haveBaseline) {
        g_prevByClass = std::move(cur);
        g_prevTotal = total;
        g_haveBaseline = true;
        UE_LOGW("[leak_probe] baseline: %d live UObject(s) across %zu class(es) -- "
                "growth report every %llds (watch the TOTAL delta vs RSS: lockstep => "
                "UObject leak named below; flat total + climbing RSS => raw-heap leak, "
                "escalate to a GMalloc stack hook)",
                total, g_prevByClass.size(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kInterval).count()));
        return;
    }

    // Rank classes by growth since the previous snapshot.
    struct Grow { void* cls; int32_t delta; int32_t count; };
    std::vector<Grow> growers;
    growers.reserve(64);
    for (const auto& [cls, c] : cur) {
        auto it = g_prevByClass.find(cls);
        const int32_t prev = (it != g_prevByClass.end()) ? it->second : 0;
        const int32_t delta = c - prev;
        if (delta > 0) growers.push_back({cls, delta, c});
    }
    std::sort(growers.begin(), growers.end(),
              [](const Grow& a, const Grow& b) { return a.delta > b.delta; });

    const int32_t totalDelta = total - g_prevTotal;
    ++g_snap;
    UE_LOGW("[leak_probe] #%llu: %d live UObject(s) (delta %+d since last %llds) -- "
            "%zu class(es) grew. %s",
            static_cast<unsigned long long>(g_snap), total, totalDelta,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kInterval).count()),
            growers.size(),
            totalDelta <= 1 ? "TOTAL ~flat -> if RSS still climbs the leak is RAW HEAP (not UObjects)"
                            : "TOTAL climbing -> the grower(s) below are the UObject leak");

    // Name only the top growers (ClassNameOf is the only per-line reflection cost).
    const size_t kTop = 12;
    for (size_t i = 0; i < growers.size() && i < kTop; ++i) {
        const std::wstring cn = R::ClassNameOf(growers[i].cls);
        UE_LOGW("[leak_probe]   +%-5d -> %-6d  %ls", growers[i].delta, growers[i].count,
                cn.empty() ? L"<unknown>" : cn.c_str());
    }

    g_prevByClass = std::move(cur);
    g_prevTotal = total;
}

}  // namespace coop::dev::leak_probe

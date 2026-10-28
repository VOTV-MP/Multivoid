// coop/dev/grid_drill_walk.cpp -- see grid_drill_walk.h.

#include "grid_drill_walk.h"

#include "grid_drill_checks.h"  // co-located private header (src tree, not include/)

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/world/power_grid.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"  // FVector
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::grid_drill {
namespace {

namespace GEN = ue_wrap::generator;
namespace DR  = coop::director;
using ue_wrap::FVector;

// The standpoints are a ring about the Activate button; a walk ends this near its standpoint, flat. Both stay well
// inside the host's reach for a generator op.
constexpr float kRingCm = 150.f;
constexpr int   kStandpoints = 8;
constexpr float kStandReachCm = 60.f;
constexpr float kBelowButtonCm = 100.f;  // the second try's ring: the ground the button stands over
constexpr int   kTraceLines = 12;        // the last lines of the route's trace an ABANDONED line is followed by
// A client joins about 920 m from the generator. The director's own bound on the walk: a failure bound.
constexpr int   kWalkDeadlineS = 600;

std::shared_ptr<DR::BackgroundWalk> g_walk;
bool        g_asked = false;
float       g_routeUU = -1.f;
FVector     g_button{};
std::string g_trace;

// The last `kTraceLines` lines of the route's trace.
void LogTraceTail() {
    std::vector<size_t> starts;
    for (size_t pos = 0; pos < g_trace.size();) {
        starts.push_back(pos);
        const size_t nl = g_trace.find('\n', pos);
        pos = nl == std::string::npos ? g_trace.size() : nl + 1;
    }
    for (size_t i = starts.size() > kTraceLines ? starts.size() - kTraceLines : 0; i < starts.size(); ++i) {
        const size_t end = g_trace.find('\n', starts[i]);
        UE_LOGI("[GRID-DRILL] client walk trace: %s",
                g_trace.substr(starts[i], end == std::string::npos ? std::string::npos : end - starts[i]).c_str());
    }
}

WalkState Abandon(const char* why) {
    UE_LOGW("[GRID-DRILL] ABANDONED on the client: %s", why);
    LogTraceTail();
    return WalkState::Ended;
}

std::vector<FVector> Standpoints(void* player, const float* ringZ) {
    g_trace.clear();  // the ABANDONED tail is the search that failed last
    return DR::ReachableStandpoints(player, g_button, kRingCm, kStandpoints, kStandReachCm, ringZ, &g_trace);
}

}  // namespace

WalkState WalkBesideGenerator(void* player) {
    constexpr float allowed = coop::power_grid::kGeneratorReachUU;
    if (!g_asked) {
        void* button = GEN::ActivateButton(DrillGen());
        FVector me{};
        if (!button || !ue_wrap::engine::TryGetActorLocation(player, me))
            return Abandon("the drill's generator's Activate button or this client's place does not read");
        g_asked = true;
        g_button = ue_wrap::engine::GetComponentLocation(button);
        // The ring at the height this client stands at, then at the ground below the button.
        std::vector<FVector> ring = Standpoints(player, nullptr);
        UE_LOGI("[GRID-DRILL] client standpoints at z=%.0f: %zu", me.Z, ring.size());
        if (ring.empty()) {
            const float z = g_button.Z - kBelowButtonCm;
            ring = Standpoints(player, &z);
            UE_LOGI("[GRID-DRILL] client standpoints at z=%.0f: %zu", z, ring.size());
        }
        if (ring.empty()) {
            char why[160];
            std::snprintf(why, sizeof(why), "no route reaches a standpoint %.0f uu about the Activate button (%.0f uu away)",
                          kRingCm, Dist(me, g_button));
            return Abandon(why);
        }
        if (!DR::RouteInLegs(player, me, ring[0], kStandReachCm, &g_routeUU)) g_routeUU = -1.f;
        UE_LOGI("[GRID-DRILL] client walks to a standpoint at (%.0f, %.0f, %.0f), route %.0f uu", ring[0].X, ring[0].Y,
                ring[0].Z, g_routeUU);
        g_walk = DR::StartBackgroundWalk(ring[0], kStandReachCm, kWalkDeadlineS);
        return WalkState::Walking;
    }
    if (!g_walk) return Abandon("the walk to the standpoint is gone");
    const int st = g_walk->state.load();
    if (st == 0) return WalkState::Walking;
    g_walk.reset();
    FVector me{};
    if (!ue_wrap::engine::TryGetActorLocation(player, me)) return Abandon("this client's place does not read");
    const float dist = Dist(me, g_button);
    if (st != 1 || dist > allowed) {
        char why[192];
        std::snprintf(why, sizeof(why), "the walk %s %.0f uu from the Activate button, allowed %.0f (route %.0f)",
                      st == 1 ? "ended" : "gave up", dist, allowed, g_routeUU);
        return Abandon(why);
    }
    UE_LOGI("[GRID-DRILL] client walked beside generator %d: route=%.0f dist=%.0f allowed=%.0f",
            GEN::IndexOf(DrillGen()), g_routeUU, dist, allowed);
    return WalkState::Arrived;
}

void WalkReset() {
    g_walk.reset();
    g_asked = false;
    g_routeUU = -1.f;
    g_trace.clear();
}

}  // namespace coop::dev::grid_drill

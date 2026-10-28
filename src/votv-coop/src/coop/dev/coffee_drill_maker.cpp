// coop/dev/coffee_drill_maker.cpp -- see coop/dev/coffee_drill_detail.h: where this peer's copy of the maker rests,
// and what touches it.

#include "coffee_drill_detail.h"  // co-located private header (src tree, not include/)
#include "cord_drill_detail.h"    // co-located private header: Narrow

#include "coop/interactables/coffee_state.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace coop::dev::coffee_drill::detail {
namespace {

namespace E = ue_wrap::engine;

constexpr uint64_t kEveryMs = 5000;
constexpr size_t   kTopClasses = 3;

uint64_t g_nextMs = 0;
uint64_t g_prevHits = 0;
std::vector<coop::coffee_state::ClassCount> g_prev;   // the tally at the last line

// The body's speed: the physics body's when the root simulates, else the actor's own.
float SpeedOf(void* maker) {
    ue_wrap::FVector lin{}, ang{};
    if (!E::GetActorRootPhysicsVelocity(maker, lin, ang)) lin = E::GetActorVelocity(maker);
    return std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z);
}

uint64_t PrevCount(const std::wstring& cls) {
    for (const auto& p : g_prev)
        if (p.cls == cls) return p.n;
    return 0;
}

}  // namespace

void LogMakerAt(const char* when) {
    ue_wrap::FVector at{};
    void* maker = Maker();
    if (!maker || !E::TryGetActorLocation(maker, at)) {
        UE_LOGI("[COFFEE] maker %s: place unread", when);
        return;
    }
    UE_LOGI("[COFFEE] maker %s at (%.0f,%.0f,%.0f)", when, at.X, at.Y, at.Z);
}

void MakerProbeTick() {
    void* maker = Maker();
    if (!maker) return;
    const uint64_t now = NowMs();
    if (g_nextMs == 0) g_nextMs = now + kEveryMs;   // the first line covers everything since the session began
    if (now < g_nextMs) return;
    g_nextMs = now + kEveryMs;
    ue_wrap::FVector at{};
    const bool read = E::TryGetActorLocation(maker, at);
    const auto view = coop::coffee_state::ReadForDrill();
    std::vector<coop::coffee_state::ClassCount> delta;
    for (const auto& c : view.byClass)
        if (c.n > PrevCount(c.cls)) delta.push_back({c.cls, c.n - PrevCount(c.cls)});
    std::sort(delta.begin(), delta.end(), [](const auto& a, const auto& b) { return a.n > b.n; });
    std::string top;
    for (size_t i = 0; i < delta.size() && i < kTopClasses; ++i) {
        if (i) top += ",";
        top += coop::dev::cord_drill::detail::Narrow(delta[i].cls) + ":" + std::to_string(delta[i].n);
    }
    UE_LOGI("[COFFEE] maker at (%.0f,%.0f,%.0f)%s vel=%.1f hits=%llu top=%s", at.X, at.Y, at.Z,
            read ? "" : " (unread)", SpeedOf(maker), static_cast<unsigned long long>(view.hits - g_prevHits),
            top.empty() ? "-" : top.c_str());
    g_prevHits = view.hits;
    g_prev = view.byClass;
}

void MakerProbeReset() {
    g_nextMs = 0;
    g_prevHits = 0;
    g_prev.clear();
}

}  // namespace coop::dev::coffee_drill::detail

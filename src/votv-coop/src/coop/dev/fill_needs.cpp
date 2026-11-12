// coop/dev/fill_needs.cpp -- see coop/dev/fill_needs.h.

#include "coop/dev/fill_needs.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::fill_needs {
namespace {

namespace V = ue_wrap::vitals;

constexpr uint64_t kRefillMs = 20000;
constexpr uint64_t kRetryMs = 2000;
constexpr float kFull = 100.f;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::fill_needs);
    return s;
}

}  // namespace

// A clock and not an event: the game's ticker drains the two floats in place and nothing announces the drain, so the
// refill is periodic. Until the world's local player is there the retry is the short period.
void Tick() {
    if (!Enabled()) return;
    static uint64_t nextMs = 0;
    const uint64_t now = ::GetTickCount64();
    if (now < nextMs) return;
    float food = -1.f, sleep = -1.f;
    const bool ok = coop::players::Registry::Get().Local() != nullptr && V::Read(V::Field::Food, &food) &&
                    V::Read(V::Field::Sleep, &sleep) && V::Write(V::Field::Food, kFull) &&
                    V::Write(V::Field::Sleep, kFull);
    nextMs = now + (ok ? kRefillMs : kRetryMs);
    if (ok) UE_LOGI("fill-needs: food %.1f and sleep %.1f refilled to 100", food, sleep);
}

}  // namespace coop::dev::fill_needs

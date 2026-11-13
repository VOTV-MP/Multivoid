// coop/dev/vitals_keepalive.cpp -- see header.

#include "coop/dev/vitals_keepalive.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/restore_vitals.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/vitals.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>

namespace coop::dev::vitals_keepalive {

namespace V = ue_wrap::vitals;

namespace {

// Ini period in ms, latched on first Tick (config reads hit the file).
// 0 = disabled. Floor 30 s -- a mistyped 1-second period must not turn the
// refill broadcast into wire spam.
long long PeriodMs() {
    static const long long s = [] {
        // Registry-typed read (arc 2); the 30 s floor for a non-zero period is
        // SITE intent (a mistyped 1 s must not become wire spam), not a range.
        const long sec = coop::config::ResolveInt(coop::config_registry::rows::vitals_keepalive_sec);
        if (sec <= 0) return 0LL;
        return static_cast<long long>(sec < 30 ? 30 : sec) * 1000LL;
    }();
    return s;
}

long long g_lastFireMs = 0;

bool FillNeedsOn() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::fill_needs);
    return s;
}

constexpr uint64_t kFillMs = 20000;       // the refill period
constexpr uint64_t kFillRetryMs = 2000;   // until the local player's world is up
constexpr float kFull = 100.f;

// Each peer's OWN food and sleep to 100, local and off the wire. A clock and not an event: the game's ticker drains the
// two floats in place and nothing announces the drain, so the refill is periodic. No dev_gate or local_grants check: the
// row is a dev-only config seam in the dev-only source, and it touches only this machine's own player.
void FillOwnNeeds() {
    static uint64_t nextMs = 0;
    const uint64_t now = ::GetTickCount64();
    if (now < nextMs) return;
    float food = -1.f, sleep = -1.f;
    const bool ok = coop::players::Registry::Get().Local() != nullptr && V::Read(V::Field::Food, &food) &&
                    V::Read(V::Field::Sleep, &sleep) && V::Write(V::Field::Food, kFull) &&
                    V::Write(V::Field::Sleep, kFull);
    nextMs = now + (ok ? kFillMs : kFillRetryMs);
    if (ok) UE_LOGI("[VITALS-KEEPALIVE] fill_needs: food %.1f and sleep %.1f refilled to 100", food, sleep);
}

// The host's broadcast refill, every `period`.
void RestoreAll(long long period) {
    const long long now = static_cast<long long>(::GetTickCount64());
    if (g_lastFireMs != 0 && now - g_lastFireMs < period) return;
    g_lastFireMs = now;

    // Host/solo only -- strict client lockout (dev_gate). A client with the
    // flag set gets one skip per period at most; no log spam by design (the
    // host's broadcast refills it anyway).
    if (!coop::dev_gate::Allowed()) return;

    // MEASURE before refilling: the drain curve between firings is the
    // idle-death adjudication data. A failed read = save not registered yet
    // (booting / menu) -- skip this period, retry next.
    float food = -1.f, sleep = -1.f, health = -1.f;
    if (!V::Read(V::Field::Food, &food)) return;
    V::Read(V::Field::Sleep, &sleep);
    V::Read(V::Field::Health, &health);
    UE_LOGI("[VITALS-KEEPALIVE] pre-refill food=%.1f sleep=%.1f health=%.1f "
            "(period=%llds) -> Restore", food, sleep, health, period / 1000);

    // The proven dev path: local apply (GT-posted) + RestoreVitals reliable
    // broadcast so every connected peer refills too.
    restore_vitals::Restore();
}

}  // namespace

void Tick() {
    if (FillNeedsOn()) FillOwnNeeds();
    const long long period = PeriodMs();
    if (period != 0) RestoreAll(period);
}

}  // namespace coop::dev::vitals_keepalive

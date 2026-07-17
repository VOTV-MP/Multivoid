// coop/dev/vitals_keepalive.cpp -- see header.

#include "coop/dev/vitals_keepalive.h"

#include "coop/config/config.h"
#include "coop/dev/dev_gate.h"
#include "coop/dev/restore_vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/vitals.h"

#include <windows.h>

#include <cstdlib>

namespace coop::dev::vitals_keepalive {

namespace V = ue_wrap::vitals;

namespace {

// Ini period in ms, latched on first Tick (config reads hit the file).
// 0 = disabled. Floor 30 s -- a mistyped 1-second period must not turn the
// refill broadcast into wire spam.
long long PeriodMs() {
    static const long long s = [] {
        const int sec = std::atoi(
            coop::config::ReadIniValue("vitals_keepalive_sec", "0").c_str());
        if (sec <= 0) return 0LL;
        return static_cast<long long>(sec < 30 ? 30 : sec) * 1000LL;
    }();
    return s;
}

long long g_lastFireMs = 0;

}  // namespace

void Tick() {
    const long long period = PeriodMs();
    if (period == 0) return;
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

}  // namespace coop::dev::vitals_keepalive

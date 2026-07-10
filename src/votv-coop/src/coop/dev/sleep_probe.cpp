// coop/dev/sleep_probe.cpp -- see coop/dev/sleep_probe.h.

#include "coop/dev/sleep_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sleep.h"

#include <chrono>

namespace coop::dev::sleep_probe {
namespace {

namespace SLP = ue_wrap::sleep;
using Clock = std::chrono::steady_clock;

bool g_checked = false;
bool g_enabled = false;
bool g_armed = false;
Clock::time_point g_t0{};
int g_phase = 0;  // 0 = idle, 1 = slept (the role's own step done), 2 = host woke (done)

void TrySleep(const char* role) {
    if (!SLP::EnsureResolved()) {
        UE_LOGW("sleep_probe: %s sleep step skipped -- sleep surface unresolved", role);
        return;
    }
    void* bed = SLP::FindBed();
    if (!bed) {
        UE_LOGW("sleep_probe: %s sleep step skipped -- no bed_C in the world", role);
        return;
    }
    const bool ok = SLP::CallSleep(bed);
    UE_LOGI("sleep_probe: %s sleep() dispatched (bed=%p ok=%d isSleep=%d dilation=%.1f)",
            role, bed, ok ? 1 : 0, SLP::IsSleeping() ? 1 : 0, SLP::GetGlobalTimeDilation());
}

}  // namespace

void Install() {
    if (g_checked) return;
    g_checked = true;
    g_enabled = ::coop::config::MasterEnabled() &&
                ::coop::config::IsIniKeyTrue("sleep_probe");
    if (g_enabled)
        UE_LOGI("sleep_probe: ARMED (ini sleep_probe=1) -- client sleeps T+15s, host sleeps "
                "T+25s (expect ACCELERATE), host wakeup T+40s (expect END natural=0)");
}

void Tick(bool isConnected, bool isHost) {
    if (!g_enabled || g_phase >= 2) return;
    if (!isConnected) { g_armed = false; return; }
    const auto now = Clock::now();
    if (!g_armed) {
        g_armed = true;
        g_t0 = now;
        return;
    }
    const auto dt = now - g_t0;
    if (g_phase == 0) {
        if (!isHost && dt >= std::chrono::seconds(15)) {
            g_phase = 1;
            TrySleep("CLIENT");
        } else if (isHost && dt >= std::chrono::seconds(25)) {
            g_phase = 1;
            TrySleep("HOST");
        }
    } else if (g_phase == 1 && isHost && dt >= std::chrono::seconds(40)) {
        g_phase = 2;
        UE_LOGI("sleep_probe: HOST manual wakeup() (the early-interrupt END path; "
                "dilation before=%.1f)", SLP::GetGlobalTimeDilation());
        SLP::CallWakeup();
    } else if (g_phase == 1 && !isHost && dt >= std::chrono::seconds(50)) {
        g_phase = 2;  // client side done -- the END should have arrived via the wire
        UE_LOGI("sleep_probe: CLIENT final state (isSleep=%d dilation=%.1f)",
                SLP::IsSleeping() ? 1 : 0, SLP::GetGlobalTimeDilation());
    }
}

}  // namespace coop::dev::sleep_probe

// coop/dev/blackout_drill.cpp -- see coop/dev/blackout_drill.h.

#include "coop/dev/blackout_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/power_control.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::blackout_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace PC = ue_wrap::power_control;
namespace EF = coop::event_fire_sync;
using Clock = std::chrono::steady_clock;

// A client whose panel has not read every breaker cut this long after its join ends the run.
constexpr auto kCutBound = std::chrono::seconds(60);
constexpr auto kReadEvery = std::chrono::milliseconds(250);

void*   g_panel = nullptr;
int32_t g_panelIdx = -1;
bool    g_fired = false;  // host: the solar fire went out
bool    g_done = false;
Clock::time_point g_since{};
Clock::time_point g_nextRead{};

long long MsSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// The power panel's breaker mask, or -1 while there is none to read. The panel is one map actor: one
// walk of the object array finds it, and its slot and serial keep it.
int ReadMask() {
    if (!PC::EnsureResolved()) return -1;
    if (!g_panel || !R::IsLiveByIndex(g_panel, g_panelIdx)) {
        g_panel = nullptr;
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n && !g_panel; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o) || !PC::IsPowerControl(o)) continue;
            if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
            g_panel = o;
            g_panelIdx = R::InternalIndexOf(o);
        }
        if (!g_panel) return -1;
    }
    uint8_t mask = 0;
    return PC::ReadPress(g_panel, mask) ? mask : -1;
}

void Done(const char* line) {
    g_done = true;
    UE_LOGI("[BLACKOUT-DRILL] %s", line);
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::blackout_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_done || !session || !session->connected()) return;
    if (Clock::now() < g_nextRead) return;
    g_nextRead = Clock::now() + kReadEvery;
    if (coop::roster::LocalIsHost()) {
        if (!g_fired) {
            if (!session->AnyWorldReadyPeer()) return;
            const int before = ReadMask();
            g_fired = EF::HostFire(EF::FireKind::RunEvent, L"solar", L"None");
            g_since = Clock::now();
            UE_LOGI("[BLACKOUT-DRILL] host FIRED solar once a client's world was ready (breakers %d before), sent=%d",
                    before, g_fired ? 1 : 0);
            if (!g_fired) Done("host DONE: the fire was refused -- INCONCLUSIVE");
            return;
        }
        if (ReadMask() == 0) {
            UE_LOGI("[BLACKOUT-DRILL] host: its breakers read cut %lld ms after the fire", MsSince(g_since));
            Done("host DONE");
        }
        return;
    }
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    if (g_since == Clock::time_point{}) g_since = Clock::now();
    const int mask = ReadMask();
    if (mask == 0) {
        UE_LOGI("[BLACKOUT-DRILL] client: its breakers read cut %lld ms after its join", MsSince(g_since));
        Done("client DONE");
    } else if (Clock::now() - g_since > kCutBound) {
        UE_LOGW("[BLACKOUT-DRILL] client: its breakers still read %d %lld s after its join -- FAIL", mask,
                static_cast<long long>(kCutBound.count()));
        Done("client DONE -- FAIL");
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_panel = nullptr;
    g_panelIdx = -1;
    g_fired = false;
    g_done = false;
    g_since = g_nextRead = {};
}

}  // namespace coop::dev::blackout_drill

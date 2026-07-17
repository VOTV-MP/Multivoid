#include "coop/dev/force_weather.h"

#include "coop/net/session.h"
#include "coop/world/weather_sync.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>

namespace coop::dev::force_weather {

namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Toggle state. SetSnow flips this; the menu checkbox reads it via IsSnowOn.
// Default false (matches the cycle's initial isSnow=false).
std::atomic<bool> g_snowOn{false};

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void SetSnow(bool on) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGI("force_weather: SetSnow ignored -- weather is host-authoritative "
                "(local role is not Host)");
        return;
    }
    g_snowOn.store(on, std::memory_order_release);
    GT::Post([on] {
        if (!coop::weather_sync::DebugForceSnow(on)) {
            UE_LOGW("force_weather: DebugForceSnow(%d) failed -- cycle not yet live "
                    "or UFunction unresolved", on ? 1 : 0);
        }
    });
    UE_LOGI("force_weather: snow set to %d (host -> intComs_triggerSnow broadcast)", on ? 1 : 0);
}

bool IsSnowOn() { return g_snowOn.load(std::memory_order_acquire); }

}  // namespace coop::dev::force_weather

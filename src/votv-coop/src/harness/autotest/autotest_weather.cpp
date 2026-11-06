// harness/autotest/autotest_weather.cpp -- the weather-sync test: forced rain cycles
// (VOTVCOOP_RUN_WEATHER_TEST), a host-only driver; clients apply through the wire.
//
// After the settle, DebugForceRain forces rain on, off, on, off at 5 s intervals; the observer on
// setRainProperties broadcasts each. Pass: two or more host broadcast lines, two or more client
// applied lines, and both peers' isRaining diagnostic matching after each toggle. Blocks about
// 30 s.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/world/weather_rain.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

}  // namespace

// ---- autonomous weather sync test ------------------------------
// Host-only. Once the session is connected and the pose has settled, the host calls
// coop::weather_rain::DebugForceRain through GT::Post, which writes enable_rain=true and calls
// causeRain, setRainProperties and setWindParameters. Each forced change
// broadcasts a WeatherState packet, caught by the host's POST observer on setRainProperties and
// causeRain, and the client applies it through the mutator UFunctions on its own cycle.
//
// What lands in the logs: `weather: DebugForceRain ...` and `weather: host broadcast ...` on the
// host, `weather: applied flags 0x... ...` on the client, and an isRaining diagnostic on both --
// read here on the host, and by the per-tick diagnostic in weather_sync.cpp's TickConnect path on
// the client.
//
// Four cycles, ON / OFF / ON / OFF, six seconds apart, because the rain particle systems take a
// second or two to start and the audio ramps behind them. The final state is OFF, so the next run
// is clean.
void RunAutonomousWeatherTest() {
    const bool isHost = !IsClientRole();
    if (!isHost) {
        UE_LOGI("weather_test: not host -- this routine is host-only "
                "(client observes via wire). Returning.");
        return;
    }
    UE_LOGI("weather_test: starting autonomous routine on host (waiting "
            "20 s for stabilization: pose settle + cycle Install + session connect)");
    ::Sleep(20000);

    // Snapshot pre-test state for diagnostics.
    {
        auto found = std::make_shared<std::atomic<int>>(0);
        auto state = std::make_shared<std::atomic<bool>>(false);
        GT::Post([found, state] {
            bool ok = false;
            const bool rain = coop::weather_rain::ReadLocalIsRaining(&ok);
            state->store(rain, std::memory_order_release);
            found->store(ok ? 1 : -1, std::memory_order_release);
        });
        while (found->load() == 0) ::Sleep(5);
        const int code = found->load();
        if (code < 0) {
            UE_LOGW("weather_test: cycle not live on host yet -- aborting "
                    "(retry test after the world finishes loading)");
            return;
        }
        UE_LOGI("weather_test: host pre-test isRaining=%d",
                state->load() ? 1 : 0);
    }

    struct Phase { bool on; const char* label; float strength; };
    const Phase phases[] = {
        { true,  "ON-1",  1.0f },
        { false, "OFF-1", 0.0f },
        { true,  "ON-2",  1.0f },
        { false, "OFF-2", 0.0f },
    };

    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        const Phase& ph = phases[i];
        UE_LOGI("weather_test: phase %zu/%zu (%s) -- DebugForceRain(isRaining=%d, strength=%.1f)",
                i + 1, sizeof(phases) / sizeof(phases[0]),
                ph.label, ph.on ? 1 : 0, ph.strength);

        auto callDone = std::make_shared<std::atomic<int>>(0);
        const bool on = ph.on;
        const float strength = ph.strength;
        GT::Post([on, strength, callDone] {
            const bool ok = coop::weather_rain::DebugForceRain(on, strength);
            callDone->store(ok ? 1 : -1, std::memory_order_release);
        });
        while (callDone->load() == 0) ::Sleep(5);
        if (callDone->load() < 0) {
            UE_LOGW("weather_test: phase %s failed (DebugForceRain returned false) -- "
                    "abort", ph.label);
            return;
        }

        // 6 s spacing: lets the wire packet land + receiver apply +
        // particle/audio start on the client + screenshot timing window.
        ::Sleep(6000);

        // Post-phase state diagnostic on host.
        auto readDone = std::make_shared<std::atomic<int>>(0);
        auto readState = std::make_shared<std::atomic<bool>>(false);
        GT::Post([readDone, readState] {
            bool ok = false;
            const bool rain = coop::weather_rain::ReadLocalIsRaining(&ok);
            readState->store(rain, std::memory_order_release);
            readDone->store(ok ? 1 : -1, std::memory_order_release);
        });
        while (readDone->load() == 0) ::Sleep(5);
        UE_LOGI("weather_test: phase %s settle -- host isRaining=%d "
                "(expected=%d after DebugForceRain)",
                ph.label,
                readDone->load() > 0 ? (readState->load() ? 1 : 0) : -1,
                ph.on ? 1 : 0);
    }

    UE_LOGI("weather_test: DONE -- %zu phases on host (final state should be OFF)",
            sizeof(phases) / sizeof(phases[0]));
}

DWORD WINAPI WeatherTestThread(LPVOID /*arg*/) {
    RunAutonomousWeatherTest();
    return 0;
}

}  // namespace harness::autotest

// harness/autotest_weather.cpp -- the Phase 5W weather-sync tests: forced
// rain cycles (VOTVCOOP_RUN_WEATHER_TEST) + the red-sky variant
// (VOTVCOOP_RUN_REDSKY_TEST). Both host-only drivers; clients apply via the
// wire. Extracted verbatim from harness/autotest.cpp (2026-07-19 dissolve);
// interfaces + docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/world/weather_rain.h"
#include "coop/world/weather_redsky.h"
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

// ---- Phase 5W autonomous weather sync test ------------------------------
//
// Host-only. After session-Connected + autotest-pose settle, the host calls
// coop::weather_rain::DebugForceRain via GT::Post -- which writes
// enable_rain=true + calls setRainProperties + causeRain + setWindParameters
// per the proper-invocation RE pass (2026-05-27, RULE 1). Each forced
// state change broadcasts a WeatherState packet (the host POST observer
// on setRainProperties / causeRain catches the call). Client receives,
// applies via mutator UFunctions on its local cycle.
//
// Verification (logs only -- visual is for the user / OBS):
//   - Host log: `weather: DebugForceRain ...` + `weather: host broadcast ...`
//   - Client log: `weather: applied flags 0x... ...`
//   - Both peers also log isRaining bool diagnostic each cycle (read by
//     this routine on host, by a separate per-tick diagnostic on client --
//     see the periodic state-read in weather_sync.cpp's TickConnect path).
//
// 4 cycles: ON / OFF / ON / OFF at 6-second spacing (rain particle
// systems take ~1-2s to start + audio to ramp; 6s is comfortable for
// screenshot capture mid-cycle). Final state = OFF so the next test run
// starts clean.
void RunAutonomousWeatherTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
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

// ---- Phase 5W Inc-fix-2 autonomous RED SKY test ------------------------
//
// Host-only. After stabilization, fires DebugForceRedSky(true) -- spawns
// AredSkyEvent_C on the gamemode + the BP swaps the 4 color-curve assets
// to the "red" set. Host's POST observer on spawnRedSky catches +
// broadcasts; client invokes the same. Both peers' subsequent
// screenshots should show the entire sky / ambient lighting in red.
//
// 2 phases: ON / OFF (revert). Final state OFF so the next test run
// starts clean. 10 s ON dwell gives the visual change ample time to
// settle + the screenshot window to capture.
void RunAutonomousRedSkyTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
    if (!isHost) {
        UE_LOGI("redsky_test: not host -- this routine is host-only "
                "(client observes via wire). Returning.");
        return;
    }
    UE_LOGI("redsky_test: starting autonomous routine on host (waiting "
            "20 s for stabilization)");
    ::Sleep(20000);

    UE_LOGI("redsky_test: phase ON -- DebugForceRedSky(true)");
    auto onDone = std::make_shared<std::atomic<int>>(0);
    GT::Post([onDone] {
        const bool ok = coop::weather_redsky::DebugForce(true);
        onDone->store(ok ? 1 : -1, std::memory_order_release);
    });
    while (onDone->load() == 0) ::Sleep(5);
    if (onDone->load() < 0) {
        UE_LOGW("redsky_test: ON phase failed (DebugForceRedSky returned false)");
        return;
    }

    // 10 s ON dwell -- ample for client to receive + apply + screenshot.
    ::Sleep(10000);

    UE_LOGI("redsky_test: phase OFF -- DebugForceRedSky(false)");
    auto offDone = std::make_shared<std::atomic<int>>(0);
    GT::Post([offDone] {
        const bool ok = coop::weather_redsky::DebugForce(false);
        offDone->store(ok ? 1 : -1, std::memory_order_release);
    });
    while (offDone->load() == 0) ::Sleep(5);

    // 6 s OFF dwell -- color curves revert; verify both peers return to
    // normal coloration.
    ::Sleep(6000);

    UE_LOGI("redsky_test: DONE (ON+OFF cycle complete; final state should "
            "be normal sky)");
}

DWORD WINAPI RedSkyTestThread(LPVOID /*arg*/) {
    RunAutonomousRedSkyTest();
    return 0;
}

}  // namespace harness::autotest

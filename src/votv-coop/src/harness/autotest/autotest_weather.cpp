// harness/autotest_weather.cpp -- the weather-sync tests: forced rain cycles
// (VOTVCOOP_RUN_WEATHER_TEST) and the red-sky variant (VOTVCOOP_RUN_REDSKY_TEST). Both are
// host-only drivers; clients apply through the wire. Interfaces and docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/world/weather_rain.h"
#include "coop/world/weather_redsky.h"
#include "coop/player/puppet_drive.h"    // Puppet(1) -- the redsky ready-peer wait
#include "coop/player/remote_player.h"   // RemotePlayer::GetActor (the wait's liveness read)
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

// ---- autonomous RED SKY test ------------------------
// Host-only. After stabilization it fires DebugForceRedSky(true), which spawns AredSkyEvent_C on
// the gamemode and lets the blueprint swap the four colour-curve assets to the red set. The host's
// field poll (weather_redsky::HostPollEdge) sees the edge within 500 ms and broadcasts; the client
// applies the same chain, and the whole sky and ambient lighting turn red on both peers.
//
// Two phases, ON then OFF, with the final state OFF so the next run starts clean. The ten-second ON
// dwell gives the visual change time to settle.
void RunAutonomousRedSkyTest() {
    const bool isHost = !IsClientRole();
    if (!isHost) {
        UE_LOGI("redsky_test: not host -- this routine is host-only "
                "(client observes via wire). Returning.");
        return;
    }
    UE_LOGI("redsky_test: starting autonomous routine on host (waiting for a "
            "WORLD-READY peer -- the broadcast needs a recipient; up to 180 s)");
    // Wait for a peer that is actually world-ready rather than for a fixed delay: a cold client's
    // join does a double level load, so a blind wait sends RedSky to zero ready peers and proves
    // nothing. The slot-1 puppet existing means the peer is connected, streaming and world-ready.
    for (int attempt = 0; attempt < 180; ++attempt) {
        auto ready = std::make_shared<std::atomic<int>>(0);
        GT::Post([ready] {
            void* p = coop::puppet_drive::Puppet(1).GetActor();
            ready->store(p ? 1 : -1, std::memory_order_release);
        });
        while (ready->load() == 0) ::Sleep(5);
        if (ready->load() == 1) break;
        ::Sleep(1000);
    }
    ::Sleep(3000);  // small settle past the join seed window

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

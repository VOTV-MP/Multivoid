// harness/autotest_fog_probe.cpp -- SP-solo fog ON/OFF model + clear-path probe.
//
// The host-authoritative weather fix must, on host-clear, ACTIVELY clear the
// client's fog (hands-on bug 2026-06-01: client STRONG MIST while host CLEAR).
// The 4-agent RE workflow (2026-06-01) established the clear path -- destroy the
// rolling-fog actor AweatherFogController_C (cycle->fogEventObject @0x0338) +
// any live AsuperFog_C + zero finalFogDensity/thickFog + call SetFogDensity() --
// but left ONE thing unconfirmed that decides the WIRE design: during an active
// fog event, is thickFog a STABLE high TARGET (so a single broadcast can carry
// full fog intensity via the client's own ramp) or is finalFogDensity driven
// continuously (needing a density stream)? Per the deep-RE rule (runtime
// CONFIRM, no iterative shenanigans) this probe answers it on ONE instance
// before any protocol change is written.
//
// Sequence (all engine work on the game thread via GT::Post):
//   1. settle until the singleton daynightCycle_C resolves (gameplay world).
//   2. FORCE rolling fog (enable_fog=1 + fogProbability=1 + spawnFog()) AND
//      super fog (enable_superfog=1 + superFogEvent()).  [game's own verbs]
//   3. sample finalFogDensity / thickFog / fogEventObject!=null / superFog_C
//      count every ~1 s for ~12 s -> the density-vs-target evolution = THE MODEL.
//   4. run the CLEAR sequence (the production fix's mechanics, in isolation).
//   5. sample ~6 s more -> confirms density stays ~0 and the actors are gone
//      (clear-path efficacy).  Logs a VERDICT line.
//
// Gated by env VOTVCOOP_RUN_FOG_PROBE=1; launch `mp.py fogprobe` (solo).
// Emits FOG-FORCED READY / FOG-CLEARED READY screenshot markers. Throwaway
// diagnostic -- NOT a shipping path; the production fix lives in weather_sync.

#include "harness/autotest.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

// Bounded spin-wait on a game-thread task's completion flag (mirrors the sibling
// probes). false => the posted task faulted (SEH firewall ate the AV, flag never
// set) so the caller bails instead of hanging.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// One observable snapshot of the cycle's fog state (read on the game thread).
struct FogSample {
    bool  ok            = false;  // cycle was live and read
    float finalDensity  = 0.f;    // finalFogDensity @0x0418 (active height-fog density)
    float thickFog      = 0.f;    // thickFog @0x0330 (the per-tick density TARGET)
    bool  enableFog     = false;  // enable_fog @0x0449
    bool  enableSuperfog= false;  // enable_superfog @0x044A
    bool  permanentFog  = false;  // permanentFog @0x042D
    bool  rollingActor  = false;  // fogEventObject @0x0338 != null (rolling fog live)
    int   superFogCount = 0;      // live AsuperFog_C instances
    // AweatherFogController_C ramp state (read when rollingActor) -- to design the
    // late-joiner SNAP (copy host actor state onto the mirror so it doesn't ramp from 0).
    float fogTime = 0.f, fogAlpha = 0.f, fogDuration = 0.f, fogPhase = 0.f, fogStrength = 0.f;
};

// Read the cycle fog state on the game thread. Returns ok=false if no live cycle.
FogSample ReadFogGT() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out  = std::make_shared<FogSample>();
    GT::Post([done, out] {
        void* cycle = R::FindObjectByClass(P::name::DaynightCycleClass);
        if (cycle && R::IsLive(cycle)) {
            auto* b = reinterpret_cast<uint8_t*>(cycle);
            out->finalDensity   = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity);
            out->thickFog       = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_thickFog);
            out->enableFog      = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog);
            out->enableSuperfog = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog);
            out->permanentFog   = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog);
            void* fogActor = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
            out->rollingActor   = fogActor != nullptr;
            if (fogActor && R::IsLive(fogActor)) {
                auto* fb = reinterpret_cast<uint8_t*>(fogActor);
                out->fogTime     = *reinterpret_cast<float*>(fb + P::off::WeatherFogController_Time);
                out->fogAlpha    = *reinterpret_cast<float*>(fb + P::off::WeatherFogController_Alpha);
                out->fogDuration = *reinterpret_cast<float*>(fb + P::off::WeatherFogController_Duration);
                out->fogPhase    = *reinterpret_cast<float*>(fb + P::off::WeatherFogController_fogPhase);
                out->fogStrength = *reinterpret_cast<float*>(fb + P::off::WeatherFogController_Strength);
            }
            out->superFogCount  = R::CountObjectsByClass(P::name::SuperFogClass);
            out->ok = true;
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    return *out;
}

// Call a no-arg UFunction on the cycle by name. Game-thread caller.
bool CallCycleFn(void* cycle, const wchar_t* fnName) {
    if (!cycle || !R::IsLive(cycle)) return false;
    void* fn = R::FindFunction(R::ClassOf(cycle), fnName);
    if (!fn) { UE_LOGW("fogprobe: UFunction '%ls' not resolved", fnName); return false; }
    ue_wrap::ParamFrame f(fn);
    return ue_wrap::Call(cycle, f);
}

void LogSample(const char* tag, int i, const FogSample& s) {
    if (!s.ok) { UE_LOGW("fogprobe: [%s #%d] no live cycle", tag, i); return; }
    UE_LOGI("fogprobe: [%s #%d] finalFogDensity=%.4f thickFog=%.4f rollingActor=%d "
            "superFogCount=%d enable_fog=%d enable_superfog=%d permanentFog=%d "
            "| actor: Time=%.3f Alpha=%.4f Duration=%.3f fogPhase=%.4f Strength=%.4f",
            tag, i, s.finalDensity, s.thickFog, s.rollingActor ? 1 : 0,
            s.superFogCount, s.enableFog ? 1 : 0, s.enableSuperfog ? 1 : 0,
            s.permanentFog ? 1 : 0,
            s.fogTime, s.fogAlpha, s.fogDuration, s.fogPhase, s.fogStrength);
}

void RunProbe() {
    UE_LOGI("fogprobe: === fog ON/OFF model + clear-path probe START ===");

    // Settle: wait until the singleton cycle resolves (covers VOTV's boot-time
    // `open untitled_1` travel). The cycle only exists in the gameplay world.
    void* cycleSettled = nullptr;
    for (int i = 0; i < 40; ++i) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto out  = std::make_shared<void*>(nullptr);
        GT::Post([done, out] {
            void* c = R::FindObjectByClass(P::name::DaynightCycleClass);
            *out = (c && R::IsLive(c)) ? c : nullptr;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (*out) { cycleSettled = *out; break; }
        ::Sleep(1000);
    }
    if (!cycleSettled) {
        UE_LOGW("fogprobe: no live daynightCycle_C after 40 s -- abort");
        UE_LOGI("fogprobe: DONE");
        return;
    }
    UE_LOGI("fogprobe: cycle live -- forcing fog ON (rolling + super)");

    // FORCE fog ON via the game's own spawner verbs. Belt: pass the config gates
    // (enable_fog/enable_superfog true, fogProbability=1) so any internal roll in
    // the BP body passes -- mirrors DebugForceRain's enable_rain precondition.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([done] {
            void* cycle = R::FindObjectByClass(P::name::DaynightCycleClass);
            if (cycle && R::IsLive(cycle)) {
                auto* b = reinterpret_cast<uint8_t*>(cycle);
                *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog)      = true;
                *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog) = true;
                *reinterpret_cast<float*>(b + P::off::AdaynightCycle_fogProbability) = 1.0f;
                const bool sf = CallCycleFn(cycle, P::name::DaynightCycle_spawnFogFn);
                const bool su = CallCycleFn(cycle, P::name::DaynightCycle_superFogEventFn);
                UE_LOGI("fogprobe: force-on dispatched spawnFog=%d superFogEvent=%d", sf ? 1 : 0, su ? 1 : 0);
            }
            done->store(1);
        });
        WaitDone(done, 8000);
    }

    // Give the spawn a beat, then mark the foggy screenshot.
    ::Sleep(2500);
    FogSample forced = ReadFogGT();
    LogSample("forced", 0, forced);
    UE_LOGI("fogprobe: FOG-FORCED READY");  // mp.py captures the foggy window here
    ::Sleep(2000);

    // SAMPLE the evolution: does thickFog hold a stable high target while
    // finalFogDensity ramps toward it (=> stream thickFog, single broadcast), or
    // is finalFogDensity driven continuously (=> needs a density stream)?
    for (int i = 1; i <= 10; ++i) {
        ::Sleep(1000);
        LogSample("active", i, ReadFogGT());
    }

    // CLEAR sequence (the production fix's mechanics, in isolation). Order:
    // destroy actors FIRST (stop their tick re-driving density), THEN zero
    // density + SetFogDensity, THEN flip config bits.
    UE_LOGI("fogprobe: applying CLEAR sequence");
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([done] {
            void* cycle = R::FindObjectByClass(P::name::DaynightCycleClass);
            if (!cycle || !R::IsLive(cycle)) { done->store(1); return; }
            auto* b = reinterpret_cast<uint8_t*>(cycle);

            // (1) destroy the rolling-fog actor + null its slot.
            void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
            int rollingDestroyed = 0;
            if (*slot && R::IsLive(*slot)) { E::DestroyActor(*slot); rollingDestroyed = 1; }
            *slot = nullptr;

            // (2) destroy any live AsuperFog_C (FindObjectByClass returns the
            // first; loop bounded until count==0).
            int superDestroyed = 0;
            for (int k = 0; k < 8; ++k) {
                void* sf = R::FindObjectByClass(P::name::SuperFogClass);
                if (!sf || !R::IsLive(sf)) break;
                E::DestroyActor(sf);
                ++superDestroyed;
            }

            // (3) zero the height-fog density + push it onto the component NOW.
            *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity) = 0.f;
            *reinterpret_cast<float*>(b + P::off::AdaynightCycle_thickFog)        = 0.f;
            const bool sfd = CallCycleFn(cycle, P::name::DaynightCycle_setFogDensityFn);

            // (4) flip config bits so the scheduler won't immediately re-arm.
            *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog)      = false;
            *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog) = false;
            *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog)    = false;

            UE_LOGI("fogprobe: CLEAR applied (rollingDestroyed=%d superDestroyed=%d SetFogDensity=%d)",
                    rollingDestroyed, superDestroyed, sfd ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }

    ::Sleep(1500);
    FogSample cleared0 = ReadFogGT();
    LogSample("cleared", 0, cleared0);
    UE_LOGI("fogprobe: FOG-CLEARED READY");  // mp.py captures the cleared window here
    ::Sleep(1500);

    // Confirm it STAYS cleared (density not re-inflated by a surviving driver).
    FogSample last = cleared0;
    for (int i = 1; i <= 5; ++i) {
        ::Sleep(1000);
        last = ReadFogGT();
        LogSample("cleared", i, last);
    }

    const bool cleared = last.ok && last.finalDensity < 0.001f &&
                         !last.rollingActor && last.superFogCount == 0;
    UE_LOGI("fogprobe: VERDICT cleared=%d (finalFogDensity=%.4f rollingActor=%d superFogCount=%d) "
            "-- destroy-actors+SetFogDensity %s the host-authoritative clear path",
            cleared ? 1 : 0, last.finalDensity, last.rollingActor ? 1 : 0, last.superFogCount,
            cleared ? "VALIDATES" : "does NOT yet validate");
    UE_LOGI("fogprobe: DONE");
}

}  // namespace

void RunFogProbe() { RunProbe(); }
DWORD WINAPI FogProbeThread(LPVOID) { RunFogProbe(); return 0; }

}  // namespace harness::autotest

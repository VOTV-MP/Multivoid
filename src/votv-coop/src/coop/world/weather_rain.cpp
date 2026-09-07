// coop/world/weather_rain.cpp -- rain and snow, the cycle-side sub-lane of weather sync. See
// coop/world/weather_rain.h. The log lines keep the "weather:" prefix so log-driven tests can
// grep them.

#include "coop/world/weather_rain.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>

namespace coop::weather_rain {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// The five rain, snow and wind mutator UFunctions, resolved once in Install; a UFunction
// pointer is class-stable across cycle recreation, so there is no re-latch machinery.
void* g_causeRainFn          = nullptr;
void* g_setRainPropertiesFn  = nullptr;
void* g_setWindParametersFn  = nullptr;
void* g_intComsTriggerSnowFn = nullptr;
void* g_setRainParticlesFn   = nullptr;

// The module latch, true once all five mutators are resolved.
bool g_installed = false;

// The session pointer for the debug entry points' host-role checks; atomic, so a read cannot
// race the harness setter on another thread.
std::atomic<coop::net::Session*> g_session{nullptr};

// The echo suppress for causeRain. Its blueprint body rolls random values and eases them, so
// it re-rolls the rain strength every time it fires; when the receiver calls it to apply the
// host's state, the rolls would overwrite the strength just set, and the client would drift
// from the host. A PRE interceptor on causeRain cancels its body while this flag is set, and
// the flag is set only around the receiver's call; the host's own calls and the organic
// scheduler run normally. Atomic, since the interceptor reads it in the dispatch detour.
std::atomic<bool> g_causeRainEchoSuppress{false};

// The cycle cache for the entry points that cannot take the cycle as a parameter, revalidated
// by index like weather_sync's canonical cache; cleared in OnDisconnect. Game thread only.
void* g_cycleCache = nullptr;
int32_t g_cycleIdx = -1;

void* ResolveCycle() {
    if (g_cycleCache && R::IsLiveByIndex(g_cycleCache, g_cycleIdx)) return g_cycleCache;
    g_cycleCache = R::FindObjectByClass(P::name::DaynightCycleClass);
    g_cycleIdx = g_cycleCache ? R::InternalIndexOf(g_cycleCache) : -1;
    return g_cycleCache;
}

// The echo-suppress interceptor for causeRain: cancels the blueprint body only while the flag
// is set; a pass-through otherwise.
bool OnCauseRainPreEchoSuppress(void* /*self*/, void* /*params*/) {
    if (g_causeRainEchoSuppress.load(std::memory_order_acquire)) {
        // Skip the body, so its random rolls do not overwrite the strength just set.
        return true;
    }
    return false;  // normal pass-through
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool Install() {
    if (!g_installed) {
        void* cls = R::FindClass(P::name::DaynightCycleClass);
        if (!cls) return false;

        struct Entry { const wchar_t* name; void** out; };
        Entry mut[] = {
            { P::name::DaynightCycle_causeRainFn,          &g_causeRainFn          },
            { P::name::DaynightCycle_setRainPropertiesFn,  &g_setRainPropertiesFn  },
            { P::name::DaynightCycle_setWindParametersFn,  &g_setWindParametersFn  },
            { P::name::DaynightCycle_intComsTriggerSnowFn, &g_intComsTriggerSnowFn },
            { P::name::DaynightCycle_setRainParticlesFn,   &g_setRainParticlesFn   },
        };
        int mResolved = 0;
        for (auto& e : mut) {
            if (*e.out) { ++mResolved; continue; }
            if (void* fn = R::FindFunction(cls, e.name)) { *e.out = fn; ++mResolved; }
        }
        // All five mutators are required, so the client can apply every state delta.
        if (mResolved != 5) return false;
        g_installed = true;
        UE_LOGI("weather: rain module resolved -- 5/5 mutator UFunctions on daynightCycle_C");
    }

    // The echo-suppress interceptor on causeRain, registered on both roles, once per process; it
    // is a pass-through unless the flag is set, and the flag is set only around the receiver's
    // call.
    static bool sCauseRainInterceptorReg = false;
    if (!sCauseRainInterceptorReg && g_causeRainFn) {
        if (GT::RegisterInterceptor(g_causeRainFn, &OnCauseRainPreEchoSuppress)) {
            sCauseRainInterceptorReg = true;
            UE_LOGI("weather: causeRain echo-suppress PRE interceptor registered "
                    "(@%p; conditional on g_causeRainEchoSuppress flag)",
                    g_causeRainFn);
        } else {
            UE_LOGW("weather: causeRain echo-suppress interceptor registration FAILED "
                    "(interceptor table full?)");
        }
    }
    return true;
}

void ReadState(void* cycle, coop::net::WeatherStatePayload& out) {
    const uint8_t* base = reinterpret_cast<uint8_t*>(cycle);
    const bool isRaining       = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_isRaining);
    const bool isSnow          = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_isSnow);
    const bool enable_rain     = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_enable_rain);
    const bool enable_fog      = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_enable_fog);
    const bool enable_superfog = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_enable_superfog);
    const bool enableSunlight  = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_enableSunlight);
    const bool enableMoonlight = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_enableMoonlight);
    const bool permanentRain   = *reinterpret_cast<const bool*>(base + P::off::AdaynightCycle_permanentRain);
    out.flags = 0;
    if (isRaining)       out.flags |= coop::net::weather_flags::kIsRaining;
    if (isSnow)          out.flags |= coop::net::weather_flags::kIsSnow;
    if (enable_rain)     out.flags |= coop::net::weather_flags::kEnableRain;
    if (enable_fog)      out.flags |= coop::net::weather_flags::kEnableFog;
    if (enable_superfog) out.flags |= coop::net::weather_flags::kEnableSuperfog;
    if (enableSunlight)  out.flags |= coop::net::weather_flags::kEnableSunlight;
    if (enableMoonlight) out.flags |= coop::net::weather_flags::kEnableMoonlight;
    if (permanentRain)   out.flags |= coop::net::weather_flags::kPermanentRain;

    out.rainStrength         = *reinterpret_cast<const float*>(base + P::off::AdaynightCycle_rainStrength);
    out.rainLightningChance  = *reinterpret_cast<const float*>(base + P::off::AdaynightCycle_rainLightningChance);
    out.rainDeactivateChance = *reinterpret_cast<const float*>(base + P::off::AdaynightCycle_rainDeactivateChance);
    out.rainWindSpeed        = *reinterpret_cast<const float*>(base + P::off::AdaynightCycle_rainWindSpeed);
    // The rain strength's ease target: the strength above is the interpolated current value, and
    // the tick eases it toward the target, so a late joiner given only the strength would watch it
    // decay toward its local target; the target is carried too and anchored on apply.
    out.rain                 = *reinterpret_cast<const float*>(base + P::off::AdaynightCycle_rain);
}

void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload,
                   const coop::net::WeatherStatePayload& cur, ApplyOutcome& outcome) {
    uint8_t* base = reinterpret_cast<uint8_t*>(cycle);

    const uint8_t newFlags = payload.flags;
    const uint8_t curFlags = cur.flags;
    using namespace coop::net::weather_flags;

    // The receiver drives the game's own UFunction chain, mirroring the host's debug trigger: the
    // config bits by direct write (static config bools with no blueprint listeners), then
    // setRainProperties, causeRain with the echo suppress, setRainParticles, the snow trigger and
    // the wind parameters.

    const bool newRain = (newFlags & kIsRaining) != 0;
    const bool curRain = (curFlags & kIsRaining) != 0;

    // Step A: the config bits, direct writes (static config bools with no listeners, and no setter
    // UFunctions in the dump, so this is the canonical path). The fog bits ride with the fog
    // actor's apply instead.
    if (((curFlags ^ newFlags) & kEnableRain) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enable_rain) =
            (newFlags & kEnableRain) != 0;
    if (((curFlags ^ newFlags) & kEnableSunlight) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enableSunlight) =
            (newFlags & kEnableSunlight) != 0;
    if (((curFlags ^ newFlags) & kEnableMoonlight) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enableMoonlight) =
            (newFlags & kEnableMoonlight) != 0;
    if (((curFlags ^ newFlags) & kPermanentRain) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_permanentRain) =
            (newFlags & kPermanentRain) != 0;

    const bool rainStateChanged =
        newRain != curRain ||
        cur.rainStrength != payload.rainStrength ||
        cur.rainLightningChance != payload.rainLightningChance ||
        cur.rainDeactivateChance != payload.rainDeactivateChance ||
        cur.rainWindSpeed != payload.rainWindSpeed;

    // Step B: setRainProperties writes the flag and the scalars through the canonical UFunction,
    // in the host's own order.
    if (rainStateChanged && g_setRainPropertiesFn) {
        ue_wrap::ParamFrame f(g_setRainPropertiesFn);
        f.Set<bool>(L"isRaining",            newRain);
        f.Set<float>(L"rainStrength",        payload.rainStrength);
        f.Set<float>(L"rainLightningChance", payload.rainLightningChance);
        f.Set<float>(L"rainDeactivateChance",payload.rainDeactivateChance);
        f.Set<float>(L"rainWindSpeed",       payload.rainWindSpeed);
        ue_wrap::Call(cycle, f);
    }

    // Anchor the ease target: setRainProperties wrote the current strength, but the tick eases it
    // toward the target every frame, so a late joiner whose local target is 0 would watch the
    // synced strength decay to 0. Pinned whenever it differs, since gating on the state change
    // alone would miss a target-only drift during a continuous-rain episode.
    if (cur.rain != payload.rain) {
        *reinterpret_cast<float*>(base + P::off::AdaynightCycle_rain) = payload.rain;
    }

    // Step C: causeRain drives the blueprint rain transition. Its body would re-roll the strength
    // just written, so the echo-suppress flag short-circuits the body for this call only; the
    // transition's audio cue is lost on the receiver, while the downstream chain still drives the
    // visible state from the wire-received scalars.
    if (newRain != curRain && g_causeRainFn) {
        g_causeRainEchoSuppress.store(true, std::memory_order_release);
        ue_wrap::ParamFrame f(g_causeRainFn);
        f.Set<bool>(L"isRaining", newRain);
        ue_wrap::Call(cycle, f);
        g_causeRainEchoSuppress.store(false, std::memory_order_release);
    }

    // Step D: setRainParticles, the template swap.
    if (rainStateChanged && g_setRainParticlesFn) {
        ue_wrap::ParamFrame f(g_setRainParticlesFn);
        ue_wrap::Call(cycle, f);
    }

    // Step E: setWindParameters propagates the wind speed to the wind actor.
    if (rainStateChanged && g_setWindParametersFn) {
        ue_wrap::ParamFrame f(g_setWindParametersFn);
        ue_wrap::Call(cycle, f);
    }

    // Snow goes through the trigger UFunction: many blueprint listeners need the dispatch fan-out,
    // which a direct field write would miss.
    const bool newSnow = (newFlags & kIsSnow) != 0;
    const bool curSnow = (curFlags & kIsSnow) != 0;
    if (newSnow != curSnow && g_intComsTriggerSnowFn) {
        ue_wrap::ParamFrame f(g_intComsTriggerSnowFn);
        f.Set<bool>(L"isSnow", newSnow);
        ue_wrap::Call(cycle, f);
    }

    outcome.rainTx = (newRain != curRain);
    outcome.snowTx = (newSnow != curSnow);
    outcome.scalarsChanged = rainStateChanged;
}

bool DebugForceRain(bool isRaining, float rainStrength) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: DebugForceRain off-game-thread -- caller must wrap in GT::Post");
        return false;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("weather: DebugForceRain called on non-host or unconfigured session");
        return false;
    }
    if (!g_installed || !g_setRainPropertiesFn || !g_causeRainFn) {
        UE_LOGW("weather: DebugForceRain UFunctions not yet resolved -- skipping");
        return false;
    }
    void* cycle = ResolveCycle();
    if (!cycle || !R::IsLive(cycle)) {
        UE_LOGW("weather: DebugForceRain no live daynightCycle_C -- skipping");
        return false;
    }

    // The invocation sequence. The game itself starts rain with ONE call, causeRain, whose body
    // rolls all five scalars and ends by calling setRainProperties with them. This entrypoint
    // wants a SPECIFIC strength and no auto-stop, so it drives the pieces explicitly -- and the
    // order matters, because causeRain's own tail call would otherwise overwrite them.

    // Step 1, the precondition: the rain-enable flag the cycle's scheduler chain reads. The
    // blueprint never writes it, so the forced state has to.
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(cycle) +
                             P::off::AdaynightCycle_enable_rain) = isRaining;

    // Step 2: causeRain drives the blueprint transition, the particle activate, the audio cue and
    // the flag flip with its fan-out. It also rolls its own strength and a 0.2-0.3 deactivate
    // chance, which step 3 then replaces.
    {
        ue_wrap::ParamFrame f(g_causeRainFn);
        f.Set<bool>(L"isRaining", isRaining);
        ue_wrap::Call(cycle, f);
    }

    // Step 3: pin the scalars the test needs -- the caller's strength, and a zero deactivate
    // chance so the scheduler's auto-stop roll cannot end the rain mid-test. AFTER causeRain, not
    // before: setRainProperties writes the four scalars only when isRaining is true, so on an OFF
    // call this leaves them alone and only restates the flag.
    {
        ue_wrap::ParamFrame f(g_setRainPropertiesFn);
        f.Set<bool>(L"isRaining",            isRaining);
        f.Set<float>(L"rainStrength",        rainStrength);
        f.Set<float>(L"rainLightningChance", 0.f);
        f.Set<float>(L"rainDeactivateChance",0.f);
        f.Set<float>(L"rainWindSpeed",       0.f);
        ue_wrap::Call(cycle, f);
    }

    // Step 4: propagate to the wind actor; a zero wind speed means no extra wind, and the call
    // still puts the actor in a known state.
    if (g_setWindParametersFn) {
        ue_wrap::ParamFrame f(g_setWindParametersFn);
        ue_wrap::Call(cycle, f);
    }

    UE_LOGI("weather: DebugForceRain isRaining=%d strength=%.2f -- "
            "wrote enable_rain + causeRain + setRainProperties + setWindParameters; "
            "POST observers on the mutators broadcast WeatherState",
            isRaining ? 1 : 0, rainStrength);
    return true;
}

bool DebugForceSnow(bool isSnow) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: DebugForceSnow off-game-thread -- caller must wrap in GT::Post");
        return false;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("weather: DebugForceSnow called on non-host or unconfigured session");
        return false;
    }
    if (!g_installed || !g_intComsTriggerSnowFn) {
        UE_LOGW("weather: DebugForceSnow intComs_triggerSnow not yet resolved -- skipping");
        return false;
    }
    void* cycle = ResolveCycle();
    if (!cycle || !R::IsLive(cycle)) {
        UE_LOGW("weather: DebugForceSnow no live daynightCycle_C -- skipping");
        return false;
    }
    ue_wrap::ParamFrame f(g_intComsTriggerSnowFn);
    f.Set<bool>(L"isSnow", isSnow);
    ue_wrap::Call(cycle, f);
    UE_LOGI("weather: DebugForceSnow isSnow=%d -- intComs_triggerSnow dispatched; "
            "POST observer broadcasts WeatherState to client", isSnow ? 1 : 0);
    return true;
}

bool ReadLocalIsRaining(bool* outFound) {
    if (outFound) *outFound = false;
    if (!GT::IsGameThread()) return false;
    void* cycle = ResolveCycle();
    if (!cycle || !R::IsLive(cycle)) return false;
    if (outFound) *outFound = true;
    return *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(cycle) + P::off::AdaynightCycle_isRaining);
}

void* Cycle() {
    void* cycle = ResolveCycle();
    return (cycle && R::IsLive(cycle)) ? cycle : nullptr;
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_causeRainEchoSuppress.store(false, std::memory_order_release);
    g_cycleCache = nullptr;
    g_cycleIdx = -1;
}

}  // namespace coop::weather_rain

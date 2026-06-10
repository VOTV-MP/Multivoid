// coop/weather_fog.cpp -- Phase 5W host-authoritative FOG sync. See weather_fog.h.

#include "coop/weather_fog.h"

#include "coop/net/protocol.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::weather_fog {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

// Resolved-once. spawnFog spawns AweatherFogController_C into fogEventObject and
// drives the height-fog density. SetFogDensity pushes finalFogDensity into the
// cycle's ExponentialHeightFog component (the visible height fog) -- used by the
// v24 late-joiner snap to make a written density take effect on the apply frame.
void* g_spawnFogFn      = nullptr;
void* g_setFogDensityFn = nullptr;  // v24: cycle's SetFogDensity() (push finalFogDensity -> ExpHeightFog)
bool  g_installed  = false;
bool  g_clientInterceptorReg = false;

// CLIENT echo-suppress for spawnFog: while false the client's spawnFog PRE
// interceptor CANCELS the BP body (the client never independently makes fog);
// ApplyFromHost sets it true around its OWN mirror-spawn so that one call passes.
// Same shape as weather_sync's causeRain echo-suppress. Atomic for the
// interceptor read in ProcessEventDetour vs the Apply setter (both game thread
// today, but kept atomic to match the sibling pattern).
std::atomic<bool> g_spawnFogEchoSuppress{false};

// Current role: true on a CLIENT. The spawnFog interceptor is RUNTIME-gated on
// this (not registration-gated) so a process that reconnects with a different
// role can't keep suppressing -- e.g. a client->host reconnect goes inert on the
// host. Install() updates it every call (before the g_installed early-out).
std::atomic<bool> g_isClient{false};

// HOST detector cache + throttle. Game-thread only, but the throttle clock is a
// steady_clock read (game_thread.cpp uses the same). 0xFF = "never sampled".
uint8_t   g_lastHostFogBits = 0xFF;
long long g_lastDetectMs    = 0;

// CLIENT reconcile cache (set by ApplyFromHost, read by TickClientReconcile).
// ApplyFromHost's clear only runs when the host BROADCASTS; a clear + STATIC host
// never re-broadcasts, so a rolling-fog actor that leaked during the pre-suppression
// connect window (before the spawnFog interceptor latched) rides out its Duration
// while the host is clear -- the user's persistent "balls of fog around the client".
// The heartbeat continuously re-asserts host-clear (MTA CBlendedWeather::DoPulse),
// independent of broadcasts. Cheap: a slot pointer read, no GUObjectArray walk.
bool      g_haveHostFog        = false;  // received >=1 host fog state
bool      g_lastHostFogActive  = false;  // host's last-known rolling-fog presence (kFogActive)
long long g_lastReconcileMs    = 0;

// Super-fog presence cache (perf 2026-06-04). The 3 Hz fog-edge detector used to
// call CountObjectsByClass(SuperFogClass) -- a FULL ~1M-entry GUObjectArray walk
// with a wstring alloc PER entry -- which measured ~90-150 ms/s on the host (the
// single biggest mod cost; perf_probe weatherConn bucket). Super-fog is a rare,
// minutes-long UFO encounter, so: cache the actor + revalidate by index (O(1),
// catches the DESPAWN edge every detection) and only WALK to find a freshly-spawned
// one on a coarse throttle (5 s edge latency is fine for a multi-minute event, and
// the apply path is clear-only anyway). Drops the walk from 3 Hz to <=0.2 Hz.
void*     g_superFogActor   = nullptr;
int32_t   g_superFogIdx     = -1;
long long g_lastSuperScanMs = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// True iff a live AsuperFog_C exists in the world. O(1) when one is cached (index
// revalidate); a single FindObjectByClass walk at most once per 5 s when none is
// cached. Replaces the per-detection full GUObjectArray Count walk.
bool HostHasSuperFog() {
    if (g_superFogActor && R::IsLiveByIndex(g_superFogActor, g_superFogIdx)) return true;
    g_superFogActor = nullptr;
    g_superFogIdx   = -1;
    const long long now = NowMs();
    if (now - g_lastSuperScanMs < 5000) return false;  // coarse rescan (super-fog is minutes-long)
    g_lastSuperScanMs = now;
    void* sf = R::FindObjectByClass(P::name::SuperFogClass);
    if (sf && R::IsLive(sf)) { g_superFogActor = sf; g_superFogIdx = R::InternalIndexOf(sf); return true; }
    return false;
}

// Compute the host's active-fog bits from the live cycle. All reads are cheap now:
// fogEventObject is a pointer read, super-fog goes through the cached HostHasSuperFog
// (no per-call GUObjectArray walk), permanentFog is a bool read.
uint8_t ComputeHostFogBits(void* cycle) {
    using namespace coop::net::fog_flags2;
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    uint8_t f2 = 0;
    if (*reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject) != nullptr)
        f2 |= kFogActive;
    if (HostHasSuperFog())
        f2 |= kSuperFogActive;
    if (*reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog))
        f2 |= kPermanentFog;
    return f2;
}

// spawnFog PRE interceptor. Returns true to CANCEL the BP body. Cancels organic
// spawnFog ONLY on a CLIENT (g_isClient -- so a client->host reconnect can't
// suppress the host's own fog) and only when ApplyFromHost is NOT mid mirror-spawn
// (echo flag). On the host it is always a pass-through.
bool OnSpawnFogPreSuppress(void* /*self*/, void* /*params*/) {
    return g_isClient.load(std::memory_order_acquire) &&
           !g_spawnFogEchoSuppress.load(std::memory_order_acquire);
}

}  // namespace

bool Install(bool isHost) {
    // Update the role EVERY call (before the latch early-out): a process can
    // reconnect with a different role, and the interceptor is runtime-gated on
    // g_isClient so it goes inert on the host without re-registration.
    g_isClient.store(!isHost, std::memory_order_release);
    if (g_installed) return true;

    void* cls = R::FindClass(P::name::DaynightCycleClass);
    if (!cls) return false;  // cycle class not loaded yet -- retry next tick.
    if (!g_spawnFogFn) g_spawnFogFn = R::FindFunction(cls, P::name::DaynightCycle_spawnFogFn);
    if (!g_spawnFogFn) return false;
    // v24: SetFogDensity for the late-joiner density snap (best-effort -- if it
    // doesn't resolve, the snapped finalFogDensity still takes effect on the next
    // cycle ReceiveTick, just one frame later).
    if (!g_setFogDensityFn) g_setFogDensityFn = R::FindFunction(cls, P::name::DaynightCycle_setFogDensityFn);

    // Register the spawnFog PRE interceptor ONCE (for both roles -- it self-gates
    // on g_isClient, a pass-through on the host). Do NOT latch g_installed until
    // it succeeds: a silently-dropped registration would leave a client able to
    // make uncommanded fog (the exact bug this fixes), so return false to retry.
    if (!g_clientInterceptorReg) {
        if (!GT::RegisterInterceptor(g_spawnFogFn, &OnSpawnFogPreSuppress)) {
            UE_LOGW("weather_fog: spawnFog interceptor registration FAILED (table full?) "
                    "-- NOT latching; retry next tick");
            return false;
        }
        g_clientInterceptorReg = true;
        UE_LOGI("weather_fog: spawnFog PRE interceptor registered (@%p; role-gated on "
                "g_isClient + echo-suppressed -- a client never makes uncommanded fog)",
                g_spawnFogFn);
    }

    g_installed = true;
    return true;
}

void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out) {
    if (!cycle || !R::IsLive(cycle)) return;
    out.flags2 = ComputeHostFogBits(cycle);
    // v24 late-joiner snap: stamp the host's CURRENT fog level so a joiner can match
    // it instantly instead of ramping from 0. finalFogDensity is the visible (eased)
    // height-fog density; the rolling-fog actor's Alpha is the ramp driver + Strength
    // its per-spawn scale (thickFog = Alpha*Strength). fogAlpha/fogStrength stay 0
    // when there's no rolling-fog actor (the receiver then skips the actor snap).
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    out.finalFogDensity = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity);
    void* actor = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    if (actor && R::IsLive(actor)) {
        auto* ab = reinterpret_cast<uint8_t*>(actor);
        out.fogAlpha    = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha);
        out.fogStrength = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength);
    }
}

void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload) {
    if (!GT::IsGameThread()) return;
    if (!cycle || !R::IsLive(cycle)) return;
    using namespace coop::net::weather_flags;  // kEnableFog, kEnableSuperfog
    using namespace coop::net::fog_flags2;      // kFogActive, kSuperFogActive, kPermanentFog
    auto* b = reinterpret_cast<uint8_t*>(cycle);

    const bool hostFogActive   = (payload.flags2 & kFogActive)   != 0;
    const bool hostSuperActive = (payload.flags2 & kSuperFogActive) != 0;

    // Cache for the client reconcile heartbeat (TickClientReconcile): a clear +
    // static host won't re-broadcast, so the heartbeat re-asserts host-clear from
    // this snapshot to clear a fog actor that leaked the pre-suppression window.
    g_haveHostFog       = true;
    g_lastHostFogActive = hostFogActive;

    // ---- rolling fog: assert the host's actor presence (MTA DoPulse, no diff-skip).
    void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    const bool clientHasRolling = (*slot != nullptr) && R::IsLive(*slot);
    if (!hostFogActive && clientHasRolling) {
        // Host clear -> destroy the client's stray/mirror rolling fog. The cycle's
        // own ReceiveTick then eases finalFogDensity back to the shared ToD ambient
        // (the destroyed actor was the only thing pushing the density target up).
        E::DestroyActor(*slot);
        *slot = nullptr;
        UE_LOGI("weather_fog: host CLEAR -> destroyed client rolling-fog actor");
    } else if (hostFogActive) {
        // Host fog -> ensure the client's OWN rolling-fog actor exists (echo-
        // suppressed past the client interceptor), then SNAP it to the host's
        // CURRENT fog level. Without the snap a fresh mirror actor ramps its density
        // from 0 over its Duration (minutes) -- the late-joiner "warm-up" the user
        // reported. fogprobe-confirmed (2026-06-02): writing the actor's Alpha is
        // accepted (a plain accumulator, NOT Timeline-locked) and the actor keeps
        // ramping from the written value, so the mirror tracks the host in lockstep.
        if (!clientHasRolling && g_spawnFogFn) {
            g_spawnFogEchoSuppress.store(true, std::memory_order_release);
            ue_wrap::ParamFrame f(g_spawnFogFn);
            ue_wrap::Call(cycle, f);
            g_spawnFogEchoSuppress.store(false, std::memory_order_release);
            UE_LOGI("weather_fog: host FOG -> mirror-spawned client rolling-fog actor");
        }
        // Copy the host actor's ramp state (Alpha = intensity, Strength = per-spawn
        // density scale; thickFog = Alpha*Strength is then recomputed by the actor's
        // own tick). Re-read the slot -- spawnFog above just populated it. Guard on
        // non-zero so a host with no actor (race) doesn't zero the client's.
        void* actor = *slot;
        if (actor && R::IsLive(actor) &&
            (payload.fogAlpha != 0.f || payload.fogStrength != 0.f)) {
            auto* ab = reinterpret_cast<uint8_t*>(actor);
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha)    = payload.fogAlpha;
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength) = payload.fogStrength;
        }
        // (The visible height-fog density snap is UNCONDITIONAL below -- a host-clear
        // apply also pins the client's base haze, not just the fog case.)
        UE_LOGI("weather_fog: host FOG snap -> Alpha=%.4f Strength=%.4f finalFogDensity=%.4f",
                payload.fogAlpha, payload.fogStrength, payload.finalFogDensity);
    }

    // ---- super fog: clear-only in v1 (destroy stray when host has none). Spawn
    // deferred -- AsuperFog_C owns a UFO encounter + its spawn was not reliably
    // reproducible in the probe. (Bounded loop: FindObjectByClass returns one.)
    if (!hostSuperActive) {
        for (int k = 0; k < 8; ++k) {
            void* sf = R::FindObjectByClass(P::name::SuperFogClass);
            if (!sf || !R::IsLive(sf)) break;
            E::DestroyActor(sf);
            UE_LOGI("weather_fog: host CLEAR -> destroyed stray client super-fog actor");
        }
    }

    // ---- base ambient height-fog density: snap to the host's value UNCONDITIONALLY
    // (also when the host is clear), then push it into the ExponentialHeightFog. The
    // cycle's clear-weather fog floor is ~0.03; a freshly-joined client reads 0.00
    // until its own ReceiveTick catches up (host=0.03 vs client=0.00 in the logs), so
    // without this the client stays a touch clearer than the host even with no fog
    // actor. In the fog-actor case this is the same value the mirror's ramp targets.
    *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity) = payload.finalFogDensity;
    if (g_setFogDensityFn) {
        ue_wrap::ParamFrame f(g_setFogDensityFn);
        ue_wrap::Call(cycle, f);
    }

    // ---- config bits: canonical static-config writes (no BP listeners need a
    // UFunction fan-out for these), mirroring the host exactly.
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog)      = (payload.flags  & kEnableFog)      != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog) = (payload.flags  & kEnableSuperfog) != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog)    = (payload.flags2 & kPermanentFog)   != 0;
}

void TickClientReconcile(void* cycle) {
    // CLIENT-ONLY backstop: continuously enforce host-clear so a rolling-fog actor
    // that leaked the pre-suppression connect window (and that a clear + static host
    // never re-broadcasts to clear) doesn't persist as "balls of fog around the
    // client". Runs only when (a) we're the client, (b) we've heard >=1 host fog
    // state, and (c) that state was CLEAR -- when the host HAS fog the legitimate
    // mirror actor must survive. Cheap: a slot pointer read, no GUObjectArray walk.
    // Throttled ~3 s (fog is slow; a few seconds of clear latency is invisible).
    if (!GT::IsGameThread()) return;
    if (!g_isClient.load(std::memory_order_acquire)) return;
    if (!g_haveHostFog || g_lastHostFogActive) return;  // act only when host known-CLEAR
    if (!cycle || !R::IsLive(cycle)) return;
    const long long now = NowMs();
    if (now - g_lastReconcileMs < 3000) return;
    g_lastReconcileMs = now;

    auto* b = reinterpret_cast<uint8_t*>(cycle);
    void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    if (*slot && R::IsLive(*slot)) {
        E::DestroyActor(*slot);
        *slot = nullptr;
        UE_LOGI("weather_fog: client reconcile -> destroyed stray rolling-fog actor (host is clear)");
    }
}

bool HostFogStateChanged(void* cycle) {
    if (!cycle || !R::IsLive(cycle)) return false;
    // Throttle ~3 Hz: the super-fog check walks GUObjectArray; never per-frame.
    const long long now = NowMs();
    if (now - g_lastDetectMs < 300) return false;
    g_lastDetectMs = now;

    const uint8_t bits = ComputeHostFogBits(cycle);
    if (bits == g_lastHostFogBits) return false;
    g_lastHostFogBits = bits;
    return true;
}

void OnDisconnect() {
    g_spawnFogEchoSuppress.store(false, std::memory_order_release);
    g_lastHostFogBits = 0xFF;
    g_lastDetectMs    = 0;
    g_superFogActor   = nullptr;  // session-scoped; a reconnect re-scans
    g_superFogIdx     = -1;
    g_lastSuperScanMs = 0;
    g_haveHostFog       = false;  // the next session re-learns the host's fog state
    g_lastHostFogActive = false;
    g_lastReconcileMs   = 0;
    // g_installed + g_clientInterceptorReg STAY set across sessions: g_spawnFogFn
    // is stable (same UClass) and the interceptor self-gates at runtime on
    // g_isClient -- which Install() refreshes every call -- so a reconnect with a
    // different role is handled WITHOUT re-resolving / re-registering (no double-
    // register). The interceptor is also conditional on g_spawnFogEchoSuppress
    // (false outside Apply). Mirrors weather_sync's kept causeRain interceptor.
}

}  // namespace coop::weather_fog

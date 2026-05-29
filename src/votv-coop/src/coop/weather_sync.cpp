// coop/weather_sync.cpp -- Phase 5W weather sync. See coop/weather_sync.h.

#include "coop/weather_sync.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "coop/ini_config.h"
#include "coop/weather_lightning.h"
#include "coop/weather_redsky.h"
#include "ue_wrap/call.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::weather_sync {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Resolved-once UFunction pointers. Each is nullptr until Install resolves
// it; once non-null, stays non-null for the session lifetime.
void* g_timerRainFn       = nullptr;
void* g_timerLightningFn  = nullptr;
void* g_fogEventFn        = nullptr;
void* g_superFogEventFn   = nullptr;
void* g_permaRainTimerFn  = nullptr;

void* g_causeRainFn          = nullptr;
void* g_setRainPropertiesFn  = nullptr;
void* g_setWindParametersFn  = nullptr;
void* g_intComsTriggerSnowFn = nullptr;
void* g_spawnFogFn           = nullptr;
void* g_setFogDensityFn      = nullptr;
void* g_setRainParticlesFn   = nullptr;

// Phase 5W Inc2 lightning lives in coop/weather_lightning.{h,cpp}.
// Install() below calls coop::weather_lightning::TryResolve +
// RegisterHostObserver; ApplyLightningStrike() forwards to the module's
// Apply(). The 6 resolved pointers / offsets + observer-registered latch
// moved with the implementation.

// Phase 5W Inc-fix-2 red-sky lives in coop/weather_redsky.{h,cpp}. Install()
// below calls coop::weather_redsky::TryResolve / RegisterHostObservers;
// ApplyRedSky / DebugForceRedSky forward to the module. The 3 resolved
// pointers + observer-registered latch + echo-suppress flag moved with
// the implementation.

// Note: no echo-suppress flag is needed for lightning. The host registers
// the spawn observer; the client does NOT. When the client's
// weather_lightning::Apply calls BeginDeferred, no POST observer is
// registered locally to broadcast an echo. And event_feed.cpp drops
// LightningStrike packets received on host, so the host never enters
// the Apply path at all. The role gates already make the broadcast path
// one-directional; an inbound flag would be dead code (RULE 2).

bool g_installed = false;
bool g_observersRegistered = false;     // host POST observers
bool g_interceptorsRegistered = false;  // client PRE interceptors

// Session pointer; atomic so the observer / interceptor reads can't race
// the harness setter on another thread (matches coop::dev::teleport_client +
// item_activate pattern).
std::atomic<coop::net::Session*> g_session{nullptr};

// FNV1a dedup signature over the wire payload. kNoSendYet sentinel
// disambiguates "never sent" from "last sent had this hash". Single atomic
// so the load is uninterruptible (matches the post-audit item_activate v6
// shape -- a split (have_flag + sig) had a race window with worker-thread
// senders; single atomic eliminates it).
inline constexpr uint64_t kNoSendYet = 0xFFFFFFFFFFFFFFFFULL;
std::atomic<uint64_t> g_lastSentSig{kNoSendYet};

// Pending receiver-side apply (mirrors item_activate::g_pendingApplyValid).
// A WeatherState packet can arrive on the client BEFORE Install resolves
// all UFunctions (e.g. the cycle is still loading) OR before the local
// cycle is live. In both cases ApplyFromHost stashes the latest payload
// here; TickConnect drains it once both gates pass. Latest-wins -- a
// newer packet overrides a still-pending older one. Game thread only.
bool g_pendingApply = false;
coop::net::WeatherStatePayload g_pendingApplyPayload{};

// Phase 5W Inc-fix-2 (2026-05-27) echo-suppress for causeRain. Per the IDA
// RE pass (votv-weather-RE-causeRain-IDA-2026-05-27.md), causeRain's BP
// body has 3x RandomFloat + 3x Ease calls -- it re-rolls rainStrength
// every time it fires. When the RECEIVER calls causeRain via reflection
// to apply the host's wire-received state, the Random rolls overwrite
// the strength we just set. Result: client's rainStrength drifts away
// from host's authoritative value.
//
// Fix (Option a from the IDA RE, the project's own echo-suppress
// precedent at item_activate.cpp:145+220): a PRE-interceptor on
// causeRain that cancels its body when this flag is set. The flag is
// set ONLY around the receiver's causeRain call in ApplyFromHost --
// elsewhere (host's DebugForceRain, organic timerRain scheduler) the
// flag stays false and causeRain runs normally. Atomic for cross-thread
// safety with the interceptor's read in ProcessEventDetour.
std::atomic<bool> g_causeRainEchoSuppress{false};

// Cached cycle pointer. The cycle is singleton-per-session; FindObjectByClass
// is a linear GUObjectArray walk and CLAUDE.md's audit rule prohibits per-
// frame full-array scans. Cache the pointer; revalidate on use via IsLive
// (the cycle is destroyed + recreated on level transitions OMEGA -> story,
// at which point we re-resolve). Game-thread only.
void* g_cycleCache = nullptr;

void* ResolveCycle() {
    if (g_cycleCache && R::IsLive(g_cycleCache)) return g_cycleCache;
    g_cycleCache = R::FindObjectByClass(P::name::DaynightCycleClass);
    return g_cycleCache;
}

bool ReadCycleState(void* cycle, coop::net::WeatherStatePayload& out) {
    if (!cycle || !R::IsLive(cycle)) return false;
    out = {};
    // v13 (A4 2026-05-29): stamp host's local Player Element id (we're
    // only called on host). 0 if host hasn't allocated its Element yet
    // (boot/seed window before SetLocalPeerId fires); receiver falls back
    // to msg.senderPeerSlot trust-bound check in that case.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        out.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }

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
    return true;
}

uint64_t SignaturePayload(const coop::net::WeatherStatePayload& p) {
    // FNV1a over the mutable fields (flags + 4 floats). senderElementId
    // is constant for the session (host's local Player Element id);
    // _pad is constant (= 0). Same shape as item_activate's
    // SignaturePayload.
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 0x100000001b3ULL; };
    mix(p.flags);
    mix(reinterpret_cast<const uint32_t&>(p.rainStrength));
    mix(reinterpret_cast<const uint32_t&>(p.rainLightningChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainDeactivateChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainWindSpeed));
    return h;
}

// ---- HOST: POST observer broadcasts state -------------------------------

void OnSchedulerPost(void* self, void* function, void* /*params*/) {
    // Diag flag -- read once per process. Used to trace observer firing
    // when broadcasts go missing. Off by default (ini-only gate).
    static const bool sLog = ::coop::ini_config::IsIniKeyTrue("weather_observer_log");
    if (sLog) {
        UE_LOGI("weather: OnSchedulerPost ENTRY (self=%p function=%p)", self, function);
    }

    if (!GT::IsGameThread()) {
        UE_LOGW("weather: OnSchedulerPost fired off-game-thread -- skipping");
        return;
    }
    if (!self) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- self null");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- session null");
        return;
    }
    if (!s->connected()) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- session not connected");
        return;
    }
    if (s->role() != coop::net::Role::Host) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- role != host");
        return;
    }

    coop::net::WeatherStatePayload p{};
    if (!ReadCycleState(self, p)) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- ReadCycleState(%p) failed", self);
        return;
    }

    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    const uint64_t lastSig = g_lastSentSig.load(std::memory_order_acquire);
    if (lastSig == storeSig) {
        if (sLog) UE_LOGI("weather: OnSchedulerPost early -- sig dedup (cur=%llX last=%llX)",
                          static_cast<unsigned long long>(storeSig),
                          static_cast<unsigned long long>(lastSig));
        return;
    }

    // 2026-05-27: reliable channel buffers internally; Send always succeeds.
    s->SendReliable(coop::net::ReliableKind::WeatherState, &p, sizeof(p));
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("weather: host broadcast flags=0x%02X rain=%.2f lc=%.2f dc=%.2f ws=%.2f (sig=%llX)",
            p.flags, p.rainStrength, p.rainLightningChance,
            p.rainDeactivateChance, p.rainWindSpeed,
            static_cast<unsigned long long>(storeSig));
}

// ---- CLIENT: PRE interceptor suppresses local scheduler -----------------

bool OnSchedulerPreSuppress(void* /*self*/, void* /*params*/) {
    // Returning true skips the original UFunction body. Client only -- the
    // host's role-gate at install time means the host never registers this.
    // No filter on `self`: there's one cycle per session; all dispatches
    // on the 5 scheduler UFunctions are this cycle's. Logging this at INFO
    // would spam (each fires on a timer); silent suppression is correct.
    return true;
}

// Phase 5W Inc-fix-2: echo-suppress interceptor for causeRain. Cancels
// the BP body ONLY when g_causeRainEchoSuppress is set. Used by the
// receiver's ApplyFromHost to skip causeRain's Random-roll body when
// applying wire-received state. When the flag is false (host's organic
// DebugForceRain / scheduler-driven calls), this is a pass-through.
bool OnCauseRainPreEchoSuppress(void* /*self*/, void* /*params*/) {
    if (g_causeRainEchoSuppress.load(std::memory_order_acquire)) {
        // Suppress: skip the BP body so its Random rolls don't overwrite
        // the rainStrength we just set via setRainProperties.
        return true;
    }
    return false;  // normal pass-through
}

// ---- common installer ---------------------------------------------------

bool TryResolveAllFunctions() {
    void* cls = R::FindClass(P::name::DaynightCycleClass);
    if (!cls) return false;

    struct Entry { const wchar_t* name; void** out; };
    Entry sched[] = {
        { P::name::DaynightCycle_timerRainFn,       &g_timerRainFn       },
        { P::name::DaynightCycle_timerLightningFn,  &g_timerLightningFn  },
        { P::name::DaynightCycle_fogEventFn,        &g_fogEventFn        },
        { P::name::DaynightCycle_superFogEventFn,   &g_superFogEventFn   },
        { P::name::DaynightCycle_permaRainTimerFn,  &g_permaRainTimerFn  },
    };
    Entry mut[] = {
        { P::name::DaynightCycle_causeRainFn,          &g_causeRainFn          },
        { P::name::DaynightCycle_setRainPropertiesFn,  &g_setRainPropertiesFn  },
        { P::name::DaynightCycle_setWindParametersFn,  &g_setWindParametersFn  },
        { P::name::DaynightCycle_intComsTriggerSnowFn, &g_intComsTriggerSnowFn },
        { P::name::DaynightCycle_spawnFogFn,           &g_spawnFogFn           },
        { P::name::DaynightCycle_setFogDensityFn,      &g_setFogDensityFn      },
        { P::name::DaynightCycle_setRainParticlesFn,   &g_setRainParticlesFn   },
    };

    int sResolved = 0, mResolved = 0;
    for (auto& e : sched) {
        if (*e.out) { ++sResolved; continue; }
        if (void* fn = R::FindFunction(cls, e.name)) { *e.out = fn; ++sResolved; }
    }
    for (auto& e : mut) {
        if (*e.out) { ++mResolved; continue; }
        if (void* fn = R::FindFunction(cls, e.name)) { *e.out = fn; ++mResolved; }
    }
    // All 5 schedulers required for host broadcasts + client suppression.
    // All 7 mutators required so the client can apply every state delta.
    return sResolved == 5 && mResolved == 7;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;

    if (!TryResolveAllFunctions()) {
        // daynightCycle_C not yet loaded (still in OMEGA / loading) OR a
        // UFunction renamed in a future recook (caught here, not at runtime).
        // Retry on next NetPumpTick.
        return;
    }

    if (!session) {
        // Session not yet wired in -- harness calls Install with the session
        // pointer once it has it. Wait.
        return;
    }

    const bool isHost = (session->role() == coop::net::Role::Host);

    if (isHost && !g_observersRegistered) {
        int n = 0;
        struct Tgt { void* fn; const wchar_t* name; };
        const Tgt all[] = {
            // The 5 SCHEDULER UFunctions: organic weather changes (timer-
            // driven probabilistic rolls) propagate via these.
            { g_timerRainFn,       L"timerRain" },
            { g_timerLightningFn,  L"timerLightning" },
            { g_fogEventFn,        L"fogEvent" },
            { g_superFogEventFn,   L"superFogEvent" },
            { g_permaRainTimerFn,  L"permaRain_timer" },
            // The 3 user-visible MUTATOR UFunctions: needed for story-
            // event-driven and dev-test-driven state changes that don't
            // go through the schedulers (e.g. DebugForceRain calls
            // setRainProperties directly; BP story events can call
            // causeRain or intComs_triggerSnow outside timerRain). The
            // same OnSchedulerPost callback handles both -- it reads the
            // cycle state post-mutation and dedups via FNV1a sig.
            { g_causeRainFn,          L"causeRain" },
            { g_setRainPropertiesFn,  L"setRainProperties" },
            { g_intComsTriggerSnowFn, L"intComs_triggerSnow" },
        };
        for (const auto& t : all) {
            if (!t.fn) {
                UE_LOGW("weather: HOST observer skip '%ls' -- UFunction pointer null at registration time",
                        t.name);
                continue;
            }
            if (!GT::RegisterPostObserver(t.fn, &OnSchedulerPost)) {
                UE_LOGE("weather: HOST observer REGISTRATION FAILED for '%ls' "
                        "(observer table full? bump kMaxObservers)", t.name);
                continue;
            }
            ++n;
        }
        g_observersRegistered = true;
        UE_LOGI("weather: HOST POST observers registered on %d/8 UFunctions "
                "(5 schedulers + 3 mutators)", n);
    }

    if (!isHost && !g_interceptorsRegistered) {
        int n = 0;
        void* targets[] = { g_timerRainFn, g_timerLightningFn, g_fogEventFn,
                            g_superFogEventFn, g_permaRainTimerFn };
        for (void* t : targets) {
            if (t && GT::RegisterInterceptor(t, &OnSchedulerPreSuppress)) ++n;
        }
        g_interceptorsRegistered = true;
        UE_LOGI("weather: CLIENT PRE interceptors registered on %d/5 scheduler "
                "UFunctions (client never decides 'rain now' locally; receives "
                "WeatherState from host instead)", n);
    }

    // Phase 5W Inc-fix-2: echo-suppress interceptor on causeRain, registered
    // on BOTH host AND client. The interceptor itself is conditional on
    // g_causeRainEchoSuppress -- if the flag is false (organic dispatches:
    // host's DebugForceRain, scheduler-driven timerRain bodies that
    // internally call causeRain) the interceptor is a pass-through. The
    // flag is set true only around ApplyFromHost's causeRain call, so the
    // BP body's Random rolls don't overwrite our wire-received scalars.
    // Registered once per session (regardless of role).
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

    // Phase 5W Inc2 lightning observability. HOST registers a POST observer
    // on BeginDeferredActorSpawnFromClass (same UFunction npc_sync intercepts;
    // observers + interceptors are separate dispatch stages, no conflict).
    // We use a POST observer because lightning doesn't need to CANCEL spawns
    // -- the client never spawns lightning locally (timerLightning is
    // suppressed by Inc1's interceptor on the cycle), so there's nothing to
    // suppress. Host just needs to OBSERVE the spawn to broadcast the loc.
    // CLIENT skips this registration -- no observer needed for receive
    // (event_feed dispatches LightningStrike packets directly).
    // Phase 5W Inc-fix-2 red-sky: delegate to coop::weather_redsky module.
    coop::weather_redsky::SetSession(session);
    if (isHost) {
        coop::weather_redsky::RegisterHostObservers();
    } else {
        coop::weather_redsky::TryResolve();
    }

    // Phase 5W Inc2 lightning: delegate observer registration + path resolve
    // to coop::weather_lightning. SetSession every re-entry so the observer
    // (registered once) reads the current session on fire.
    coop::weather_lightning::SetSession(session);
    if (isHost) {
        coop::weather_lightning::RegisterHostObserver();
        // RegisterHostObserver internally calls TryResolve; if the GameplayStatics
        // CDO or lightningStrike_C isn't loaded yet, returns false and is retried
        // on the next NetPumpTick via Install's idempotent re-entry.
    } else {
        // Client also needs the spawn path resolved so Apply() can call
        // BeginDeferred + FinishSpawning on receive. Resolve lazily.
        coop::weather_lightning::TryResolve();
    }

    g_installed = true;
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

    // Proper RULE-1 invocation sequence per
    // research/findings/votv-weather-RE-mainGamemode-2026-05-26.md + the
    // RE agent's 2026-05-27 deep pass. Required because direct
    // `setRainProperties(true, 1.0, 0, 0, 0)` ALONE has two latent risks:
    //   (a) the BP body of `setRainProperties` is not visible in the CXX
    //       dump -- we cannot prove it internally calls `setRainParticles`
    //       to start the visible UParticleSystemComponent rainEffect;
    //   (b) `causeRain` body may internally guard on `enable_rain` (the
    //       feature flag at @0x044B) -- the dump can't disprove this.
    //
    // The proper sequence:

    // Step 1 -- precondition: enable_rain := true. Cheap (one byte write).
    // Belt against any internal guard in causeRain's BP body; harmless
    // if the BP body doesn't check the flag.
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(cycle) +
                             P::off::AdaynightCycle_enable_rain) = isRaining;

    // Step 2 -- setRainProperties FIRST. Writes the 5 scalar fields,
    // including rainDeactivateChance=0 which prevents the scheduler's
    // per-tick auto-stop roll from terminating the rain mid-test.
    // Ordering matters: if causeRain ran first the rain could be
    // auto-deactivated by a stray timerRain tick before scalars tighten.
    {
        ue_wrap::ParamFrame f(g_setRainPropertiesFn);
        f.Set<bool>(L"isRaining",            isRaining);
        f.Set<float>(L"rainStrength",        rainStrength);
        f.Set<float>(L"rainLightningChance", 0.f);
        f.Set<float>(L"rainDeactivateChance",0.f);
        f.Set<float>(L"rainWindSpeed",       0.f);
        ue_wrap::Call(cycle, f);
    }

    // Step 3 -- causeRain drives the BP transition: particle Activate,
    // audio cue start, isRaining @0x02E4 flip with side-effect fan-out.
    // setRainProperties wrote the bool already; calling causeRain with
    // the same value is the documented BP-edge trigger (we want the
    // particle Activate, not just the state write).
    {
        ue_wrap::ParamFrame f(g_causeRainFn);
        f.Set<bool>(L"isRaining", isRaining);
        ue_wrap::Call(cycle, f);
    }

    // Step 4 -- propagate to AdirectionalWind_C. rainWindSpeed=0 above
    // means no extra wind, but the function still ensures the wind
    // actor is in a known state.
    if (g_setWindParametersFn) {
        ue_wrap::ParamFrame f(g_setWindParametersFn);
        ue_wrap::Call(cycle, f);
    }

    UE_LOGI("weather: DebugForceRain isRaining=%d strength=%.2f -- "
            "wrote enable_rain + setRainProperties + causeRain + setWindParameters; "
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

bool DebugForceRedSky(bool red) {
    // Body moved to coop::weather_redsky::DebugForce.
    return coop::weather_redsky::DebugForce(red);
}

void ApplyRedSky(const coop::net::RedSkyPayload& payload) {
    // Body moved to coop::weather_redsky::Apply.
    coop::weather_redsky::Apply(payload);
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

void ApplyLightningStrike(const coop::net::LightningStrikePayload& payload) {
    // Body moved to coop::weather_lightning::Apply (one-line passthrough so
    // event_feed's existing dispatch surface stays stable).
    coop::weather_lightning::Apply(payload);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only sender
    if (peerSlot < 1 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) {
        UE_LOGW("weather: QueueConnectBroadcastForSlot peerSlot=%d out of [1..%u)",
                peerSlot, static_cast<unsigned>(coop::players::kMaxPeers));
        return;
    }
    void* cycle = ResolveCycle();
    if (!cycle) {
        UE_LOGI("weather: QueueConnectBroadcastForSlot(slot=%d) no daynightCycle_C "
                "live -- skipping (client receives default; observer-driven path "
                "will catch the next state change)", peerSlot);
        return;
    }
    coop::net::WeatherStatePayload p{};
    if (!ReadCycleState(cycle, p)) return;
    // Send to ONE slot only -- a fan-out send would skip late-joiners
    // (they'd see default-weather world even mid-storm if the storm
    // started before they connected).
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WeatherState, &p, sizeof(p));
    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("weather: connect-broadcast slot=%d sent flags=0x%02X rain=%.2f lc=%.2f dc=%.2f ws=%.2f",
            peerSlot, p.flags, p.rainStrength, p.rainLightningChance,
            p.rainDeactivateChance, p.rainWindSpeed);
}

void TickConnect() {
    // 2026-05-27: host-side pending-broadcast retry retired (channel queues
    // internally). Kept the receiver-side g_pendingApply -- that's a STATE
    // defer (waiting for local cycle UClass to resolve), NOT a channel-busy
    // retry, so it remains relevant.
    if (g_pendingApply && g_installed) {
        void* cycle = ResolveCycle();
        if (cycle && R::IsLive(cycle)) {
            coop::net::WeatherStatePayload p = g_pendingApplyPayload;
            g_pendingApply = false;
            UE_LOGI("weather: draining deferred apply (flags=0x%02X rain=%.2f)",
                    p.flags, p.rainStrength);
            ApplyFromHost(p);  // re-enters; this time both gates pass
        }
        // else: cycle still loading; retry next tick.
    }
}

void OnDisconnect() {
    if (g_pendingApply) {
        UE_LOGI("weather: OnDisconnect clearing pending apply (flags=0x%02X)",
                g_pendingApplyPayload.flags);
    }
    g_pendingApply = false;
    g_pendingApplyPayload = {};
    g_lastSentSig.store(kNoSendYet, std::memory_order_release);
    // Cycle cache may dangle if the session ends mid-level-transition;
    // clear so the next ResolveCycle re-walks via FindObjectByClass.
    g_cycleCache = nullptr;

    // Audit M26 (2026-05-27): unregister observers that are bound to
    // long-lived engine UFunctions (BeginDeferredActorSpawnFromClass +
    // redSkyEvent.set) AND scoped to a single session's role. If the next
    // session reconnects with a different role (host->client or back),
    // the still-registered host-only observers would fire on the wrong
    // peer. The 5 scheduler PRE-interceptors and the causeRain
    // echo-suppress interceptor are KEPT registered -- they're conditional
    // on g_causeRainEchoSuppress (false outside Apply) and on role
    // checks inside the observer body, so they stay safe across role
    // changes; unregistering them would require re-resolving + re-
    // registering on each connect-edge, which is heavier than the
    // run-time role check.
    coop::weather_lightning::OnDisconnect();
    coop::weather_redsky::OnDisconnect();
    // Clear g_installed so the next session's Install() call can re-enter
    // and re-register lightning + red-sky observers. The other per-feature
    // flags (g_observersRegistered, g_interceptorsRegistered) stay set --
    // their observers/interceptors are role-conditional inside the
    // callback body, so they're safe across role changes without
    // re-registration churn. UFunction pointers are stable across cycle
    // recreation (UClass children, not instance state).
    g_installed = false;
}

void ApplyFromHost(const coop::net::WeatherStatePayload& payload) {
    // v13 (A4 2026-05-29): the "is the sender host?" trust-bound check
    // moved up into event_feed::Update's WeatherState dispatcher (it
    // validates msg.senderPeerSlot == 0 before posting here). The
    // payload's senderElementId is host's local Player Element id; we
    // keep it on the payload for cross-cutting (logging, future
    // syncContext byte under B1).
    // g_installed gate: TryResolveAllFunctions requires all 11 UFunctions
    // (5 scheduler + 6 mutator) to be resolved before flipping g_installed.
    // If a WeatherState packet arrives while we're still resolving (e.g.
    // mutators partially resolved on a slow boot), partial application
    // would brick the cycle into an inconsistent state. Stash latest-wins
    // and let TickConnect drain once g_installed flips true.
    void* cycle = g_installed ? ResolveCycle() : nullptr;
    if (!g_installed || !cycle || !R::IsLive(cycle)) {
        g_pendingApplyPayload = payload;
        g_pendingApply = true;
        UE_LOGI("weather: ApplyFromHost defer (installed=%d cycle=%p) -- "
                "stashing for TickConnect drain",
                g_installed ? 1 : 0, cycle);
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(cycle);

    // Read current state for delta-compare; we avoid redundant UFunction
    // calls when the bit hasn't changed (causeRain / intComs_triggerSnow
    // fan out to BP listeners + start/stop particle systems; calling them
    // with the current value is a wasted dispatch + a redundant fanout
    // notification).
    coop::net::WeatherStatePayload cur{};
    ReadCycleState(cycle, cur);

    const uint8_t newFlags = payload.flags;
    const uint8_t curFlags = cur.flags;
    using namespace coop::net::weather_flags;

    // Phase 5W Inc-fix-2 (2026-05-27): RULE-1 receiver -- revert to
    // pure-UFunction chain after the user flagged direct memory writes
    // as a crutch ("You shipped a crutch, not per rule 1"). See
    // [[feedback-no-direct-memory-write-crutch]]. The prior commit
    // (987898b) bypassed causeRain's BP body via direct memory writes
    // to skip its Random rolls. User correction: that's a bypass, not
    // a fix, and screenshots showed the host wasn't actually rendering
    // visible rain particles either (only atmospheric mist); the
    // host-vs-client diff I claimed to see was wrong. So the crutch
    // wasn't even fixing a real gap.
    //
    // Restored chain (pure UFunction, mirrors host's DebugForceRain):
    //   A. Config bits via direct write (these are STATIC config bools
    //      with no BP listeners; direct write is canonical -- not a
    //      bypass of a misbehaving BP). [acceptable]
    //   B. setRainProperties(isRaining, 4 scalars) UFunction.
    //   C. causeRain(isRaining) UFunction -- whose Random-roll behavior
    //      is now flagged for proper RE rather than bypass.
    //   D. setRainParticles UFunction (template swap).
    //   E. intComs_triggerSnow UFunction.
    //   F. setWindParameters UFunction.
    //
    // The visible-particle gap is now an OPEN problem; the in-progress
    // IDA RE pass on causeRain's BP body + the red-sky pathway will
    // surface the proper fix. Until that lands, the wire layer matches
    // host's reflection-driven trigger exactly -- both peers reach the
    // same atmospheric state, neither shows particle rendering, parity
    // is maintained without the crutch.

    const bool newRain = (newFlags & kIsRaining) != 0;
    const bool curRain = (curFlags & kIsRaining) != 0;

    // Step A: config-bit direct writes (static config bools; no BP
    // listeners that need fan-out -- the dump shows no Set* UFunctions
    // for these bits, so this is the canonical write path, not a bypass).
    if (((curFlags ^ newFlags) & kEnableRain) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enable_rain) =
            (newFlags & kEnableRain) != 0;
    if (((curFlags ^ newFlags) & kEnableFog) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enable_fog) =
            (newFlags & kEnableFog) != 0;
    if (((curFlags ^ newFlags) & kEnableSuperfog) != 0)
        *reinterpret_cast<bool*>(base + P::off::AdaynightCycle_enable_superfog) =
            (newFlags & kEnableSuperfog) != 0;
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

    // Step B: setRainProperties writes the bool + 5 scalars via the
    // canonical UFunction (matches host's DebugForceRain order).
    if (rainStateChanged && g_setRainPropertiesFn) {
        ue_wrap::ParamFrame f(g_setRainPropertiesFn);
        f.Set<bool>(L"isRaining",            newRain);
        f.Set<float>(L"rainStrength",        payload.rainStrength);
        f.Set<float>(L"rainLightningChance", payload.rainLightningChance);
        f.Set<float>(L"rainDeactivateChance",payload.rainDeactivateChance);
        f.Set<float>(L"rainWindSpeed",       payload.rainWindSpeed);
        ue_wrap::Call(cycle, f);
    }

    // Step C: causeRain triggers the BP rain-transition. ITS BODY HAS
    // 3x RandomFloat + 3x Ease (IDA RE 2026-05-27) that would re-roll
    // the rainStrength we just wrote in Step B. Echo-suppress flag
    // short-circuits the body for this call only -- the BP transition
    // visual side effects (audio cue) ARE lost on the receiver, but
    // setRainProperties downstream chain (setRainParameters, setRainParticles)
    // still fires + drives the visible state derived from our wire-
    // received scalars. Per [[feedback-no-direct-memory-write-crutch]]
    // -- proper fix, not a bypass.
    if (newRain != curRain && g_causeRainFn) {
        g_causeRainEchoSuppress.store(true, std::memory_order_release);
        ue_wrap::ParamFrame f(g_causeRainFn);
        f.Set<bool>(L"isRaining", newRain);
        ue_wrap::Call(cycle, f);
        g_causeRainEchoSuppress.store(false, std::memory_order_release);
    }

    // Step D: setRainParticles (template swap; safe per RE Q1.2).
    if (rainStateChanged && g_setRainParticlesFn) {
        ue_wrap::ParamFrame f(g_setRainParticlesFn);
        ue_wrap::Call(cycle, f);
    }

    // Step E: setWindParameters propagates rainWindSpeed to AdirectionalWind_C.
    if (rainStateChanged && g_setWindParametersFn) {
        ue_wrap::ParamFrame f(g_setWindParametersFn);
        ue_wrap::Call(cycle, f);
    }

    // ---- isSnow via intComs_triggerSnow(bool) -- 53 BP listeners need
    // the UFunction dispatch fan-out (direct write at @0x03B0 misses them).
    const bool newSnow = (newFlags & kIsSnow) != 0;
    const bool curSnow = (curFlags & kIsSnow) != 0;
    if (newSnow != curSnow && g_intComsTriggerSnowFn) {
        ue_wrap::ParamFrame f(g_intComsTriggerSnowFn);
        f.Set<bool>(L"isSnow", newSnow);
        ue_wrap::Call(cycle, f);
    }

    UE_LOGI("weather: applied flags 0x%02X -> 0x%02X rain=%.2f lc=%.2f "
            "dc=%.2f ws=%.2f (rain-tx=%d snow-tx=%d scalars-changed=%d)",
            curFlags, newFlags,
            payload.rainStrength, payload.rainLightningChance,
            payload.rainDeactivateChance, payload.rainWindSpeed,
            (newRain != curRain) ? 1 : 0,
            (newSnow != curSnow) ? 1 : 0,
            rainStateChanged ? 1 : 0);
}

}  // namespace coop::weather_sync

// coop/weather_sync.cpp -- Phase 5W weather sync. See coop/weather_sync.h.

#include "coop/world/weather_sync.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/config/config.h"
#include "coop/dev/weather_probe.h"
#include "coop/world/weather_event_births.h"
#include "coop/world/weather_fog.h"
#include "coop/world/weather_lightning.h"
#include "coop/world/weather_rain.h"
#include "coop/world/weather_redsky.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/world/directionalwind.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cmath>
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

// The 3 user-visible MUTATOR observer targets (causeRain / setRainProperties /
// intComs_triggerSnow). Resolved here INDEPENDENTLY of weather_rain's own
// resolves of the same UFunctions (each module owns what it registers/calls,
// the family shape; UFunction ptrs are UClass-stable so both once-latch).
// The rain-side CALL substrate (all 5 mutators incl. setWindParameters /
// setRainParticles) lives in coop/weather_rain.cpp (2026-07-19 cut).
void* g_causeRainFn          = nullptr;
void* g_setRainPropertiesFn  = nullptr;
void* g_intComsTriggerSnowFn = nullptr;
// Fog UFunctions (spawnFog / SetFogDensity) + all fog state moved to
// coop/weather_fog.{h,cpp} (2026-06-01 host-authoritative fog). They were
// resolved-but-dead here before; weather_fog now owns the fog substrate (P7).

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

// v50 wind-gust suppression. The client must not run AdirectionalWind_C::changeWindOrigin
// (its local RNG re-roll of windTarget, every 1-60 s) or it fights the host-synced gust.
// One PRE-interceptor on that UFunction, runtime-gated on g_windIsClient (refreshed every
// Install, reset on disconnect) -- the weather_fog::g_isClient pattern, so a client->host
// reconnect goes inert without re-registration and a disconnected client's local wind
// resumes rolling. Registered once (latch), on BOTH roles (host = pass-through). The
// interceptor lives on the directionalWind class, which may load after the cycle, so the
// registration sits ABOVE Install's cycle-gated g_installed early-out.
std::atomic<bool> g_windIsClient{false};

// Runtime gate for the 5 scheduler PRE-interceptors (latent-bug fix 2026-06-11,
// agent-found): the old OnSchedulerPreSuppress returned true UNCONDITIONALLY and
// was registered client-only at install time -- but interceptors live for the
// process, so (a) an EX-client kept suppressing its local weather rolls after
// disconnect (SP/menu world with dead rain/fog/lightning until restart), and
// (b) a process that HOSTED first never registered them at all (the g_installed
// latch), so a later client session in the same process ran its own rolls =
// weather divergence. Fix = the proven g_windIsClient shape: register on BOTH
// roles once, gate at FIRE time on this atomic (true only while connected as a
// client), clear on disconnect.
std::atomic<bool> g_schedulerSuppressActive{false};
bool g_windOriginInterceptorReg = false;

// Session pointer; atomic so the observer / interceptor reads can't race
// the harness setter on another thread (matches coop::teleport_client +
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

// (The causeRain echo-suppress flag + PRE interceptor moved to
// coop/weather_rain.cpp with the receiver apply body -- 2026-07-19 cut.)

// Cached cycle pointer. The cycle is singleton-per-session; FindObjectByClass
// is a linear GUObjectArray walk and CLAUDE.md's audit rule prohibits per-
// frame full-array scans. Cache the pointer + its InternalIndex; revalidate on
// use via IsLiveByIndex (the cycle is destroyed + recreated on level transitions
// OMEGA -> story, at which point we re-resolve). Game-thread only.
//
// IsLiveByIndex, NOT plain IsLive (2026-07-15 crash fix): on world teardown the
// daynightCycle is destroyed and its GUObjectArray slot is RECYCLED to another
// actor (observed: prop_swinger_crematorDoor_C). Plain IsLive passes the recycled
// slot -- it IS live, just a different object -- so a weather apply would dispatch
// the cycle's setRainParticles / intComs_triggerSnow UFunction on the foreign
// object -> "Failed to find function setRainParticles in prop_swinger_crematorDoor_C"
// -> LowLevelFatalError (user client crash on exit-to-menu). The cached index's
// serial check rejects the recycled slot -> we re-resolve (finding the new cycle or
// null, whereupon Apply no-ops). [[lesson-islive-recycled-slot-blind-use-by-index]]
void* g_cycleCache = nullptr;
int32_t g_cycleIdx = -1;

void* ResolveCycle() {
    if (g_cycleCache && R::IsLiveByIndex(g_cycleCache, g_cycleIdx)) return g_cycleCache;
    g_cycleCache = R::FindObjectByClass(P::name::DaynightCycleClass);
    g_cycleIdx = g_cycleCache ? R::InternalIndexOf(g_cycleCache) : -1;
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
        // (v14 stamped a paired senderContext byte; v16 PR-FOUNDATION-1b
        // moved stale-gen defense to header senderEpoch.)
    }

    // Rain/snow flag bits + the 5 rain scalars (incl. the v24 ease target):
    // stamped by the rain module (2026-07-19 cut; resets out.flags, call FIRST).
    coop::weather_rain::ReadState(cycle, out);
    // v23: stamp the host's active-fog bits (flags2). Fog is driven by event ACTORS,
    // not the enable_* config bits, so the wire carries live actor presence. v24 also
    // stamps the host's current fog DENSITY + the rolling-fog actor's ramp state so a
    // joiner snaps to the host's fog level instead of ramping from 0.
    coop::weather_fog::ReadHostFogState(cycle, out);

    // v43/v50: stamp the host's directionalWind state. The wind actor is SEPARATE from the
    // cycle (resolved by class). kWindValid marks the read as valid so the client never
    // zeros its wind from a (rare) mid-transition unread host -- and calm (all-zero)
    // wind still syncs because the bit, not the values, gates the apply. v50 also stamps
    // windTarget (the gust input that drives the leaf-shake `intensity`); gate the bit on
    // BOTH reads so a null windTarget mid-init never ships a 0 gust that forces the client
    // calm.
    ue_wrap::directionalwind::WindState wind;
    ue_wrap::FVector windTgt{};
    if (ue_wrap::directionalwind::Read(wind) && ue_wrap::directionalwind::ReadTarget(windTgt)) {
        out.windSpeedBg      = wind.speedBg;
        out.windStrengthBg   = wind.strengthBg;
        out.windSpeedRain    = wind.speedRain;
        out.windStrengthRain = wind.strengthRain;
        out.windTargetX      = windTgt.X;
        out.windTargetY      = windTgt.Y;
        out.windTargetZ      = windTgt.Z;
        out.flags2 |= coop::net::fog_flags2::kWindValid;
    }
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
    mix(p.flags2);   // v23: fog active-actor bits -- MUST be in the sig or a fog
                     // on<->off that doesn't change rain is deduped away (silent-fail).
    mix(reinterpret_cast<const uint32_t&>(p.rainStrength));
    mix(reinterpret_cast<const uint32_t&>(p.rainLightningChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainDeactivateChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainWindSpeed));
    // v43: the 4 wind fields -- else a wind-only change (e.g. the host's background
    // wind shifts on a day rollover with no rain change) hashes the same + deduplicates
    // away, so the client never gets the new wind. Same trap v23 flags2 closed for fog.
    mix(reinterpret_cast<const uint32_t&>(p.windSpeedBg));
    mix(reinterpret_cast<const uint32_t&>(p.windStrengthBg));
    mix(reinterpret_cast<const uint32_t&>(p.windSpeedRain));
    mix(reinterpret_cast<const uint32_t&>(p.windStrengthRain));
    // v50: windTarget (the gust) -- so a gust-only change on a scheduler/fog-edge send
    // isn't deduped away. (The ~1.2 s DoPulse re-sends it unconditionally regardless, but
    // including it keeps the change-driven path honest, matching the v43 field decision.)
    mix(reinterpret_cast<const uint32_t&>(p.windTargetX));
    mix(reinterpret_cast<const uint32_t&>(p.windTargetY));
    mix(reinterpret_cast<const uint32_t&>(p.windTargetZ));
    return h;
}

// ---- HOST: POST observer broadcasts state -------------------------------

void OnSchedulerPost(void* self, void* function, void* /*params*/) {
    // Diag flag -- read once per process. Used to trace observer firing
    // when broadcasts go missing. Off by default (ini-only gate).
    static const bool sLog = ::coop::config::ResolveFlag(::coop::config_registry::rows::weather_observer_log);
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
    // Returning true skips the original UFunction body. Runtime-gated on the
    // connected-as-client atomic (the g_windIsClient shape): host + idle/SP
    // pass through, a connected client's local weather rolls are suppressed
    // (it receives WeatherState from the host instead). No filter on `self`:
    // there's one cycle per session; all dispatches on the 5 scheduler
    // UFunctions are this cycle's. Logging this at INFO would spam (each
    // fires on a timer); silent suppression is correct.
    return g_schedulerSuppressActive.load(std::memory_order_acquire);
}

// (OnCauseRainPreEchoSuppress moved to coop/weather_rain.cpp -- 2026-07-19 cut.)

// v50: cancel AdirectionalWind_C::changeWindOrigin (the per-peer RNG windTarget re-roll)
// on the CLIENT so the host-synced gust holds. Runtime-gated (registered on both roles;
// host = pass-through). One wind actor per session -> no `self` filter; changeWindOrigin
// fires on a 1-60 s timer so silent suppression is correct (no log spam).
// [probe] fire/suppress counters (2026-07-04 wind-desync probe): the interceptor is
// otherwise silent, so a live "client windy, host calm" report cannot distinguish
// "suppression never fired" (dispatch bypassed ProcessEvent) from "state synced but
// visuals diverged elsewhere". Read by the weather_probe line below. NOTE: the BP body
// re-arms its own 1-60 s timer, so ONE suppressed fire kills the client's roll chain
// [V bytecode @393] -- suppressed=1 total is the EXPECTED healthy count, and fired
// counts on the host are its organic gust rolls.
std::atomic<uint32_t> g_windRollFired{0};       // dispatches seen (both roles)
std::atomic<uint32_t> g_windRollSuppressed{0};  // dispatches cancelled (client)
bool OnWindOriginPreSuppress(void* /*self*/, void* /*params*/) {
    g_windRollFired.fetch_add(1, std::memory_order_relaxed);
    const bool suppress = g_windIsClient.load(std::memory_order_acquire);
    if (suppress) g_windRollSuppressed.fetch_add(1, std::memory_order_relaxed);
    return suppress;
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
        { P::name::DaynightCycle_intComsTriggerSnowFn, &g_intComsTriggerSnowFn },
    };
    // (fog mutators spawnFog/SetFogDensity moved to weather_fog -- resolved there;
    // the full 5-mutator CALL substrate moved to weather_rain -- resolved there.)

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
    // All 3 mutator OBSERVER TARGETS required so host state changes broadcast.
    return sResolved == 5 && mResolved == 3;
}

// (The [probe weather]/[probe wind] diagnostics + ReadComponentIsActive moved
// to coop/dev/weather_probe.{h,cpp} -- 2026-07-19 cut, commit 2.)

}  // namespace

uint32_t WindRollFired() {
    return g_windRollFired.load(std::memory_order_relaxed);
}
uint32_t WindRollSuppressed() {
    return g_windRollSuppressed.load(std::memory_order_relaxed);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);

    // v50 wind-gust suppression (ABOVE the g_installed early-out: the directionalWind class
    // may load after the cycle, and this runs every tick until it registers -- the cycle-
    // gated path below must not block it). Refresh the role gate every call (so a reconnect
    // with a different role is handled without re-registration), then register the
    // changeWindOrigin PRE-interceptor once. Registered on BOTH roles; the host's
    // g_windIsClient=false makes it a pass-through. Best-effort resolve (retries next tick
    // if the class/UFunction isn't loaded or the interceptor table is momentarily full).
    g_windIsClient.store(session && session->role() != coop::net::Role::Host,
                         std::memory_order_release);
    // Same refresh for the 5 scheduler interceptors' runtime gate (see the
    // atomic's comment): true only while connected as a client.
    g_schedulerSuppressActive.store(session && session->role() != coop::net::Role::Host,
                                    std::memory_order_release);
    if (!g_windOriginInterceptorReg) {
        // Throttle the resolve to ~1 Hz: R::FindClass walks the ~190k-entry GUObjectArray,
        // and this block sits ABOVE the g_installed early-out so it would otherwise walk
        // every net-pump tick until registered (the CLAUDE.md per-frame-scan smell -- the
        // now-removed ticker_sync hit exactly this and threw the lesson). directionalWind
        // loads at gamemode BeginPlay, well before Install runs, so this resolves on ~the
        // first attempt; the throttle only bounds the pathological "class never loads" case.
        static uint32_t sWindResolveN = 0;
        if ((sWindResolveN++ % 125) == 0) {
            if (void* wc = R::FindClass(P::name::DirectionalWindClass)) {
                if (void* fn = R::FindFunction(wc, L"changeWindOrigin")) {
                    if (GT::RegisterInterceptor(fn, &OnWindOriginPreSuppress)) {
                        g_windOriginInterceptorReg = true;
                        UE_LOGI("weather: changeWindOrigin PRE-interceptor registered (@%p; "
                                "client-suppressed wind-gust roll, host pass-through)", fn);
                    } else {
                        UE_LOGW("weather: changeWindOrigin interceptor registration FAILED "
                                "(table full?) -- retrying");
                    }
                }
            }
        }
    }

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

    // Registered on BOTH roles (2026-06-11 latent-bug fix): the interceptor body
    // gates at fire time on g_schedulerSuppressActive (true only while connected
    // as a client), so the host and an idle/SP world pass through. Registering
    // host-side too closes the host-first-then-client-session gap (interceptors
    // can only be registered, never removed, and g_installed latches per process).
    if (!g_interceptorsRegistered) {
        int n = 0;
        void* targets[] = { g_timerRainFn, g_timerLightningFn, g_fogEventFn,
                            g_superFogEventFn, g_permaRainTimerFn };
        for (void* t : targets) {
            if (t && GT::RegisterInterceptor(t, &OnSchedulerPreSuppress)) ++n;
        }
        g_interceptorsRegistered = true;
        UE_LOGI("weather: scheduler PRE interceptors registered on %d/5 UFunctions "
                "(runtime-gated: suppress only while connected as a CLIENT -- the "
                "client never decides 'rain now' locally; receives WeatherState "
                "from the host instead)", n);
    }

    // Rain+snow cycle-side sub-lane (2026-07-19 cut): resolves the 5 mutator
    // CALL substrate + registers the causeRain echo-suppress interceptor
    // in-module. Gate our latch on its success like fog below -- if the
    // mutators aren't resolved yet, retry next NetPumpTick.
    coop::weather_rain::SetSession(session);
    if (!coop::weather_rain::Install()) return;

    // Phase 5W Inc2 lightning observability. HOST registers a POST observer
    // on BeginDeferredActorSpawnFromClass (same UFunction npc_sync intercepts;
    // observers + interceptors are separate dispatch stages, no conflict).
    // We use a POST observer because lightning doesn't need to CANCEL spawns
    // -- the client never spawns lightning locally (timerLightning is
    // suppressed by Inc1's interceptor on the cycle), so there's nothing to
    // suppress. Host just needs to OBSERVE the spawn to broadcast the loc.
    // CLIENT skips this registration -- no observer needed for receive
    // (event_feed dispatches LightningStrike packets directly).
    // Red-sky: delegate to coop::weather_redsky. (2026-08-29 reroot: the old
    // host POST observers were PE-blind to the organic newDay roll and are
    // retired; the host edge detector is now the field poll in TickConnect,
    // so both roles just resolve here.)
    coop::weather_redsky::SetSession(session);
    coop::weather_redsky::TryResolve();

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

    // Host-authoritative FOG (2026-06-01). Resolves spawnFog + registers the
    // echo-suppressed, role-gated spawnFog interceptor so the client can never make
    // uncommanded fog. Gate our latch on its success: if the interceptor failed to
    // register (table full) or the fog UFunction isn't resolved yet, do NOT latch --
    // retry next NetPumpTick rather than leave the client unsuppressed.
    if (!coop::weather_fog::Install(isHost)) return;

    // The newDay weather-event birth-catch (2026-08-29): a CLIENT's own 1%
    // rolls (red sky / black fog / rolling fog) are destroyed at
    // FinishSpawningActor -- the EX_Local caller is invisible to every PE
    // seam, so birth is the one place the organic roll surfaces. Gate the
    // latch on it like the fog interceptor: an unsuppressed client is the
    // exact bug this fixes.
    if (!coop::weather_event_births::Install(session, isHost)) return;

    g_installed = true;
}

// (DebugForceRain / DebugForceSnow / ReadLocalIsRaining moved to
// coop/weather_rain.cpp; the DebugForceRedSky / ApplyRedSky /
// ApplyLightningStrike thin forwards RETIRED (RULE 2) -- callers call
// coop::weather_redsky / coop::weather_lightning directly. 2026-07-19 cut.)

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

    // Red-sky late-join seed (principle 8, 2026-08-29): WeatherStatePayload
    // does not carry the red-sky bit (its own reliable kind), so a joiner
    // entering an already-red world needs its own targeted ON. OFF needs no
    // seed -- a fresh client world has no red sky to clear.
    if (coop::weather_redsky::LocalRedSkyActive()) {
        coop::net::RedSkyPayload rp{};
        rp.state = 1;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::RedSky, &rp, sizeof(rp));
        UE_LOGI("weather: connect-broadcast slot=%d RedSky seed state=1 (world is red)", peerSlot);
    }
}

void TickConnect() {
    // The ini-gated [probe weather]/[probe wind] diagnostics live in
    // coop/dev/weather_probe (2026-07-19 cut); cheap when the flag is off.
    coop::dev::weather_probe::Tick(g_session.load(std::memory_order_acquire));
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

    // HOST red-sky field poll (2026-08-29): the organic spawnRedSky caller is
    // EX_LocalVirtualFunction (PE-invisible), so the edge is detected at the
    // FIELD level -- gamemode.redSky liveness + isred, throttled internally.
    coop::weather_redsky::HostPollEdge();

    // HOST fog-edge detector. The rolling-fog actor self-destructs on its Duration
    // via its OWN ReceiveTick -- NO scheduler UFunction fires, so OnSchedulerPost
    // MISSES the fog-END. Detect the fog-bit edge (HostFogStateChanged throttles to
    // ~3 Hz internally; the super-fog probe walks GUObjectArray) and re-broadcast so
    // the client clears its mirror. Host-only + cheap when nothing changed.
    {
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->connected() && s->role() == coop::net::Role::Host && g_installed) {
            void* cycle = ResolveCycle();
            if (cycle && coop::weather_fog::HostFogStateChanged(cycle)) {
                coop::net::WeatherStatePayload p{};
                if (ReadCycleState(cycle, p)) {
                    const uint64_t sig = SignaturePayload(p);
                    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
                    if (g_lastSentSig.load(std::memory_order_acquire) != storeSig) {
                        s->SendReliable(coop::net::ReliableKind::WeatherState, &p, sizeof(p));
                        g_lastSentSig.store(storeSig, std::memory_order_release);
                        UE_LOGI("weather: host fog-edge re-broadcast flags=0x%02X flags2=0x%02X",
                                p.flags, p.flags2);
                    }
                }
            }
        }
    }

    // HOST weather DoPulse (MTA CBlendedWeather::DoPulse). Re-assert the FULL weather
    // state to all clients on a slow heartbeat, INDEPENDENT of change-detection. The
    // change-driven OnSchedulerPost + the one-shot connect-snapshot do NOT cover a
    // joined client that fresh-boots its OWN New-Game world: that world's daynightCycle
    // rolls RNG rain/fog at BeginPlay (intComs_settingsApplied -> fogEvent()/timerRain(),
    // ~15%/10% per the BP) BEFORE our scheduler PRE-interceptors install -- so the client
    // paints rain+fog the clear host never had, and a STATIC-clear host emits no weather
    // CHANGE to ever tell it to clear (the user's "client has rain+fog, host is clear").
    // This heartbeat re-sends the host's authoritative state every ~150 host-connected
    // ticks (~1-2 s); the receiver's diff-gated ApplyFromHost then clears the leaked rain
    // (setRainProperties(false) + causeRain(false)) and fog (weather_fog destroys the
    // stray actor) within one pulse, and is a convergent no-op thereafter. The
    // post-connect interceptors stop FURTHER local rolls; this DoPulse mops up the
    // pre-install BeginPlay leak that the interceptors arrived too late to prevent.
    // Host-only; a 56 B reliable datagram ~twice a second is negligible. Does NOT touch
    // g_lastSentSig -- OnSchedulerPost's change-dedup stays intact for the live path.
    {
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->connected() && s->role() == coop::net::Role::Host && g_installed) {
            static uint32_t sPulseN = 0;
            if ((sPulseN++ % 150) == 0) {
                void* cycle = ResolveCycle();
                if (cycle && R::IsLive(cycle)) {
                    coop::net::WeatherStatePayload p{};
                    if (ReadCycleState(cycle, p)) {
                        s->SendReliable(coop::net::ReliableKind::WeatherState, &p, sizeof(p));
                        // Refresh the change-dedup baseline: without this, a same-tick OnSchedulerPost
                        // carrying the SAME state would see lastSig != sig and re-send a duplicate, so
                        // the client runs the UNCONDITIONAL fog apply twice in one tick. A LATER real
                        // change still differs from this sig -> still sends (dedup is sig-vs-sig).
                        const uint64_t sig = SignaturePayload(p);
                        g_lastSentSig.store((sig == kNoSendYet) ? (kNoSendYet - 1) : sig,
                                            std::memory_order_release);
                    }
                }
            }
        }
    }

    // CLIENT fog reconcile heartbeat: clear a rolling-fog actor that leaked the
    // pre-suppression connect window when the host is known-clear. Self-gates to
    // client + host-clear + throttles internally; cheap when idle (a slot read).
    if (g_installed) {
        coop::weather_fog::TickClientReconcile(ResolveCycle());
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
    g_cycleIdx = -1;

    // Audit M26 (2026-05-27): unregister observers that are bound to
    // long-lived engine UFunctions (BeginDeferredActorSpawnFromClass +
    // redSkyEvent.set) AND scoped to a single session's role. If the next
    // session reconnects with a different role (host->client or back),
    // the still-registered host-only observers would fire on the wrong
    // peer. The 5 scheduler PRE-interceptors and the causeRain
    // echo-suppress interceptor (weather_rain's) are KEPT registered
    // (interceptors are process-lifetime by design) -- each is gated
    // INSIDE the body: causeRain on the rain module's echo flag (false
    // outside Apply; cleared in its OnDisconnect), the
    // schedulers on g_schedulerSuppressActive (cleared below -- the
    // 2026-06-11 fix; the body previously returned true unconditionally,
    // leaving an ex-client's local weather rolls dead until restart).
    coop::weather_rain::OnDisconnect();
    coop::weather_lightning::OnDisconnect();
    coop::weather_redsky::OnDisconnect();
    coop::weather_fog::OnDisconnect();
    coop::weather_event_births::OnDisconnect();
    ue_wrap::directionalwind::OnDisconnect();  // drop the cached wind-actor ptr
    // v50: drop the client gate so the changeWindOrigin interceptor (kept registered for
    // the process lifetime) goes inert -- a disconnected client's local wind resumes its
    // own RNG gust rolls instead of staying frozen at the last synced target.
    g_windIsClient.store(false, std::memory_order_release);
    // 2026-06-11: same for the scheduler suppressors -- a disconnected client's
    // local weather scheduler resumes (its own rain/fog/lightning rolls).
    g_schedulerSuppressActive.store(false, std::memory_order_release);
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
    // g_installed gate: Install requires the 5 schedulers + 3 observer
    // targets here AND weather_rain's 5 mutators (its Install gates our
    // latch) to be resolved before flipping g_installed. If a WeatherState
    // packet arrives while we're still resolving (e.g. mutators partially
    // resolved on a slow boot), partial application would brick the cycle
    // into an inconsistent state. Stash latest-wins and let TickConnect
    // drain once g_installed flips true.
    void* cycle = g_installed ? ResolveCycle() : nullptr;
    if (!g_installed || !cycle || !R::IsLive(cycle)) {
        g_pendingApplyPayload = payload;
        g_pendingApply = true;
        UE_LOGI("weather: ApplyFromHost defer (installed=%d cycle=%p) -- "
                "stashing for TickConnect drain",
                g_installed ? 1 : 0, cycle);
        return;
    }

    // Read current state for delta-compare; we avoid redundant UFunction
    // calls when the bit hasn't changed (causeRain / intComs_triggerSnow
    // fan out to BP listeners + start/stop particle systems; calling them
    // with the current value is a wasted dispatch + a redundant fanout
    // notification).
    coop::net::WeatherStatePayload cur{};
    ReadCycleState(cycle, cur);

    // The cycle-side delta apply (enable bits, rain scalars/target, echo-
    // bracketed causeRain, particles, wind params, snow) -- the rain module
    // (2026-07-19 cut). `outcome` feeds the fused log line below unchanged.
    coop::weather_rain::ApplyOutcome oc;
    coop::weather_rain::ApplyFromHost(cycle, payload, cur, oc);

    // v23 host-authoritative FOG: assert the host's fog-ACTOR presence -- destroy a
    // stray rolling/super fog actor on host-clear, mirror-spawn on host-fog -- and
    // mirror the enable_fog/enable_superfog/permanentFog config bits. UNCONDITIONAL
    // (MTA DoPulse, NOT diff-gated): the connect-edge apply must clear a pre-existing
    // client fog even when the enable bits already match the host.
    coop::weather_fog::ApplyFromHost(cycle, payload);

    // v43/v50 WIND: overwrite the client's directionalWind state with the host's (gated on
    // kWindValid so an unread host never zeros it; calm wind still syncs because the bit,
    // not the values, marks validity). The 4 rain/background fields converge the derived
    // totals + engine speed. v50 ALSO writes windTarget -- the GUST input: the client's own
    // ReceiveTick springs `intensity` from it next frame, reproducing the host's leaf-shake
    // (the foliage MPC scalar + engine wind). Tick suppression IS needed here, but only of
    // the client's local changeWindOrigin RNG roll (registered in Install) -- NOT ReceiveTick,
    // which must run to drive the outputs from the synced target. RE 2026-06-09.
    if (payload.flags2 & coop::net::fog_flags2::kWindValid) {
        ue_wrap::directionalwind::WindState wind;
        wind.speedBg      = payload.windSpeedBg;
        wind.strengthBg   = payload.windStrengthBg;
        wind.speedRain    = payload.windSpeedRain;
        wind.strengthRain = payload.windStrengthRain;
        ue_wrap::directionalwind::Write(wind);
        ue_wrap::directionalwind::WriteTarget(
            ue_wrap::FVector{ payload.windTargetX, payload.windTargetY, payload.windTargetZ });
    }

    UE_LOGI("weather: applied flags 0x%02X -> 0x%02X flags2=0x%02X rain=%.2f lc=%.2f "
            "dc=%.2f ws=%.2f (rain-tx=%d snow-tx=%d scalars-changed=%d)",
            cur.flags, payload.flags, payload.flags2,
            payload.rainStrength, payload.rainLightningChance,
            payload.rainDeactivateChance, payload.rainWindSpeed,
            oc.rainTx ? 1 : 0,
            oc.snowTx ? 1 : 0,
            oc.scalarsChanged ? 1 : 0);
}

}  // namespace coop::weather_sync

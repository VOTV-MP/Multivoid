// coop/weather_sync.cpp -- the weather lane's hub: the host's cycle observers broadcast
// WeatherState, the client's interceptors suppress its own scheduler rolls, and the fog, rain,
// lightning, red-sky and event-birth modules hang off Install. See coop/weather_sync.h.

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

// UFunction pointers, resolved once per process (UClass-stable across cycle recreation).
void* g_timerRainFn       = nullptr;
void* g_timerLightningFn  = nullptr;
void* g_fogEventFn        = nullptr;
void* g_superFogEventFn   = nullptr;
void* g_permaRainTimerFn  = nullptr;

// The three mutator observer targets (causeRain, setRainProperties, intComs_triggerSnow),
// resolved here independently of weather_rain's own resolves of the same UFunctions: each module
// owns what it registers or calls. The call substrate for all five mutators is weather_rain's.
void* g_causeRainFn          = nullptr;
void* g_setRainPropertiesFn  = nullptr;
void* g_intComsTriggerSnowFn = nullptr;

// Lightning needs no echo-suppress flag: only the host registers the spawn observer, so a
// client's Apply broadcasts nothing, and event_feed drops LightningStrike on the host.

bool g_installed = false;
bool g_observersRegistered = false;     // host POST observers
bool g_interceptorsRegistered = false;  // client PRE interceptors

// The wind-gust suppression: a client must not run AdirectionalWind_C::changeWindOrigin (its
// local re-roll of windTarget every 1-60 s) or it fights the host-synced gust. One PRE
// interceptor on that UFunction, registered once on both roles and gated at fire time on this
// atomic (refreshed every Install, cleared on disconnect), so a reconnect as the host goes inert
// without re-registration and a disconnected client's wind resumes rolling.
std::atomic<bool> g_windIsClient{false};

// The fire-time gate for the five scheduler interceptors, the same shape: interceptors live for
// the process, so an unconditional body left an ex-client suppressing its own weather after
// disconnect, and a client-only registration left a process that hosted first unsuppressed as a
// later client. True only while connected as a client.
std::atomic<bool> g_schedulerSuppressActive{false};
bool g_windOriginInterceptorReg = false;

// Atomic: the observer and interceptor reads race the harness setter on another thread.
std::atomic<coop::net::Session*> g_session{nullptr};

// The dedupe signature of the last send; kNoSendYet separates "never sent" from a hash. One
// atomic, so the load is uninterruptible against worker-thread senders.
inline constexpr uint64_t kNoSendYet = 0xFFFFFFFFFFFFFFFFULL;
std::atomic<uint64_t> g_lastSentSig{kNoSendYet};

// A WeatherState that arrives before Install resolved every UFunction or before the local cycle
// is live is stashed here, latest wins, and TickConnect drains it once both gates pass. Game
// thread only.
bool g_pendingApply = false;
coop::net::WeatherStatePayload g_pendingApplyPayload{};

// The cached cycle (one per session; FindObjectByClass is a full array walk), re-validated with
// IsLiveByIndex: on world teardown the cycle's GUObjectArray slot is recycled to another actor,
// and plain IsLive passed the recycled slot, so a weather apply dispatched the cycle's UFunctions
// on a foreign object and the engine fataled on exit to menu. The serial check rejects the
// recycled slot and the pointer re-resolves. Game thread only.
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
    // The host's local Player element id; 0 before the host has one (the boot window), and the
    // receiver then falls back to the sender-slot check.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        out.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }

    // The rain and snow bits and the rain scalars come from the rain module, which resets flags:
    // first.
    coop::weather_rain::ReadState(cycle, out);
    // The active-fog bits, density and ramp state: fog is driven by event actors, not the enable
    // bits, so the wire carries live actor presence, and a joiner snaps to the host's fog level
    // instead of ramping from zero.
    coop::weather_fog::ReadHostFogState(cycle, out);

    // The directionalWind state (a separate actor, resolved by class) with windTarget, the gust
    // input behind the leaf shake. kWindValid is set only when both reads succeed, so an unread
    // host mid-transition never zeros the client's wind and a null target never ships a calm gust;
    // calm wind still syncs, since the bit, not the values, gates the apply.
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
    // FNV1a over the mutable fields; senderElementId is constant for the session.
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 0x100000001b3ULL; };
    mix(p.flags);
    mix(p.flags2);   // the fog actor bits: a fog edge with no rain change must not dedupe away
    mix(reinterpret_cast<const uint32_t&>(p.rainStrength));
    mix(reinterpret_cast<const uint32_t&>(p.rainLightningChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainDeactivateChance));
    mix(reinterpret_cast<const uint32_t&>(p.rainWindSpeed));
    // The wind fields: a wind-only change (the background wind shifting on a day rollover) must not
    // hash the same and dedupe away.
    mix(reinterpret_cast<const uint32_t&>(p.windSpeedBg));
    mix(reinterpret_cast<const uint32_t&>(p.windStrengthBg));
    mix(reinterpret_cast<const uint32_t&>(p.windSpeedRain));
    mix(reinterpret_cast<const uint32_t&>(p.windStrengthRain));
    // The gust too, so a gust-only change on a scheduler or fog-edge send is not deduped (the pulse
    // re-sends it regardless).
    mix(reinterpret_cast<const uint32_t&>(p.windTargetX));
    mix(reinterpret_cast<const uint32_t&>(p.windTargetY));
    mix(reinterpret_cast<const uint32_t&>(p.windTargetZ));
    return h;
}

// The host's POST observer: read the cycle after a scheduler or mutator ran, dedupe, broadcast.

void OnSchedulerPost(void* self, void* function, void* /*params*/) {
    // An ini-only trace of observer firing, read once per process.
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

    s->SendReliable(coop::net::ReliableKind::WeatherState, &p, sizeof(p));
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("weather: host broadcast flags=0x%02X rain=%.2f lc=%.2f dc=%.2f ws=%.2f (sig=%llX)",
            p.flags, p.rainStrength, p.rainLightningChance,
            p.rainDeactivateChance, p.rainWindSpeed,
            static_cast<unsigned long long>(storeSig));
}

// The client's PRE interceptor on the five schedulers.

bool OnSchedulerPreSuppress(void* /*self*/, void* /*params*/) {
    // True skips the UFunction body. Gated on the connected-as-client atomic: the host and an idle
    // world pass through, a connected client's own weather rolls are suppressed (it gets
    // WeatherState instead). No self filter (one cycle per session); silent, since each fires on a
    // timer.
    return g_schedulerSuppressActive.load(std::memory_order_acquire);
}

// The changeWindOrigin interceptor: cancelled on a client so the host-synced gust holds; one wind
// actor per session, so no self filter, and silent (a 1-60 s timer). The counters let the probe
// line tell "suppression never fired" from "synced, but the visuals diverged": the BP body
// re-arms its own timer, so one suppressed fire ends the client's roll chain and a suppressed
// total of 1 is the healthy count; fired counts on the host are its own gust rolls.
std::atomic<uint32_t> g_windRollFired{0};       // dispatches seen (both roles)
std::atomic<uint32_t> g_windRollSuppressed{0};  // dispatches cancelled (client)
bool OnWindOriginPreSuppress(void* /*self*/, void* /*params*/) {
    g_windRollFired.fetch_add(1, std::memory_order_relaxed);
    const bool suppress = g_windIsClient.load(std::memory_order_acquire);
    if (suppress) g_windRollSuppressed.fetch_add(1, std::memory_order_relaxed);
    return suppress;
}

// The installer, re-entered every pump tick until it latches.

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

    int sResolved = 0, mResolved = 0;
    for (auto& e : sched) {
        if (*e.out) { ++sResolved; continue; }
        if (void* fn = R::FindFunction(cls, e.name)) { *e.out = fn; ++sResolved; }
    }
    for (auto& e : mut) {
        if (*e.out) { ++mResolved; continue; }
        if (void* fn = R::FindFunction(cls, e.name)) { *e.out = fn; ++mResolved; }
    }
    // All five schedulers (broadcast and suppression) and all three mutator targets.
    return sResolved == 5 && mResolved == 3;
}

}  // namespace

uint32_t WindRollFired() {
    return g_windRollFired.load(std::memory_order_relaxed);
}
uint32_t WindRollSuppressed() {
    return g_windRollSuppressed.load(std::memory_order_relaxed);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);

    // The wind interceptor sits above the cycle-gated early-out: the directionalWind class may load
    // after the cycle. The role gates refresh every call (a reconnect with the other role needs no
    // re-registration); the registration itself is once, best effort, retried next tick.
    g_windIsClient.store(session && session->role() != coop::net::Role::Host,
                         std::memory_order_release);
    // The scheduler gate, refreshed the same way.
    g_schedulerSuppressActive.store(session && session->role() != coop::net::Role::Host,
                                    std::memory_order_release);
    if (!g_windOriginInterceptorReg) {
        // The resolve is throttled to one attempt in 125 ticks: FindClass walks the whole
        // GUObjectArray, and this sits above the latch. directionalWind loads at gamemode
        // BeginPlay, before Install runs, so it resolves on about the first attempt; the throttle
        // bounds the class-never-loads case.
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
        // The cycle class is not loaded yet, or a UFunction was renamed by a recook; retry next
        // tick.
        return;
    }

    if (!session) {
        // No session yet; the harness re-calls with it.
        return;
    }

    const bool isHost = (session->role() == coop::net::Role::Host);

    if (isHost && !g_observersRegistered) {
        int n = 0;
        struct Tgt { void* fn; const wchar_t* name; };
        const Tgt all[] = {
            // The five schedulers carry the organic, timer-rolled changes.
            { g_timerRainFn,       L"timerRain" },
            { g_timerLightningFn,  L"timerLightning" },
            { g_fogEventFn,        L"fogEvent" },
            { g_superFogEventFn,   L"superFogEvent" },
            { g_permaRainTimerFn,  L"permaRain_timer" },
            // The three mutators carry the changes that bypass the schedulers (a story event
            // calling causeRain or intComs_triggerSnow, a dev command calling setRainProperties).
            // One callback for all eight: it reads the post-mutation state and dedupes.
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

    // Registered on both roles, since an interceptor can only be added, never removed, and the
    // installed latch is per process; the body gates at fire time.
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

    // The rain module resolves its five mutators and registers the causeRain echo-suppress
    // interceptor; the latch waits on it.
    coop::weather_rain::SetSession(session);
    if (!coop::weather_rain::Install()) return;

    // Red sky: both roles resolve; the host's edge is the field poll in TickConnect, since the
    // organic roll is invisible to a ProcessEvent observer.
    coop::weather_redsky::SetSession(session);
    coop::weather_redsky::TryResolve();

    // Lightning: the host observes BeginDeferredActorSpawnFromClass (a POST observer; nothing to
    // cancel, the client never spawns lightning locally since timerLightning is suppressed) and
    // broadcasts the strike. SetSession on every re-entry, so the once-registered observer reads
    // the current session.
    coop::weather_lightning::SetSession(session);
    if (isHost) {
        coop::weather_lightning::RegisterHostObserver();
        // RegisterHostObserver resolves first and is retried by the re-entry until the spawn path
        // loads.
    } else {
        // The client needs the spawn path for Apply.
        coop::weather_lightning::TryResolve();
    }

    // Fog: the role-gated, echo-suppressed spawnFog interceptor, so a client never makes
    // uncommanded fog. The latch waits on it: an unregistered interceptor must retry, not leave the
    // client unsuppressed.
    if (!coop::weather_fog::Install(isHost)) return;

    // The event-birth catch: a client's own newDay rolls (red sky, black fog, rolling fog) are
    // destroyed at FinishSpawningActor, the one place the organic roll surfaces (its caller is
    // invisible to every ProcessEvent seam). The latch waits on it too.
    if (!coop::weather_event_births::Install(session, isHost)) return;

    g_installed = true;
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
    // To the one slot: a joiner mid-storm needs the current state, and the fan-out never reaches
    // it.
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WeatherState, &p, sizeof(p));
    const uint64_t sig = SignaturePayload(p);
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    UE_LOGI("weather: connect-broadcast slot=%d sent flags=0x%02X rain=%.2f lc=%.2f dc=%.2f ws=%.2f",
            peerSlot, p.flags, p.rainStrength, p.rainLightningChance,
            p.rainDeactivateChance, p.rainWindSpeed);

    // The red-sky seed: WeatherState does not carry the red-sky bit (its own reliable kind), so a
    // joiner entering a red world gets its own ON. OFF needs no seed.
    if (coop::weather_redsky::LocalRedSkyActive()) {
        coop::net::RedSkyPayload rp{};
        rp.state = 1;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::RedSky, &rp, sizeof(rp));
        UE_LOGI("weather: connect-broadcast slot=%d RedSky seed state=1 (world is red)", peerSlot);
    }
}

void TickConnect() {
    // The ini-gated weather and wind probes; cheap when off.
    coop::dev::weather_probe::Tick(g_session.load(std::memory_order_acquire));
    // The deferred apply drains once the cycle is live (a state defer, not a channel retry).
    if (g_pendingApply && g_installed) {
        void* cycle = ResolveCycle();
        if (cycle && R::IsLive(cycle)) {
            coop::net::WeatherStatePayload p = g_pendingApplyPayload;
            g_pendingApply = false;
            UE_LOGI("weather: draining deferred apply (flags=0x%02X rain=%.2f)",
                    p.flags, p.rainStrength);
            ApplyFromHost(p);  // re-enters; this time both gates pass
        }
        // Otherwise the cycle is still loading; next tick.
    }

    // The host's red-sky edge, polled at the field level (gamemode.redSky liveness and isred),
    // throttled inside.
    coop::weather_redsky::HostPollEdge();

    // The host's fog-edge detector: the rolling-fog actor self-destructs from its own tick, no
    // scheduler UFunction fires, so the observer misses the fog end. HostFogStateChanged throttles
    // to ~3 Hz (the super-fog probe walks the array); a changed bit re-broadcasts so the client
    // clears its mirror.
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

    // The host's pulse (MTA's CBlendedWeather::DoPulse): the full state to every client every 150
    // ticks, independent of change detection. A client that fresh-boots its own New Game world
    // rolls rain and fog at BeginPlay before the interceptors install, and a static-clear host
    // emits no change to tell it to clear; the pulse lands within a second or two, the client's
    // diff-gated apply clears the leaked rain and fog, and it is a no-op after. The interceptors
    // stop further rolls; the pulse mops up the pre-install leak.
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
                        // The dedupe baseline follows the pulse, or a same-tick observer with the
                        // same state re-sends it and the client runs the unconditional fog apply
                        // twice; a later real change still differs.
                        const uint64_t sig = SignaturePayload(p);
                        g_lastSentSig.store((sig == kNoSendYet) ? (kNoSendYet - 1) : sig,
                                            std::memory_order_release);
                    }
                }
            }
        }
    }

    // The client's fog reconcile: a rolling-fog actor that leaked in the pre-suppression connect
    // window clears once the host is known clear. Self-gated and throttled.
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
    // The cycle cache may dangle after a session ending mid-transition; the next resolve re-walks.
    g_cycleCache = nullptr;
    g_cycleIdx = -1;

    // The lightning module unregisters its host observer: it is bound to a long-lived engine
    // UFunction and scoped to one session's role, and a reconnect as a client would fire it on the
    // wrong peer. The interceptors stay registered for the process and are gated inside their
    // bodies: causeRain on the rain module's echo flag, the schedulers on the atomic cleared below.
    coop::weather_rain::OnDisconnect();
    coop::weather_lightning::OnDisconnect();
    coop::weather_redsky::OnDisconnect();
    coop::weather_fog::OnDisconnect();
    coop::weather_event_births::OnDisconnect();
    ue_wrap::directionalwind::OnDisconnect();  // drop the cached wind-actor ptr
    // The wind gate drops, so a disconnected client's own gust rolls resume instead of freezing at
    // the last synced target.
    g_windIsClient.store(false, std::memory_order_release);
    // The scheduler gate too: the local weather rolls resume.
    g_schedulerSuppressActive.store(false, std::memory_order_release);
    // The latch clears so the next session's Install re-enters and re-registers the lightning
    // observer. The per-feature registration flags stay set: those callbacks are role-gated inside,
    // and the UFunction pointers are stable across cycle recreation.
    g_installed = false;
}

void ApplyFromHost(const coop::net::WeatherStatePayload& payload) {
    // The sender-is-host check is event_feed's (senderPeerSlot 0) before this is posted. The latch
    // gate: a WeatherState applied while the mutators are still resolving would leave the cycle
    // inconsistent, so it is stashed, latest wins, until TickConnect drains it.
    void* cycle = g_installed ? ResolveCycle() : nullptr;
    if (!g_installed || !cycle || !R::IsLive(cycle)) {
        g_pendingApplyPayload = payload;
        g_pendingApply = true;
        UE_LOGI("weather: ApplyFromHost defer (installed=%d cycle=%p) -- "
                "stashing for TickConnect drain",
                g_installed ? 1 : 0, cycle);
        return;
    }

    // The current state, for the delta: causeRain and intComs_triggerSnow fan out to BP listeners
    // and start or stop particle systems, so they are not called with the value they already hold.
    coop::net::WeatherStatePayload cur{};
    ReadCycleState(cycle, cur);

    // The cycle-side delta (enable bits, rain scalars and target, the echo-bracketed causeRain,
    // particles, wind params, snow) is the rain module's; the outcome feeds the log line.
    coop::weather_rain::ApplyOutcome oc;
    coop::weather_rain::ApplyFromHost(cycle, payload, cur, oc);

    // Fog asserts the host's actor presence (a stray fog actor destroyed on host-clear, a mirror
    // spawned on host-fog) and mirrors the enable bits. Unconditional, not diff-gated: the
    // connect-edge apply must clear a pre-existing client fog even when the bits already match.
    coop::weather_fog::ApplyFromHost(cycle, payload);

    // Wind: the client's directionalWind state is overwritten with the host's when kWindValid is
    // set, windTarget included; the client's own ReceiveTick then springs the intensity from the
    // target next frame, reproducing the host's leaf shake. Only the local changeWindOrigin roll is
    // suppressed, never ReceiveTick, which drives the outputs from the synced target.
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

// coop/event_cue_sync.cpp -- see coop/event_cue_sync.h.
//
// HOST-AUTHORITATIVE: only the host detects + broadcasts (clients run a dormant scheduler --
// time_sync pins TimeScale=0 -- so they never fire an event). The detection is a ~2 Hz host-only
// poll that diffs the live ParticleSystemComponent set against the registered cue templates (the
// EX_CallMath SpawnEmitterAtLocation is invisible to our ProcessEvent detour, the firefly trap).
// Clients replay the emitter via a reflected SpawnEmitterAtLocation (the firefly_sync spawn path).

#include "coop/world/event_cue_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace coop::event_cue_sync {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the cue registry (B1: cosmetic emitter cues) -----------------------------------------
// cueId == index. APPEND-ONLY -- the id is on the wire. starRain is bytecode-verified
// (trigger_eventer @4709: SpawnEmitterAtLocation(eff_shootingStar_rain, (0,0,6000))). Add the
// other PSC-based cosmetic cues here (Eye Moon, Pink Beam, TriFO, Blinking Lights, Green-Fire) --
// one line each. `fixedLocation` cues spawn at `loc` (the BP hardcodes the position); otherwise
// the host captures the live component's location.
//
// CAVEAT (detection is a ~1 Hz poll-diff): a cue must LIVE longer than ~1 s to be caught.
// starRain is safe (Meteor Shower ~2 min; a single Shooting Star several s). Before registering a
// SHORT sub-second one-shot cue, give it a SYNCHRONOUS capture (the firefly_sync PRE/POST tick
// window) instead of this poll -- otherwise it can spawn-and-die between polls and never broadcast.
struct CueDef {
    const char* name;             // log label
    const wchar_t* templateName;  // the UParticleSystem object the SpawnEmitter call uses
    bool fixedLocation;           // true -> spawn at `loc`; false -> use the captured PSC pos
    ue_wrap::FVector loc;         // used iff fixedLocation
};
constexpr CueDef kCues[] = {
    { "starRain", L"eff_shootingStar_rain", true, { 0.f, 0.f, 6000.f } },  // Meteor Shower / Shooting Star
};
constexpr int kCueCount = static_cast<int>(sizeof(kCues) / sizeof(kCues[0]));

// Resolved cue template objects (parallel to kCues; GUObjectArray entries are stable once cached).
void* g_cueTemplates[kCueCount] = {};
int   g_resolvedTemplates = 0;

// Reflected spawn path (resolved once -- cloned from firefly_sync).
void* g_gameplayStaticsCdo = nullptr;
void* g_spawnEmitterFn     = nullptr;

// Host poll state (game-thread only).
std::unordered_set<void*> g_lastCuePscs;  // cue-matching PSCs seen at the last poll
long long g_lastPollMs = 0;
// ~1 Hz host-only walk (matches turbine_sync's cadence). The shortest cue (a single Shooting
// Star) lives several seconds and the Meteor Shower ~2 real minutes, so 1 Hz catches every cue
// with large margin while keeping the periodic FindObjectsByClass walk cheap.
constexpr long long kPollIntervalMs = 1000;

constexpr const wchar_t* kPscClass = L"ParticleSystemComponent";

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Resolve cue template objects opportunistically. Keeps retrying the not-yet-loaded ones (a
// cue's particle asset only loads when its owning BP -- e.g. trigger_eventer -- is in the world).
// Does NOT gate on ALL resolved: one cue's missing asset must not block detecting the others.
void ResolveTemplates() {
    if (g_resolvedTemplates == kCueCount) return;
    int n = 0;
    for (int i = 0; i < kCueCount; ++i) {
        if (!g_cueTemplates[i])
            g_cueTemplates[i] = R::FindObject(kCues[i].templateName, L"ParticleSystem");
        if (g_cueTemplates[i]) ++n;
    }
    g_resolvedTemplates = n;
}

bool ResolveSpawn() {
    if (g_gameplayStaticsCdo && g_spawnEmitterFn) return true;
    if (!g_gameplayStaticsCdo)
        g_gameplayStaticsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gameplayStaticsCdo && !g_spawnEmitterFn) {
        if (void* cls = R::ClassOf(g_gameplayStaticsCdo))
            g_spawnEmitterFn = R::FindFunction(cls, L"SpawnEmitterAtLocation");
    }
    return g_gameplayStaticsCdo && g_spawnEmitterFn;
}

// Which cue (if any) a live PSC belongs to, by Template identity. -1 = not a cue.
int CueIdForTemplate(void* tmpl) {
    if (!tmpl) return -1;
    for (int i = 0; i < kCueCount; ++i)
        if (g_cueTemplates[i] == tmpl) return i;
    return -1;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Opportunistic resolve (cheap once cached) -- the client needs the spawn path + the cue
    // template before a packet lands; the host needs them before the poll.
    ResolveSpawn();
    ResolveTemplates();
}

void Tick() {
    // HOST-ONLY, ~1 Hz, connected. The only cost is the FindObjectsByClass GUObjectArray walk,
    // capped to 1 Hz + host-only (the cue-PSC set is virtually always empty). NOTE for the perf
    // audit: this is a bounded periodic walk like turbine_sync's host poll, NOT a per-frame scan.
    // Game-thread check FIRST -- before touching g_lastPollMs -- so a hypothetical off-thread
    // call is a true no-op that mutates no poll state (the GUObjectArray walk + raw reads are
    // game-thread only). In the shipping path TickGameplay is always game-thread (net_pump::Tick
    // asserts it), so this guard never actually fires; it's defensive.
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;

    ResolveTemplates();
    if (g_resolvedTemplates == 0) return;  // no cue asset loaded yet -> no event possible -> skip the walk

    std::unordered_set<void*> current;
    for (void* psc : R::FindObjectsByClass(kPscClass)) {
        const int cueId = CueIdForTemplate(ue_wrap::engine::GetParticleSystemTemplate(psc));
        if (cueId < 0) continue;          // not a registered cosmetic cue
        current.insert(psc);
        if (g_lastCuePscs.count(psc)) continue;  // already broadcast on a prior poll (still alive)

        // NEW cue PSC -> the host just fired this event. Broadcast its spawn position.
        ue_wrap::FVector loc = kCues[cueId].loc;
        if (!kCues[cueId].fixedLocation) {
            loc = ue_wrap::engine::GetComponentRelativeLocation(psc);
            if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) {
                UE_LOGW("event_cue: '%s' PSC has non-finite location -- skipping", kCues[cueId].name);
                continue;
            }
        }
        coop::net::EventCuePayload p{ static_cast<uint32_t>(cueId), loc.X, loc.Y, loc.Z };
        s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
        UE_LOGI("event_cue: host broadcast '%s' (cue %d) at (%.0f, %.0f, %.0f)",
                kCues[cueId].name, cueId, loc.X, loc.Y, loc.Z);
    }
    g_lastCuePscs.swap(current);
}

void OnReliable(const coop::net::EventCuePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_cue: OnReliable off-game-thread -- dropping"); return; }
    if (payload.cueId >= static_cast<uint32_t>(kCueCount)) {
        UE_LOGW("event_cue: OnReliable unknown cueId=%u (have %d cues) -- dropping (newer host?)",
                payload.cueId, kCueCount);
        return;
    }
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z)) return;
    ResolveTemplates();
    if (!ResolveSpawn()) {
        UE_LOGW("event_cue: OnReliable spawn path unresolved (cdo=%p fn=%p) -- dropping",
                g_gameplayStaticsCdo, g_spawnEmitterFn);
        return;
    }
    void* tmpl = g_cueTemplates[payload.cueId];
    if (!tmpl) {
        UE_LOGW("event_cue: cue %u ('%s') template not loaded on this peer -- dropping",
                payload.cueId, kCues[payload.cueId].name);
        return;
    }
    void* worldCtx = coop::players::Registry::Get().Local();
    if (!worldCtx) return;  // no local pawn yet -> can't resolve the world

    ue_wrap::FVector loc{ payload.x, payload.y, payload.z };
    ue_wrap::FVector scale{ 1.f, 1.f, 1.f };
    ue_wrap::FRotator rot{ 0.f, 0.f, 0.f };
    ue_wrap::ParamFrame f(g_spawnEmitterFn);
    f.Set<void*>(L"WorldContextObject", worldCtx);
    f.Set<void*>(L"EmitterTemplate", tmpl);
    f.SetRaw(L"Location", &loc, sizeof(loc));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);       // EPSCPoolMethod::None
    f.Set<bool>(L"bAutoActivateSystem", true);
    ue_wrap::Call(g_gameplayStaticsCdo, f);
    UE_LOGI("event_cue: replayed '%s' (cue %u) at (%.0f, %.0f, %.0f)",
            kCues[payload.cueId].name, payload.cueId, payload.x, payload.y, payload.z);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    // HOST, join edge only (rare). One bounded PSC walk -- the same cost as one Tick poll.
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    ResolveTemplates();
    if (g_resolvedTemplates == 0) return;  // no cue asset loaded -> no live cue possible

    for (void* psc : R::FindObjectsByClass(kPscClass)) {
        const int cueId = CueIdForTemplate(ue_wrap::engine::GetParticleSystemTemplate(psc));
        if (cueId < 0) continue;
        // Only re-send cues the poll has ALREADY broadcast. A cue newer than the last poll is
        // not in the snapshot yet; the next Tick broadcasts it to everyone INCLUDING this slot
        // (its send gate is open now -- ConnectReplayForSlot runs post-world-ready), so sending
        // it here too would spawn the emitter twice on the joiner.
        if (!g_lastCuePscs.count(psc)) continue;
        ue_wrap::FVector loc = kCues[cueId].loc;
        if (!kCues[cueId].fixedLocation) {
            loc = ue_wrap::engine::GetComponentRelativeLocation(psc);
            if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) continue;
        }
        coop::net::EventCuePayload p{ static_cast<uint32_t>(cueId), loc.X, loc.Y, loc.Z };
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::EventCue, &p, sizeof(p));
        UE_LOGI("event_cue: connect-snapshot -- re-sent live '%s' (cue %d) to slot %d",
                kCues[cueId].name, cueId, peerSlot);
    }
    // The common case (no event running) stays silent -- the per-cue line above is the diagnostic.
}

void OnDisconnect() {
    g_lastCuePscs.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::event_cue_sync

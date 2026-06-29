// coop/firefly_sync.cpp -- see coop/firefly_sync.h.
//
// PEER-SYMMETRIC: every peer runs its OWN firefly spawner, captures its own spawns by
// PRE+POST-observing the spawner's ReceiveTick and diffing the live ParticleSystemComponent
// set (the EX_CallMath SpawnEmitterAtLocation is invisible to our ProcessEvent detour),
// broadcasts the position, and spawns eff_fireflies at every OTHER peer's broadcast position
// via a reflected SpawnEmitterAtLocation. No suppression; the host relays a client's spawn
// to the other clients (IsClientRelayableReliableKind).

#include "coop/world/firefly_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace coop::firefly_sync {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolve / registration latches.
void* g_fireflyClass  = nullptr;
void* g_fireflyTickFn = nullptr;
bool  g_hooksRegistered = false;
uint32_t g_resolveN = 0;  // ~1 Hz throttle on the FindClass walk until registered

// Reflected spawn path (resolved once; GUObjectArray entries are stable in shipped UE4).
void* g_effFireflies        = nullptr;  // the eff_fireflies UParticleSystem template
void* g_gameplayStaticsCdo  = nullptr;
void* g_spawnEmitterFn      = nullptr;

// Capture state. PRE snapshots the live ParticleSystemComponent pointers; POST diffs to
// find the one this peer's firefly tick just spawned. Game-thread only, one firefly actor,
// synchronous tick -> a plain global is safe between the PRE and POST observer calls.
std::unordered_set<void*> g_preTickPscs;
bool g_havePreSnapshot = false;
long long g_lastCaptureMs = 0;  // defensive ~1 Hz cap on the capture walk (see PRE)

constexpr const wchar_t* kFireflyClass = L"ticker_fireflySpawner_C";
constexpr const wchar_t* kEffFireflies = L"eff_fireflies";
constexpr const wchar_t* kPscClass     = L"ParticleSystemComponent";

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected();
}

// ---- capture: PRE snapshot, POST diff + broadcast (every connected peer) -----
void OnFireflyTickPre(void* /*self*/, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread()) return;
    g_havePreSnapshot = false;
    if (!Connected()) return;  // nothing to share when solo / not yet connected
    // Defensive: the firefly ReceiveTick is TickInterval=30 s, so this walk is inherently
    // rare. But if a future recook ever made it tick hot, an unbounded FindObjectsByClass
    // (a GUObjectArray walk) here would be the CLAUDE.md per-frame-scan footgun -- so cap
    // the capture to ~1 Hz. Real (30 s) firefly ticks are never blocked.
    const long long now = NowMs();
    if (now - g_lastCaptureMs < 1000) return;
    g_lastCaptureMs = now;
    g_preTickPscs.clear();
    for (void* psc : R::FindObjectsByClass(kPscClass)) g_preTickPscs.insert(psc);
    g_havePreSnapshot = true;
}

void OnFireflyTickPost(void* /*self*/, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread()) return;
    if (!g_havePreSnapshot) return;
    g_havePreSnapshot = false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    // The firefly tick (synchronous, between PRE and POST) spawns at most one eff_fireflies
    // PSC via EX_CallMath -- the only NEW ParticleSystemComponent in this window (most ticks
    // roll false and spawn nothing). Its world location is the unattached component's
    // RelativeLocation (the point SpawnEmitterAtLocation set) -- raw read, no nested
    // UFunction call from inside the observer. Broadcast it: a client -> host, the host fans
    // out to all clients; the host also relays a client's spawn to the OTHER clients
    // (IsClientRelayableReliableKind). The sender never re-spawns its own (it already has
    // the local emitter from its own spawner, and the origin never receives its own send).
    // The firefly tick spawns EXACTLY ONE emitter per successful roll, so broadcast at most
    // one new PSC (the first) and WARN if more than one appeared -- a >1 count means a nested
    // call spawned an unrelated transient PSC (a recook change), and blindly broadcasting it
    // would paint a stray firefly on every other peer. (audit I-3 2026-06-09.)
    int newCount = 0;
    bool sent = false;
    for (void* psc : R::FindObjectsByClass(kPscClass)) {
        if (g_preTickPscs.count(psc)) continue;  // existed pre-tick -> not ours
        ++newCount;
        if (sent) continue;
        ue_wrap::FVector loc = ue_wrap::engine::GetComponentRelativeLocation(psc);
        if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) continue;
        coop::net::FireflySpawnPayload p{ loc.X, loc.Y, loc.Z };
        s->SendReliable(coop::net::ReliableKind::FireflySpawn, &p, sizeof(p));
        UE_LOGI("firefly: broadcast own spawn at (%.0f, %.0f, %.0f)", p.x, p.y, p.z);
        sent = true;
    }
    if (newCount > 1)
        UE_LOGW("firefly: %d new ParticleSystemComponents in one ReceiveTick (expected 1) -- "
                "broadcast the first only; nested-spawn / recook change?", newCount);
    g_preTickPscs.clear();
}

// ---- reflected spawn path resolve ---------------------------------------
bool ResolveSpawn() {
    if (g_effFireflies && g_gameplayStaticsCdo && g_spawnEmitterFn) return true;
    if (!g_effFireflies)
        g_effFireflies = R::FindObject(kEffFireflies, L"ParticleSystem");
    if (!g_gameplayStaticsCdo)
        g_gameplayStaticsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gameplayStaticsCdo && !g_spawnEmitterFn) {
        if (void* cls = R::ClassOf(g_gameplayStaticsCdo))
            g_spawnEmitterFn = R::FindFunction(cls, L"SpawnEmitterAtLocation");
    }
    return g_effFireflies && g_gameplayStaticsCdo && g_spawnEmitterFn;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);

    ResolveSpawn();  // opportunistic (cheap once cached); needed before a packet lands

    if (g_hooksRegistered) return;

    // Throttle the FindClass walk to ~1 Hz until the firefly BP class loads + we register
    // (the class is gamemode-spawned and may be absent for a while). Install() runs every
    // ~125 Hz NetPumpTick.
    if ((g_resolveN++ % 125) != 0) return;

    if (!g_fireflyClass) g_fireflyClass = R::FindClass(kFireflyClass);
    if (!g_fireflyClass) return;  // BP class not loaded yet -- retry next tick.
    if (!g_fireflyTickFn) g_fireflyTickFn = R::FindFunction(g_fireflyClass, L"ReceiveTick");
    if (!g_fireflyTickFn) {
        UE_LOGW("firefly: '%ls' has no ReceiveTick -- CXX dump stale?; skipping", kFireflyClass);
        g_hooksRegistered = true;  // resolved-but-nothing-to-hook: stop retrying.
        return;
    }

    // PRE+POST capture observers on the firefly ReceiveTick (every peer captures its own
    // spawns). Roll back PRE if POST fails so a retry is clean (no duplicate slot).
    if (!GT::RegisterPreObserver(g_fireflyTickFn, &OnFireflyTickPre)) {
        UE_LOGE("firefly: PRE observer registration FAILED (table full?) -- retrying next tick");
        return;
    }
    if (!GT::RegisterPostObserver(g_fireflyTickFn, &OnFireflyTickPost)) {
        UE_LOGE("firefly: POST observer registration FAILED (table full?) -- rolling back, retrying");
        GT::UnregisterObservers(g_fireflyTickFn, &OnFireflyTickPre);
        return;
    }
    g_hooksRegistered = true;
    UE_LOGI("firefly: capture observers installed on %ls::ReceiveTick @ %p "
            "(peer-symmetric; spawn-path resolved=%d)",
            kFireflyClass, g_fireflyTickFn, ResolveSpawn() ? 1 : 0);
}

void OnReliable(const coop::net::FireflySpawnPayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("firefly: OnReliable off-game-thread -- dropping"); return; }
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z)) return;
    if (!ResolveSpawn()) {
        UE_LOGW("firefly: OnReliable spawn path unresolved (eff=%p cdo=%p fn=%p) -- dropping",
                g_effFireflies, g_gameplayStaticsCdo, g_spawnEmitterFn);
        return;
    }
    void* worldCtx = coop::players::Registry::Get().Local();
    if (!worldCtx) return;  // no local pawn yet -> can't resolve the world

    ue_wrap::FVector loc{ payload.x, payload.y, payload.z };
    ue_wrap::FVector scale{ 1.f, 1.f, 1.f };
    ue_wrap::FRotator rot{ 0.f, 0.f, 0.f };
    ue_wrap::ParamFrame f(g_spawnEmitterFn);
    f.Set<void*>(L"WorldContextObject", worldCtx);
    f.Set<void*>(L"EmitterTemplate", g_effFireflies);
    f.SetRaw(L"Location", &loc, sizeof(loc));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);       // EPSCPoolMethod::None
    f.Set<bool>(L"bAutoActivateSystem", true);
    ue_wrap::Call(g_gameplayStaticsCdo, f);
    UE_LOGI("firefly: spawned peer's eff_fireflies at (%.0f, %.0f, %.0f)",
            payload.x, payload.y, payload.z);
}

void OnDisconnect() {
    g_havePreSnapshot = false;
    g_preTickPscs.clear();
    g_session.store(nullptr, std::memory_order_release);
    // Observers stay registered (they self-gate on a connected session). With no session,
    // OnFireflyTickPre early-returns -> a now-solo peer just runs its spawner locally (its
    // own fireflies), exactly like single-player.
}

}  // namespace coop::firefly_sync

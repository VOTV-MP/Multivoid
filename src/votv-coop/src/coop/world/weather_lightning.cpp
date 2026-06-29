// coop/weather_lightning.cpp -- see coop/weather_lightning.h.

#include "coop/world/weather_lightning.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <cstdint>

namespace coop::weather_lightning {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Atomic session pointer -- the POST observer is registered once but fires
// for the session lifetime; on Stop/Start the harness re-stores the new
// pointer via SetSession so the observer's role/connected checks read the
// current session. Acquire/release matches weather_state's g_session pattern.
std::atomic<coop::net::Session*> g_session{nullptr};

// Resolved-once dependencies. Once non-null, stay non-null for the process
// lifetime (GUObjectArray entries don't get unloaded in shipped UE4).
void* g_gameplayStaticsCdo   = nullptr;
void* g_beginDeferredSpawnFn = nullptr;
void* g_finishSpawnFn        = nullptr;
void* g_lightningStrikeClass = nullptr;
int32_t g_spawnActorClassParamOff = -1;
int32_t g_spawnTransformParamOff  = -1;
bool g_observerRegistered = false;

// HOST POST observer on BeginDeferredActorSpawnFromClass. Fires for EVERY
// deferred spawn (also drives NPC spawns / prop spawns); filtered by
// ActorClass == lightningStrike_C. The strike's actor location IS the
// SpawnTransform translation per
// research/findings/votv-weather-RE-effect-actors-2026-05-26.md.
void OnSpawnPostLightning(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;       // defensive
    if (!params) return;
    if (g_spawnActorClassParamOff < 0 || g_spawnTransformParamOff < 0) return;
    if (!g_lightningStrikeClass) return;

    // ActorClass filter -- exact pointer match (lightningStrike_C has no
    // subclasses in VOTV; the dump shows only this one). Pointer compare,
    // no string walk -- fast path safe to run on every spawn.
    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_spawnActorClassParamOff);
    if (actorClass != g_lightningStrikeClass) return;

    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only sender

    // Read SpawnTransform.Translation (16-byte FQuat first, then 12-byte
    // FVector translation -- same layout as npc_sync uses).
    const uint8_t* xform = reinterpret_cast<const uint8_t*>(params) + g_spawnTransformParamOff;
    coop::net::LightningStrikePayload p{};
    // v13 (A4 2026-05-29): host stamps its own local Player Element id.
    // (v14 stamped paired senderContext; v16 PR-FOUNDATION-1b moved
    // stale-gen defense to header senderEpoch.)
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        p.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    p.locX = *reinterpret_cast<const float*>(xform + 0x10);
    p.locY = *reinterpret_cast<const float*>(xform + 0x14);
    p.locZ = *reinterpret_cast<const float*>(xform + 0x18);

    const bool sent = s->SendReliable(
        coop::net::ReliableKind::LightningStrike, &p, sizeof(p));
    if (!sent) {
        UE_LOGW("weather: lightning SendReliable failed (channel busy) -- "
                "strike at (%.0f, %.0f, %.0f) NOT broadcast", p.locX, p.locY, p.locZ);
        return;
    }
    UE_LOGI("weather: host broadcast LightningStrike at (%.0f, %.0f, %.0f)",
            p.locX, p.locY, p.locZ);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool TryResolve() {
    if (!g_gameplayStaticsCdo) {
        g_gameplayStaticsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    }
    if (!g_gameplayStaticsCdo) return false;

    if (!g_beginDeferredSpawnFn || !g_finishSpawnFn) {
        void* gsCls = R::ClassOf(g_gameplayStaticsCdo);
        if (!gsCls) return false;
        if (!g_beginDeferredSpawnFn) {
            g_beginDeferredSpawnFn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
        }
        if (!g_finishSpawnFn) {
            g_finishSpawnFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        }
    }
    if (!g_beginDeferredSpawnFn || !g_finishSpawnFn) return false;

    if (g_spawnActorClassParamOff < 0) {
        g_spawnActorClassParamOff = R::FindParamOffset(g_beginDeferredSpawnFn, L"ActorClass");
    }
    if (g_spawnTransformParamOff < 0) {
        g_spawnTransformParamOff = R::FindParamOffset(g_beginDeferredSpawnFn, L"SpawnTransform");
    }
    if (g_spawnActorClassParamOff < 0 || g_spawnTransformParamOff < 0) return false;

    if (!g_lightningStrikeClass) {
        g_lightningStrikeClass = R::FindClass(P::name::LightningStrikeClass);
    }
    if (!g_lightningStrikeClass) return false;

    return true;
}

bool RegisterHostObserver() {
    if (g_observerRegistered) return true;
    if (!TryResolve()) return false;
    if (!GT::RegisterPostObserver(g_beginDeferredSpawnFn, &OnSpawnPostLightning)) {
        UE_LOGE("weather: lightning POST observer registration FAILED "
                "(observer table full? bump kMaxObservers)");
        return false;
    }
    g_observerRegistered = true;
    UE_LOGI("weather: HOST lightning POST observer registered on %ls @ %p "
            "(ActorClass@%d, SpawnTransform@%d, lightningStrike_C=%p)",
            P::name::BeginDeferredSpawnFn, g_beginDeferredSpawnFn,
            g_spawnActorClassParamOff, g_spawnTransformParamOff,
            g_lightningStrikeClass);
    return true;
}

void OnDisconnect() {
    if (g_observerRegistered && g_beginDeferredSpawnFn) {
        GT::UnregisterObservers(g_beginDeferredSpawnFn, &OnSpawnPostLightning);
        g_observerRegistered = false;
        UE_LOGI("weather: lightning OnDisconnect unregistered POST observer");
    }
    // Clear cached session pointer so a stale Session* can't leak into the
    // next session's observer fires before SetSession is re-called.
    g_session.store(nullptr, std::memory_order_release);
}

void Apply(const coop::net::LightningStrikePayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: lightning Apply off-game-thread -- dropping");
        return;
    }
    // v13 (A4 2026-05-29): the "is sender host?" trust-bound check moved
    // up into event_feed::Update's LightningStrike dispatcher (validates
    // msg.senderPeerSlot == 0 before posting here).
    if (!g_gameplayStaticsCdo || !g_beginDeferredSpawnFn || !g_finishSpawnFn ||
        !g_lightningStrikeClass) {
        UE_LOGW("weather: lightning Apply spawn path not resolved "
                "(cdo=%p begin=%p finish=%p cls=%p) -- dropping",
                g_gameplayStaticsCdo, g_beginDeferredSpawnFn, g_finishSpawnFn,
                g_lightningStrikeClass);
        return;
    }

    // Build FTransform at the received location. FTransform layout:
    //   FQuat Rotation @ 0x00 (16 B, XYZW)
    //   FVector Translation @ 0x10 (12 B)
    //   FVector Scale3D @ 0x20 (12 B; identity = (1,1,1))
    // Identity quat = (0,0,0,1). Strike orientation isn't meaningful (it's
    // a vertical lightning bolt; the actor's mesh orients to world up
    // internally per the RE doc).
    alignas(16) uint8_t xform[48] = {};
    *reinterpret_cast<float*>(xform + 0x0C) = 1.f;   // FQuat.W = 1 (identity)
    *reinterpret_cast<float*>(xform + 0x10) = payload.locX;
    *reinterpret_cast<float*>(xform + 0x14) = payload.locY;
    *reinterpret_cast<float*>(xform + 0x18) = payload.locZ;
    *reinterpret_cast<float*>(xform + 0x20) = 1.f;   // Scale.X
    *reinterpret_cast<float*>(xform + 0x24) = 1.f;   // Scale.Y
    *reinterpret_cast<float*>(xform + 0x28) = 1.f;   // Scale.Z

    ue_wrap::ParamFrame f(g_beginDeferredSpawnFn);
    f.Set<void*>(L"WorldContextObject", coop::players::Registry::Get().Local());
    f.Set<void*>(L"ActorClass", g_lightningStrikeClass);
    f.SetRaw(L"SpawnTransform", xform, sizeof(xform));
    // CollisionHandlingOverride = 1 (AlwaysSpawn). UE4 enum: 0=Undefined
    // (delegates to class default which may be collision-conditional;
    // risks silent drop on geometry edge), 1=AlwaysSpawn, 2-4 conditional.
    // The host already confirmed the spawn at this loc; the client should
    // always materialise it (RULE 1 root-cause: don't let UE4 silently
    // drop our received event).
    f.Set<uint8_t>(L"CollisionHandlingOverride", 1);
    ue_wrap::Call(g_gameplayStaticsCdo, f);
    void* actor = f.Get<void*>(L"ReturnValue");
    if (!actor) {
        UE_LOGW("weather: lightning Apply BeginDeferred returned null "
                "for loc=(%.0f, %.0f, %.0f) -- skipping",
                payload.locX, payload.locY, payload.locZ);
        return;
    }
    // FinishSpawningActor(Actor, SpawnTransform).
    ue_wrap::ParamFrame f2(g_finishSpawnFn);
    f2.Set<void*>(L"Actor", actor);
    f2.SetRaw(L"SpawnTransform", xform, sizeof(xform));
    ue_wrap::Call(g_gameplayStaticsCdo, f2);
    UE_LOGI("weather: lightning Apply spawned lightningStrike_C=%p at "
            "(%.0f, %.0f, %.0f)", actor, payload.locX, payload.locY, payload.locZ);
}

}  // namespace coop::weather_lightning

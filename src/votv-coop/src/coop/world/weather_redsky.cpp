// coop/weather_redsky.cpp -- see coop/weather_redsky.h.

#include "coop/world/weather_redsky.h"

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

namespace coop::weather_redsky {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolved-once dependencies. spawnRedSky is on AmainGamemode_C (resolved
// via the gamemode CDO). redSkyEvent_C is a content BP class that may not
// be loaded until first spawn -- the set UFunction is resolved lazily.
void* g_gamemodeCdo            = nullptr;
void* g_spawnRedSkyFn          = nullptr;
void* g_redSkyEventSetFn       = nullptr;
bool g_observersRegistered = false;

// Receiver-side suppression: when we APPLY a remote red-sky state, the
// local POST observers on spawnRedSky / set would fire + broadcast back.
// The observer's role-gate (host-only) already prevents this on the
// client, but the flag is a belt-and-braces for symmetry with the rain
// path + future N-peer scenarios. Atomic for the same reason as g_session.
std::atomic<bool> g_echoSuppress{false};

void* ResolveSetFn(void* redSkyActor) {
    // Prefer resolving from the actor's runtime class. More reliable than
    // FindClass because BP-content classes are loaded lazily and may not
    // exist in the GUObjectArray before the first spawn.
    if (g_redSkyEventSetFn) return g_redSkyEventSetFn;
    if (!redSkyActor) return nullptr;
    void* cls = R::ClassOf(redSkyActor);
    if (!cls) return nullptr;
    g_redSkyEventSetFn = R::FindFunction(cls, P::name::RedSkyEvent_SetFn);
    if (g_redSkyEventSetFn) {
        UE_LOGI("weather: lazily resolved redSkyEvent.set @ %p (from actor's class %p)",
                g_redSkyEventSetFn, cls);
    }
    return g_redSkyEventSetFn;
}

// HOST POST observer on spawnRedSky -- fires after the gamemode spawns the
// AredSkyEvent_C actor. The act of spawning IS the "red sky ON" signal.
void OnSpawnRedSkyPost(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread()) return;
    if (!self) return;
    if (g_echoSuppress.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;

    coop::net::RedSkyPayload p{};
    // v13 (A4 2026-05-29): host stamps its own local Player Element id.
    // (v14 stamped paired senderContext; v16 PR-FOUNDATION-1b moved
    // stale-gen defense to header senderEpoch.)
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        p.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    p.state = 1;  // spawnRedSky == turning red ON
    const bool sent = s->SendReliable(
        coop::net::ReliableKind::RedSky, &p, sizeof(p));
    if (!sent) {
        UE_LOGW("weather: RedSky ON SendReliable failed (channel busy)");
        return;
    }
    UE_LOGI("weather: host broadcast RedSky state=1 (spawnRedSky observed)");
}

// HOST POST observer on redSkyEvent.set(bool) -- fires for subsequent
// on/off toggles after the actor has been spawned.
void OnRedSkyEventSetPost(void* self, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;
    if (!self || !params) return;
    if (g_echoSuppress.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;

    // Read the `isred` arg from the param frame. ParamFrame layout: the
    // first param is at offset 0 unless aligned (for a single bool the
    // offset IS 0).
    static int32_t sIsredOff = -2;
    if (sIsredOff == -2) {
        sIsredOff = R::FindParamOffset(g_redSkyEventSetFn, L"isred");
    }
    if (sIsredOff < 0) return;
    const bool isred = *reinterpret_cast<const bool*>(
        reinterpret_cast<const uint8_t*>(params) + sIsredOff);

    coop::net::RedSkyPayload p{};
    // v13 (A4 2026-05-29): host stamps its own local Player Element id.
    // (v14 stamped paired senderContext; v16 PR-FOUNDATION-1b moved
    // stale-gen defense to header senderEpoch.)
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        p.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    p.state = isred ? 1 : 0;
    const bool sent = s->SendReliable(
        coop::net::ReliableKind::RedSky, &p, sizeof(p));
    if (!sent) {
        UE_LOGW("weather: RedSky state=%d SendReliable failed", p.state);
        return;
    }
    UE_LOGI("weather: host broadcast RedSky state=%d (set observed)", p.state);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool TryResolve() {
    if (!g_gamemodeCdo) {
        g_gamemodeCdo = R::FindClassDefaultObject(P::name::GamemodeClass);
    }
    if (!g_gamemodeCdo) return false;
    if (!g_spawnRedSkyFn) {
        void* gmCls = R::ClassOf(g_gamemodeCdo);
        if (gmCls) {
            g_spawnRedSkyFn = R::FindFunction(gmCls, P::name::MainGamemode_SpawnRedSkyFn);
        }
    }
    if (!g_spawnRedSkyFn) return false;
    if (!g_redSkyEventSetFn) {
        // redSkyEvent_C is a content BP class; may not be loaded until
        // first spawnRedSky call. Try to resolve, but don't gate the
        // overall install on it -- the host's POST observer on
        // spawnRedSky still fires + broadcasts even before the set fn
        // is resolved, and the receiver lazily resolves on first apply.
        void* cls = R::FindClass(P::name::RedSkyEventClass);
        if (cls) {
            g_redSkyEventSetFn = R::FindFunction(cls, P::name::RedSkyEvent_SetFn);
        }
    }
    return true;
}

bool RegisterHostObservers() {
    if (g_observersRegistered) return true;
    if (!TryResolve() || !g_spawnRedSkyFn) return false;
    int n = 0;
    if (GT::RegisterPostObserver(g_spawnRedSkyFn, &OnSpawnRedSkyPost)) ++n;
    if (g_redSkyEventSetFn) {
        if (GT::RegisterPostObserver(g_redSkyEventSetFn, &OnRedSkyEventSetPost)) ++n;
    }
    g_observersRegistered = (n > 0);
    UE_LOGI("weather: HOST red-sky POST observers registered (%d/2; "
            "spawnRedSky=%p set=%p) -- set fn may resolve later if "
            "redSkyEvent_C class not yet loaded",
            n, g_spawnRedSkyFn, g_redSkyEventSetFn);
    return g_observersRegistered;
}

bool DebugForce(bool red) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: red-sky DebugForce off-game-thread -- wrap in GT::Post");
        return false;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("weather: red-sky DebugForce called on non-host");
        return false;
    }
    if (!TryResolve() || !g_spawnRedSkyFn) {
        UE_LOGW("weather: red-sky DebugForce spawnRedSky UFunction not yet resolved");
        return false;
    }
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) {
        UE_LOGW("weather: red-sky DebugForce no live mainGamemode_C");
        return false;
    }
    // Lookup the existing redSky pointer at @0x0888.
    void* redSky = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky);

    if (red) {
        // Step 1: spawn the AredSkyEvent_C actor if it doesn't exist.
        // spawnRedSky's IDA-dump locals show ONLY the spawn chain
        // (MakeTransform/BeginDeferred/FinishSpawn/IsValid) -- it does
        // NOT call set internally. The actor stays inert (isred=false,
        // color curves unchanged) until we explicitly call set(true).
        if (!redSky) {
            ue_wrap::ParamFrame f(g_spawnRedSkyFn);
            ue_wrap::Call(gm, f);
            UE_LOGI("weather: red-sky DebugForce -- spawnRedSky() called (actor instantiation)");
            // Re-read the pointer (spawnRedSky stores it at @0x0888).
            redSky = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky);
        }
        // Step 2: call set(true) to swap the color curves to the red set.
        if (redSky && R::IsLive(redSky)) {
            void* setFn = ResolveSetFn(redSky);
            if (setFn) {
                ue_wrap::ParamFrame f(setFn);
                f.Set<bool>(L"isred", true);
                ue_wrap::Call(redSky, f);
                UE_LOGI("weather: red-sky DebugForce -- redSky.set(true) called -- "
                        "color curves should swap to red set");
            } else {
                UE_LOGW("weather: red-sky DebugForce -- set UFunction unresolvable");
                return false;
            }
        } else {
            UE_LOGW("weather: red-sky DebugForce -- spawn produced no live actor (gm.redSky=%p)", redSky);
            return false;
        }
    } else {
        // OFF: revert via set(false). No-op if the actor never existed.
        if (redSky && R::IsLive(redSky)) {
            void* setFn = ResolveSetFn(redSky);
            if (setFn) {
                ue_wrap::ParamFrame f(setFn);
                f.Set<bool>(L"isred", false);
                ue_wrap::Call(redSky, f);
                UE_LOGI("weather: red-sky DebugForce -- redSky.set(false) called -- color curves revert");
            }
        } else {
            UE_LOGI("weather: red-sky DebugForce red=false but no live redSky actor -- nothing to revert");
        }
    }
    return true;
}

void Apply(const coop::net::RedSkyPayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: red-sky Apply off-game-thread -- dropping");
        return;
    }
    // v13 (A4 2026-05-29): the "is sender host?" trust-bound check moved
    // up into event_feed::Update's RedSky dispatcher (validates
    // msg.senderPeerSlot == 0 before posting here).
    if (!TryResolve() || !g_spawnRedSkyFn) {
        UE_LOGW("weather: red-sky Apply spawnRedSky UFunction not yet resolved -- dropping");
        return;
    }
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) {
        UE_LOGW("weather: red-sky Apply no live mainGamemode_C -- dropping");
        return;
    }
    const bool wantRed = (payload.state != 0);
    void* redSky = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky);

    // Echo-suppress: while the receiver invokes spawnRedSky / set, the
    // local POST observer fires + would broadcast back to the host. The
    // role gate on the observer already prevents this on the client (role
    // != host), but the suppress flag is a belt-and-braces for symmetry
    // with the rain path + future N-peer scenarios where receiver might
    // also be the host.
    g_echoSuppress.store(true, std::memory_order_release);

    if (wantRed) {
        // Spawn the actor if absent.
        if (!redSky) {
            ue_wrap::ParamFrame f(g_spawnRedSkyFn);
            ue_wrap::Call(gm, f);
            UE_LOGI("weather: red-sky Apply -- spawnRedSky() called (instantiating actor)");
            redSky = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky);
        }
        // Call set(true) to swap the color curves to red. spawnRedSky
        // alone doesn't apply the effect (IDA RE confirms its body is
        // pure SpawnActor with no set call).
        if (redSky && R::IsLive(redSky)) {
            void* setFn = ResolveSetFn(redSky);
            if (setFn) {
                ue_wrap::ParamFrame f(setFn);
                f.Set<bool>(L"isred", true);
                ue_wrap::Call(redSky, f);
                UE_LOGI("weather: red-sky Apply -- redSky.set(true) called");
            } else {
                UE_LOGW("weather: red-sky Apply -- set UFunction unresolvable; color curves NOT applied");
            }
        }
    } else {
        if (redSky && R::IsLive(redSky)) {
            void* setFn = ResolveSetFn(redSky);
            if (setFn) {
                ue_wrap::ParamFrame f(setFn);
                f.Set<bool>(L"isred", false);
                ue_wrap::Call(redSky, f);
                UE_LOGI("weather: red-sky Apply -- redSky.set(false) called");
            }
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);
}

void OnDisconnect() {
    if (g_observersRegistered) {
        if (g_spawnRedSkyFn)    GT::UnregisterObservers(g_spawnRedSkyFn, &OnSpawnRedSkyPost);
        if (g_redSkyEventSetFn) GT::UnregisterObservers(g_redSkyEventSetFn, &OnRedSkyEventSetPost);
        g_observersRegistered = false;
        UE_LOGI("weather: red-sky OnDisconnect unregistered POST observers");
    }
    g_echoSuppress.store(false, std::memory_order_release);
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::weather_redsky

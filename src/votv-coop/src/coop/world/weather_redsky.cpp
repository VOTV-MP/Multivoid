// coop/weather_redsky.cpp -- see coop/weather_redsky.h.

#include "coop/world/weather_redsky.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
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

// Receiver-side suppression: while we APPLY a remote red-sky state, the
// mirror spawn must pass coop/weather_event_births' client birth-catch
// (which destroys any UNCOMMANDED redSkyEvent_C birth -- the organic 1%
// newDay roll). Atomic for the same reason as g_session.
std::atomic<bool> g_echoSuppress{false};

// HOST poll state (game-thread only). The 2026-08-29 reroot: the organic
// spawnRedSky caller is EX_LocalVirtualFunction (PE-invisible; measured in
// daynightCycle's newDay bytecode), so edge detection is FIELD-LEVEL like
// weather_fog -- gamemode.redSky liveness + its `isred` bool, on a throttle.
ue_wrap::CachedObjRef g_gamemodeRef;         // world-stamped live gamemode cache
long long g_lastGmResolveMs = 0;             // FindObjectByClass walk throttle (5 s)
long long g_lastPollMs      = 0;             // edge-poll throttle (500 ms)
int       g_lastPolledState = -1;            // -1 = never sampled (no edge on first read)
int32_t   g_isredOff        = -1;            // redSkyEvent_C `isred` offset (lazy)

long long NowMsSteady() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The live gamemode actor, cached + world-stamped; a FindObjectByClass walk
// at most once per 5 s when the cache is dead (the weather_fog super-fog
// cadence -- never a per-poll walk).
void* ResolveGamemodeActor() {
    if (void* gm = g_gamemodeRef.Get()) return gm;
    const long long now = NowMsSteady();
    if (now - g_lastGmResolveMs < 5000) return nullptr;
    g_lastGmResolveMs = now;
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (gm && R::IsLive(gm)) {
        g_gamemodeRef.Set(gm);
        return gm;
    }
    return nullptr;
}

// Read the CURRENT red-sky truth from a live gamemode: actor live && isred.
// The isred offset resolves lazily from the ACTOR's runtime class (the class
// may not exist before the first spawn). Unknown offset while an actor is
// live reads as OFF until the offset resolves (next poll).
bool ReadRedSkyActive(void* gm) {
    void* redSky = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky);
    if (!redSky || !R::IsLive(redSky)) return false;
    if (g_isredOff < 0) {
        void* cls = R::ClassOf(redSky);
        if (cls) g_isredOff = R::FindPropertyOffset(cls, L"isred");
        if (g_isredOff >= 0)
            UE_LOGI("weather: red-sky isred offset resolved @ +0x%X", g_isredOff);
    }
    if (g_isredOff < 0) return false;
    return *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(redSky) + g_isredOff);
}

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

// Broadcast the host's red-sky state. (The 2026-05 POST observers on
// spawnRedSky/set that used to author this send were RETIRED 2026-08-29,
// RULE 2: the organic caller is EX_LocalVirtualFunction, PE-invisible, so
// they fired only for our own reflected Calls -- zero organic broadcasts in
// every log on disk. The HostPollEdge field poll below is the ONE detector.)
bool SendState(coop::net::Session* s, int state) {
    coop::net::RedSkyPayload p{};
    // v13 (A4 2026-05-29): host stamps its own local Player Element id.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        p.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    p.state = static_cast<uint8_t>(state);
    const bool sent = s->SendReliable(
        coop::net::ReliableKind::RedSky, &p, sizeof(p));
    if (!sent) UE_LOGW("weather: RedSky state=%d SendReliable failed", state);
    return sent;
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

bool LocalRedSkyActive() {
    if (!GT::IsGameThread()) return false;
    void* gm = ResolveGamemodeActor();
    if (!gm) return false;
    return ReadRedSkyActive(gm);
}

bool ApplyEchoActive() {
    return g_echoSuppress.load(std::memory_order_acquire);
}

void HostPollEdge() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const long long now = NowMsSteady();
    if (now - g_lastPollMs < 500) return;
    g_lastPollMs = now;
    void* gm = ResolveGamemodeActor();
    if (!gm) return;
    const int state = ReadRedSkyActive(gm) ? 1 : 0;
    if (g_lastPolledState == -1) {
        // First sample of the session: broadcast only if ALREADY red (the
        // clients seeded before this poll armed still need the ON), stay
        // silent on the common already-clear world.
        g_lastPolledState = state;
        if (state == 1 && SendState(s, 1))
            UE_LOGI("weather: host broadcast RedSky state=1 (first poll found an active red sky)");
        return;
    }
    if (state == g_lastPolledState) return;
    g_lastPolledState = state;
    if (SendState(s, state))
        UE_LOGI("weather: host broadcast RedSky state=%d (field-poll edge)", state);
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
            // Full OFF mirror (2026-08-29): destroy the local actor + clear
            // the gamemode slot, so "no red sky" is structural, not a dormant
            // actor -- and the next ON re-spawns cleanly through the branch
            // above. ReceiveDestroyed runs the actor's own teardown.
            ue_wrap::engine::DestroyActor(redSky);
            *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(gm) + P::off::AmainGamemode_redSky) = nullptr;
            UE_LOGI("weather: red-sky Apply -- local redSkyEvent actor destroyed (OFF mirror)");
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);
}

void OnDisconnect() {
    // (The 2026-05 POST-observer unregister retired with the observers, RULE 2.)
    g_echoSuppress.store(false, std::memory_order_release);
    g_session.store(nullptr, std::memory_order_release);
    g_lastPolledState = -1;   // next session re-learns; first poll re-seeds an active red
    g_lastPollMs      = 0;
}

}  // namespace coop::weather_redsky

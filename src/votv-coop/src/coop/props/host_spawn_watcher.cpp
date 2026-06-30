// coop/host_spawn_watcher.cpp -- see coop/host_spawn_watcher.h.

#include "coop/props/host_spawn_watcher.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"      // ExpressSpawnedProp (reuse the keyed broadcast)
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "ue_wrap/game_thread.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"            // IsDescendantOfProp (the keyed-prop gate for the Q-menu seam)
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"       // FTransform (the reflected SpawnTransform param layout)

#include <atomic>
#include <cmath>          // asin/atan2 for the FQuat -> FRotator conversion
#include <cstdint>
#include <string>
#include <vector>

namespace coop::host_spawn_watcher {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// Atomic session pointer -- the POST observer is registered once but fires for
// the session lifetime; the harness re-stores the (boot-lifetime) pointer via
// Install/SetSession. Acquire/release mirrors weather_lightning's g_session.
std::atomic<coop::net::Session*> g_session{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// Resolved-once dependencies (GUObjectArray entries don't unload in shipped UE4).
void*   g_beginDeferredFn    = nullptr;
int32_t g_classParamOff      = -1;   // ActorClass UClass* param
int32_t g_returnParamOff     = -1;   // ReturnValue AActor* param
int32_t g_xformParamOff      = -1;   // SpawnTransform param -- THE spawn pose (the
                                     // actor isn't positioned yet at BeginDeferred POST)
void*   g_finishSpawnFn      = nullptr;  // FinishSpawningActor -- the KEYED Q-menu/sandbox seam
int32_t g_finishReturnOff    = -1;       // FinishSpawningActor ReturnValue (the finished actor)
bool    g_observerRegistered = false;  // also the permanent give-up latch
bool    g_classesResolved    = false;

// The ambient-prop class set (pineconeSpawner outputs), resolved from
// kAmbientPropSpawnMirrorClasses. Exact-pointer matched -- these are leaf
// classes with no in-scope subclasses, so a hierarchy walk is unnecessary.
void* g_propClasses[ue_wrap::profile::name::kAmbientPropSpawnMirrorClassesSize] = {};

bool IsMirroredPropClass(void* cls) {
    if (!cls) return false;
    for (void* c : g_propClasses) {
        if (c && c == cls) return true;
    }
    return false;
}

// Death-watch for the mirrored transient props. A pinecone self-expires via
// SetLifeSpan(600) (engine Destroy; the spawner-spawned actor has no observable
// K2_DestroyActor) -- so we liveness-check each broadcast prop and emit
// PropDestroy(eid) the tick its actor dies. GAME-THREAD ONLY (net-pump tick) --
// guarded by OnSpawnPost's IsGameThread gate on the producer side.
struct WatchedProp {
    void*    actor       = nullptr;
    int32_t  internalIdx = -1;   // captured live -> IsLiveByIndex without deref
    uint32_t eid         = 0;    // PropDestroy identity (key=None)
};
std::vector<WatchedProp> g_watched;
// Cap is a runaway backstop. Pinecones spawn on a random multi-second interval
// and expire in ~600 s, so only a handful are live at once; 256 is ample. A
// shed entry silently loses that prop's despawn -> WARN on overflow.
constexpr size_t kMaxWatched = 256;

void WatchSpawnedProp(void* actor, uint32_t eid) {
    if (!actor || eid == 0 || eid == coop::element::kInvalidId) return;
    const int32_t idx = R::InternalIndexOf(actor);
    if (idx < 0) return;
    for (auto& w : g_watched) {
        if (w.eid == eid) { w.actor = actor; w.internalIdx = idx; return; }
    }
    if (g_watched.size() >= kMaxWatched) {
        UE_LOGW("host_spawn_watcher: watch cap %zu hit -- shedding eid=%u (its despawn is lost)",
                kMaxWatched, g_watched.front().eid);
        g_watched.erase(g_watched.begin());
    }
    g_watched.push_back(WatchedProp{actor, idx, eid});
}

// HOST POST observer on BeginDeferredActorSpawnFromClass. Fires for EVERY
// deferred spawn (NPCs, lightning, every prop) -- filtered FAST by exact
// class-pointer match against the small ambient-prop set before any actor deref.
//
// GAME-THREAD GATE: actor spawning (UWorld::SpawnActor) is game-thread-only in
// UE4, so a BeginDeferred POST is always on the game thread -- the guard never
// misses a real spawn, and it lets us safely call ProcessEvent (GetActorLocation
// etc.) + mutate the game-thread-only watch list. (Contrast npc_sync's POST,
// which reads only params and so tolerates the parallel-anim worker dispatch.)
void OnSpawnPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;
    if (!params || g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) return;

    // Host-only broadcaster: a client or an idle/disconnected host bails BEFORE any class-hierarchy walk
    // (just a cheap atomic-acquire load + an enum compare). This keeps the per-spawn cost (the trash-class
    // WalksToBase pair below) entirely off non-host peers -- they spawn just as many actors (audit 2026-06-21).
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;

    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_classParamOff);
    // The ambient-prop mirror (pinecone/stick/crystal spawner output -> keyless PropSpawn). Fast-reject
    // everything else by exact class-pointer compare before any actor deref. (chipPile/clump grabs do NOT
    // come through here: their BeginDeferred is EX_CallMath-dispatched -> invisible to this ProcessEvent
    // hook -- they sync via the InpActEvt PRE + held-edge seams in trash_collect_sync/trash_channel/
    // local_streams instead. docs/piles/08; 2026-06-21 RE/hands-on.)
    if (!IsMirroredPropClass(actorClass)) return;  // fast reject (exact pointer compares)

    void* actor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_returnParamOff);
    if (!actor || !R::IsLive(actor)) return;               // spawn failed / dead

    // Dedupe: if this actor is already a tracked Prop Element (an unexpected
    // overlap with the keyed Init-observer path, or a double POST) skip -- never
    // broadcast two eids for one actor. The MarkProcessedInit below also makes a
    // later (believed-unreachable) Init POST on this actor no-op via HasProcessedInit.
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return;

    const std::wstring cls = R::ClassNameOf(actor);

    // Mint the keyless Prop Element (key=None -> eid is the cross-peer identity,
    // the trash-clump precedent) + the Init dedupe latch. MarkProcessedInit
    // here is INTENTIONAL double-broadcast prevention: these classes' Init is
    // BP-internal (never fires our prop_lifecycle Init-POST observer), so it is
    // a no-op today; if a BP recook ever made one Init-observable, this latch
    // makes the Init-POST skip (M2 already broadcast it) rather than double-send.
    PT::MarkProcessedInit(actor);
    PT::MarkPropElement(actor, L"", cls);
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(actor);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("host_spawn_watcher: MarkPropElement gave kInvalidId for '%ls' -- "
                "skipping PropSpawn (Registry full?)", cls.c_str());
        return;
    }

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    p.key.len      = 0;   // key=None: the eid is the identity
    p.propName.len = 0;   // these classes carry their own mesh (no list_props row)

    // Read the SpawnTransform PARAM -- NOT GetActorLocation(actor). At
    // BeginDeferred POST the actor EXISTS but is NOT yet positioned: the
    // SpawnTransform is applied by the later FinishSpawningActor (the deferred
    // two-step), so the actor sits at the origin until then (smoke 2026-06-11:
    // GetActorLocation returned (0,0,0) here, mirroring the prop to world-origin).
    // ue_wrap::FTransform IS the reflected param layout (FQuat@0x00, Translation
    // @0x10, Scale3D@0x20 -- 48B aligned, static_assert'd) -- the type-safe cast
    // avoids hand-offset slips (Scale3D is @0x20, NOT @0x1C which is _padT). Pure
    // reads -> thread-safe (the npc_sync / weather_lightning param-read shape).
    const auto* xt = reinterpret_cast<const ue_wrap::FTransform*>(
        reinterpret_cast<const uint8_t*>(params) + g_xformParamOff);
    p.locX   = xt->TX; p.locY = xt->TY; p.locZ = xt->TZ;
    p.scaleX = xt->SX; p.scaleY = xt->SY; p.scaleZ = xt->SZ;
    // FQuat -> FRotator (degrees) -- the standard UE4 conversion (npc_sync shape).
    const float qx = xt->RotX, qy = xt->RotY, qz = xt->RotZ, qw = xt->RotW;
    const float sinp  = 2.f * (qw * qy - qz * qx);
    const float sinpc = sinp > 1.f ? 1.f : (sinp < -1.f ? -1.f : sinp);
    constexpr float kRadToDeg = 57.29577951308232f;
    p.rotPitch = std::asin(sinpc) * kRadToDeg;
    p.rotYaw   = std::atan2(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qy * qy + qz * qz)) * kRadToDeg;
    p.rotRoll  = std::atan2(2.f * (qw * qx + qy * qz), 1.f - 2.f * (qx * qx + qy * qy)) * kRadToDeg;
    p.chipType = 0;       // not trash
    // SP-parity: spawn the mirror SIMULATING so it drops under the CLIENT's own
    // physics (local fall physics is intentionally NOT synced). No heavy/frozen/
    // static/sleep reads -- the spawner output is a fresh falling prop, and those
    // Aprop_C bools may not be init'd yet at BeginDeferred POST (pre-FinishSpawn).
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    p.elementId = static_cast<uint32_t>(eid);

    if (s->SendPropSpawn(p)) {
        UE_LOGI("host_spawn_watcher: MIRROR ambient spawn cls='%ls' eid=%u at (%.0f,%.0f,%.0f) "
                "-- client spawns + drops it under local physics",
                cls.c_str(), p.elementId, p.locX, p.locY, p.locZ);
    } else {
        UE_LOGW("host_spawn_watcher: SendPropSpawn failed for '%ls' eid=%u (channel busy?) -- "
                "client missed this spawn; despawn-watch still armed", cls.c_str(), p.elementId);
    }
    // Self-claim (an open connect-snapshot bracket must not sweep our fresh prop)
    // + enroll the death-watch for the SetLifeSpan-expiry / consumption despawn.
    coop::join_membership_sweep::RecordClaimIfTracking(actor);
    WatchSpawnedProp(actor, p.elementId);
}

// HOST POST observer on FinishSpawningActor -- the KEYED sandbox-spawn seam.
// The sandbox Q-menu / toolgun spawn a KEYED Aprop_C via BeginDeferred +
// FinishSpawningActor, but the prop's own init() (which MINTS the Key) is
// dispatched EX_LocalVirtualFunction from its UCS -> BP-internal -> never fires
// prop_lifecycle's Aprop_C::Init POST observer (the gap, RE 2026-06-11). The Key
// is set by the time FinishSpawningActor returns, so we observe HERE (not at
// BeginDeferred, where the Key is still None) and run prop_lifecycle's canonical
// KEYED broadcast. The ambient pinecone/stick/crystal set is handled KEYLESS by
// OnSpawnPost above, which MarkProcessedInit's them first -> ExpressSpawnedProp's
// shared HasProcessedInit latch dedupes those here (so they stay keyless). Fires
// per FinishSpawningActor post-connect; the IsDescendantOfProp gate (a cheap
// SuperStruct walk) rejects NPCs/effects before the heavier ExpressSpawnedProp.
void OnFinishSpawnPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;
    if (!params || g_finishReturnOff < 0) return;
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;        // host-only broadcaster
    void* actor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_finishReturnOff);
    if (!actor || !R::IsLive(actor)) return;
    if (!ue_wrap::prop::IsDescendantOfProp(actor)) return;  // KEYED Aprop_C lineage only
    // Reuse prop_lifecycle's canonical keyed broadcast (filters + HasProcessedInit
    // dedupe + keyed payload + send + self-claim). It self-gates on the Key (a
    // still-None Aprop_C falls through harmlessly), so no extra checks here.
    coop::prop_lifecycle::ExpressSpawnedProp(actor);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_observerRegistered) return;

    // Throttle the GUObjectArray walks below to ~1 Hz of the ~125 Hz pump while
    // unresolved (FindClass/FindFunction walk + alloc per entry).
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }

    if (!g_beginDeferredFn) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        if (!gsCls) { s_retry = 60; return; }
        g_beginDeferredFn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
        if (!g_beginDeferredFn) {
            UE_LOGW("host_spawn_watcher: %ls.%ls not found -- disabled for process lifetime",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_observerRegistered = true;  // permanent give-up latch (don't re-walk)
            return;
        }
        g_classParamOff  = R::FindParamOffset(g_beginDeferredFn, L"ActorClass");
        g_returnParamOff = R::FindParamOffset(g_beginDeferredFn, L"ReturnValue");
        g_xformParamOff  = R::FindParamOffset(g_beginDeferredFn, L"SpawnTransform");
        if (g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) {
            UE_LOGW("host_spawn_watcher: BeginDeferred ActorClass@%d ReturnValue@%d SpawnTransform@%d not found -- disabled",
                    g_classParamOff, g_returnParamOff, g_xformParamOff);
            g_observerRegistered = true;
            return;
        }
        // FinishSpawningActor -- the KEYED sandbox-spawn seam (same GameplayStatics
        // class). NON-FATAL if missing: the keyless pinecone detector (BeginDeferred)
        // is independent and still installs.
        g_finishSpawnFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        if (g_finishSpawnFn) {
            g_finishReturnOff = R::FindParamOffset(g_finishSpawnFn, L"ReturnValue");
            if (g_finishReturnOff < 0) {
                UE_LOGW("host_spawn_watcher: FinishSpawningActor ReturnValue param not found -- Q-menu keyed sync disabled");
                g_finishSpawnFn = nullptr;
            }
        } else {
            UE_LOGW("host_spawn_watcher: FinishSpawningActor UFunction not found -- Q-menu keyed sync disabled");
        }
    }

    // Resolve the ambient-prop classes. Partial OK (unresolved slots stay null +
    // never match); retry until all bind (they load on gameplay-level entry).
    if (!g_classesResolved) {
        size_t resolved = 0;
        for (size_t i = 0; i < P::name::kAmbientPropSpawnMirrorClassesSize; ++i) {
            if (!g_propClasses[i])
                g_propClasses[i] = R::FindClass(P::name::kAmbientPropSpawnMirrorClasses[i]);
            if (g_propClasses[i]) ++resolved;
        }
        if (resolved < P::name::kAmbientPropSpawnMirrorClassesSize) {
            s_retry = 60;
            return;  // wait for the full set before going live (no partial coverage)
        }
        g_classesResolved = true;
    }

    if (!GT::RegisterPostObserver(g_beginDeferredFn, &OnSpawnPost)) {
        UE_LOGE("host_spawn_watcher: RegisterPostObserver FAILED (observer table full?) -- disabled");
        g_observerRegistered = true;  // give up (don't retry a full table forever)
        return;
    }
    // KEYED sandbox-spawn observer (FinishSpawningActor POST) -- NON-FATAL: the
    // keyless pinecone detector above is independent, so a failure here leaves it
    // working. Catches the Q-menu/toolgun keyed spawns prop_lifecycle's Init-POST
    // observer misses (their init() is BP-internal).
    if (g_finishSpawnFn && g_finishReturnOff >= 0) {
        if (GT::RegisterPostObserver(g_finishSpawnFn, &OnFinishSpawnPost)) {
            UE_LOGI("host_spawn_watcher: FinishSpawningActor POST observer registered "
                    "(sandbox Q-menu/toolgun KEYED prop mirror, ReturnValue@%d)", g_finishReturnOff);
        } else {
            UE_LOGW("host_spawn_watcher: FinishSpawningActor POST register FAILED (table full?) "
                    "-- Q-menu keyed sync off (pinecone keyless detector still active)");
        }
    }
    g_observerRegistered = true;
    UE_LOGI("host_spawn_watcher: POST observer registered on %ls @ %p "
            "(ActorClass@%d, ReturnValue@%d, %zu ambient-prop classes resolved)",
            P::name::BeginDeferredSpawnFn, g_beginDeferredFn,
            g_classParamOff, g_returnParamOff,
            P::name::kAmbientPropSpawnMirrorClassesSize);
}

void TickWatchedProps(coop::net::Session* s) {
    // Mutates the game-thread-only g_watched + calls SendPropDestroy. Caller
    // (net_pump::Tick) is GT, but assert locally so a future off-GT call site
    // is caught at the boundary (mirrors remote_prop::OnDestroy).
    UE_ASSERT_GAME_THREAD("host_spawn_watcher::TickWatchedProps");
    if (!s || !s->connected() || g_watched.empty()) return;
    if (s->role() != coop::net::Role::Host) return;
    for (size_t i = 0; i < g_watched.size();) {
        WatchedProp& w = g_watched[i];
        if (R::IsLiveByIndex(w.actor, w.internalIdx)) { ++i; continue; }
        // The actor died (SetLifeSpan expiry / consumed) -> despawn the mirror.
        coop::net::PropDestroyPayload dp{};
        dp.key.len   = 0;        // key=None: eid-only
        dp.elementId = w.eid;
        s->SendPropDestroy(dp);
        UE_LOGI("host_spawn_watcher: watched ambient prop eid=%u died -> PropDestroy (despawn mirror)",
                w.eid);
        g_watched.erase(g_watched.begin() + i);
    }
}

void OnDisconnect() {
    g_watched.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::host_spawn_watcher

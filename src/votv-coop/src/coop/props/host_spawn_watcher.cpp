// coop/host_spawn_watcher.cpp -- see coop/host_spawn_watcher.h.

#include "coop/props/host_spawn_watcher.h"

#include "coop/creatures/kerfur_convert.h"  // NoteFreshKerfurNpcSpawn (destroy-edge first-refusal stamp, take-9)
#include "coop/element/element.h"
#include "coop/element/registry.h"          // EidForActor (drain: tracked/mirror exclusion)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"          // LocalHandActor (drain: hotbar view-actor exclusion)
#include "coop/props/prop_echo_suppress.h"  // PeekIncomingSpawn (mirror-spawn exclusion)
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
#include "ue_wrap/ufunction_hook.h"  // FinishSpawningActor Func patch (v106 keyed spawn seam)

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

// OWNER POST observer on BeginDeferredActorSpawnFromClass (peer-SYMMETRIC
// since 2026-07-10: the ambient set is OWNER-EFFECT -- pineconeSpawner anchors
// at the LOCAL player's camera, so each peer rolls its own forest drops and
// broadcasts them; [[feedback-owner-effect-rule]], the firefly_sync authority
// shape on the PropSpawn pipeline). Fires for EVERY deferred spawn (NPCs,
// lightning, every prop) -- filtered FAST by exact class-pointer match against
// the small ambient-prop set before any actor deref.
//
// GAME-THREAD GATE: actor spawning (UWorld::SpawnActor) is game-thread-only in
// UE4, so a BeginDeferred POST is always on the game thread -- the guard never
// misses a real spawn, and it lets us safely call ProcessEvent (GetActorLocation
// etc.) + mutate the game-thread-only watch list. (Contrast npc_sync's POST,
// which reads only params and so tolerates the parallel-anim worker dispatch.)
void OnSpawnPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;
    if (!params || g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) return;

    // Any connected peer broadcasts its OWN ambient spawns (owner-effect). The
    // mirror-scope check kills the echo: a wire mirror is spawned through this
    // same UFunction by remote_prop_spawn, and this POST fires INSIDE that call
    // -- before the actor exists to carry a MarkIncomingSpawn mark.
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (coop::prop_echo_suppress::InMirrorSpawnScope()) return;

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

// THE KEYED SPAWN SEAM (v106, 2026-07-07) -- a UFunction::Func patch on
// FinishSpawningActor. The old ProcessEvent POST observer here caught only the
// PE-dispatched route (sandbox Q-menu / toolgun); the R-drop / quick-slot place
// spawn runs propInventory::takeObj's EX_CallMath FinishSpawningActor -- INVISIBLE
// to a PE observer (log-proven 2026-07-07 10:15: drops waited the 20s safety
// census). Func funnels EVERY dispatch route (the pile-thunk precedent), so the
// Func patch strictly supersedes the PE observer (RULE 2: replaced).
//
// The callback only ENQUEUES: at Finish-return inside takeObj the Key is still
// the placeholder NewGuid (loadData restores the saved Key AFTER Finish -- the
// takeObj PRE/POST bracket exists for exactly this), and updateHold's HELD view
// actor has not yet been written to holding_actor (the hand exclusion below
// needs it). DrainPendingSpawns (next net-pump tick) adopts once the whole BP
// call has completed. One express attempt per entry (ExpressSpawnedProp latches
// ProcessedInit on first touch); a still-unkeyed actor falls to the safety census.
struct PendingFinishedSpawn {
    void*   actor = nullptr;
    int32_t idx   = -1;
};
std::vector<PendingFinishedSpawn> g_pendingFinished;  // GT-only
constexpr size_t kMaxPendingFinished = 128;           // runaway backstop (level-load bursts)

void OnFinishSpawnFunc(void* /*context*/, void* /*srcObj*/, void* result) {
    if (!GT::IsGameThread()) return;
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    // (take-9) BOTH-ROLES kerfur stamp, BEFORE the host gate: the destroy-edge kerfur first
    // refusal needs "this NPC was FinishSpawningActor'd moments ago" on the CLIENT too (its own
    // conversion verb's ghost). Self-gating + pointer-cheap for every non-kerfur actor.
    coop::kerfur_convert::NoteFreshKerfurNpcSpawn(result);
    if (s->role() != coop::net::Role::Host) return;        // host-only broadcaster
    void* actor = result;
    if (!actor || !R::IsLive(actor)) return;
    // Wire/display mirror spawns are marked (MarkIncomingSpawn) BEFORE Finish --
    // PEEK (non-destructive: the Init-POST observer owns consuming the mark) and
    // never enqueue them: expressing a mirror as a fresh world prop is the dupe.
    if (coop::prop_echo_suppress::PeekIncomingSpawn(actor)) return;
    if (!ue_wrap::prop::IsDescendantOfProp(actor)) return;  // KEYED Aprop_C lineage only
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return;  // already tracked
    if (g_pendingFinished.size() >= kMaxPendingFinished) {
        UE_LOGW("host_spawn_watcher: pending-finished cap %zu hit -- dropping %p (safety census catches it)",
                kMaxPendingFinished, actor);
        return;
    }
    g_pendingFinished.push_back(PendingFinishedSpawn{actor, R::InternalIndexOf(actor)});
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
    // KEYED spawn seam (FinishSpawningActor Func patch, v106) -- NON-FATAL: the
    // keyless pinecone detector above is independent, so a failure here leaves it
    // working. Catches EVERY dispatch route: the Q-menu/toolgun PE route the old
    // observer covered AND the EX_CallMath takeObj route (R-drop / quick-slot
    // place / UI inventory drop) it could never see.
    if (g_finishSpawnFn) {
        if (ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawnFunc)) {
            UE_LOGI("host_spawn_watcher: FinishSpawningActor Func-patched "
                    "(keyed spawn seam, all dispatch routes -- drop/place spawns adopt next tick)");
        } else {
            UE_LOGW("host_spawn_watcher: FinishSpawningActor Func patch FAILED (table full?) "
                    "-- keyed spawn seam off (pinecone keyless detector still active)");
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
    // Peer-symmetric (owner-effect 2026-07-10): each peer death-watches the
    // ambient props IT broadcast (g_watched only ever holds own spawns).
    UE_ASSERT_GAME_THREAD("host_spawn_watcher::TickWatchedProps");
    if (!s || !s->connected() || g_watched.empty()) return;
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

void DrainPendingSpawns(coop::net::Session* s) {
    UE_ASSERT_GAME_THREAD("host_spawn_watcher::DrainPendingSpawns");
    if (g_pendingFinished.empty()) return;
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) {
        g_pendingFinished.clear();
        return;
    }
    for (const PendingFinishedSpawn& e : g_pendingFinished) {
        if (!e.actor || !R::IsLiveByIndex(e.actor, e.idx)) continue;   // died before adopt
        // Tracked OR mirror-bound since enqueue (the unified Registry reverse
        // covers both local elements and wire mirrors) -> someone owns it.
        if (coop::element::Registry::Get().EidForActor(e.actor) != coop::element::kInvalidId)
            continue;
        // A late echo mark (a mirror finished after our enqueue check) -> not ours.
        if (coop::prop_echo_suppress::PeekIncomingSpawn(e.actor)) continue;
        // The local hotbar HAND actor (updateHold's view spawn): display-only while
        // held -- the hand-edge in coop/player/hand_item expresses it if/when it is
        // RELEASED into the world. Never adopt it here (the v105 dupe class).
        if (e.actor == coop::hand_item::LocalHandActor()) continue;
        // One-shot canonical express (filters + key gate + payload + self-claim).
        // ExpressSpawnedProp latches ProcessedInit on first touch, so this entry
        // is spent regardless of outcome; a still-unkeyed straggler falls to the
        // periodic safety census.
        coop::prop_lifecycle::ExpressSpawnedProp(e.actor);
        if (PT::GetPropElementIdForActor(e.actor) != coop::element::kInvalidId) {
            UE_LOGI("host_spawn_watcher: spawn-seam adopted %p eid=%u (FinishSpawningActor "
                    "Func seam -- drop/place visible to peers this tick)",
                    e.actor,
                    static_cast<unsigned>(PT::GetPropElementIdForActor(e.actor)));
        }
    }
    g_pendingFinished.clear();
}

void OnDisconnect() {
    g_watched.clear();
    g_pendingFinished.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::host_spawn_watcher

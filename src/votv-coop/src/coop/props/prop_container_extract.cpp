// coop/props/prop_container_extract.cpp -- the propInventory_C::takeObj container-extract seam of
// coop::prop_lifecycle (see coop/props/prop_lifecycle.h): the PRE/POST observer pair, the in-flight
// bracket and InstallInventory.
//
// An Aprop extracted from a container spawns INSIDE takeObj, before loadData has restored its saved
// Key, so the nested Aprop_C::Init POST observer in prop_lifecycle.cpp must defer its broadcast and
// let the takeObj POST here be the canonical broadcaster. g_takeObjInFlight, defined here and
// shared through prop_lifecycle_detail.h, is that bracket.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_synth_key.h"
#include "coop/props/join_membership_sweep.h"  // self-claim after the wire express
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::prop_lifecycle {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// Install idempotency (InstallInventory only; the Init/destroy latches live
// with Install in prop_lifecycle.cpp).
bool g_inventoryObserverInstalled = false;

// PRE observer for propInventory_C::takeObj -- sets g_takeObjInFlight so
// the nested Aprop_C::Init POST observer defers its broadcast (Key is
// NewGuid pre-loadData).
void GrabObserver_PropInventory_TakeObj_PRE(void* self, void* /*function*/, void* /*params*/) {
    auto* s = LoadSession();
    if (!self || !s) return;
    if (!s->connected()) return;
    g_takeObjInFlight.store(true, std::memory_order_relaxed);
}

// POST observer for propInventory_C::takeObj -- the canonical broadcaster
// for container extracts (after loadData restored the saved Key).
void GrabObserver_PropInventory_TakeObj_POST(void* self, void* function, void* params) {
    // Clear the in-flight flag FIRST regardless of any early returns.
    g_takeObjInFlight.store(false, std::memory_order_relaxed);

    auto* s = LoadSession();
    if (!self || !params || !function || !s) return;
    // Cache the Object out-param offset. Atomic because the observer can
    // dispatch on either the game thread (typical) or a task-graph worker.
    static std::atomic<int32_t> sObjectOff{-2};
    int32_t off = sObjectOff.load(std::memory_order_acquire);
    if (off == -2) {
        const int32_t resolved = R::FindParamOffset(function, L"Object");
        sObjectOff.store(resolved >= 0 ? resolved : -1, std::memory_order_release);
        off = resolved >= 0 ? resolved : -1;
        UE_LOGI("grab_hook[takeObj POST]: resolved Object out-param offset = %d", resolved);
    }
    if (off < 0) return;
    void* spawnedActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + off);
    if (!spawnedActor || !R::IsLive(spawnedActor)) return;
    if (!s->connected()) {
        UE_LOGI("grab_hook[takeObj POST]: spawned %p but session not connected -- skipping broadcast",
                spawnedActor);
        return;
    }
    if (!ue_wrap::prop::IsKeyedInteractable(spawnedActor)) {
        UE_LOGW("grab_hook[takeObj POST]: spawned actor %p is NOT a keyed-interactable -- skipping",
                spawnedActor);
        return;
    }

    coop::net::PropSpawnPayload p{};
    const std::wstring cls = R::ClassNameOf(spawnedActor);
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i) {
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    }
    // Mirror the Init POST's EnsureKeyForBroadcast call here, so a non-Aprop_C keyed interactable
    // extracted from an inventory container also gets a synthetic Key. It is a no-op for the
    // Aprop_C lineage, whose Key is already set through loadData: it just returns currentKey
    // unchanged, at zero cost in the typical inventory drop.
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(spawnedActor);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(spawnedActor, keyStr);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    const auto loc = ue_wrap::engine::GetActorLocation(spawnedActor);
    const auto rot = ue_wrap::engine::GetActorRotation(spawnedActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // Real scale, identity row and SP-parity bools, the same stamp the Init POST site makes, since
    // a container extraction is a fresh simulating spawn. The flag reads are Aprop_C-gated: without
    // that gate they read the heavy and frozen offsets on non-Aprop_C lineages too, which is stray
    // bytes.
    const auto scl = ue_wrap::engine::GetActorScale3D(spawnedActor);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(spawnedActor)) {
        if (ue_wrap::prop::IsHeavy(spawnedActor))   p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(spawnedActor))  p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(spawnedActor))  p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(spawnedActor)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(spawnedActor)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        const std::wstring nm = ue_wrap::prop::GetPropNameString(spawnedActor);
        for (size_t i = 0; i < nm.size() && i < 31; ++i) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[i]);
        }
        // The save-scalar birth channel: a reel extracted from a container keeps its Progress on
        // every peer's mirror.
        float sc = 0.f;
        if (ue_wrap::prop::ReadSavedScalarForClass(spawnedActor, sc)) {
            p.savedScalar = sc;
            p.physFlags |= coop::net::propspawn_flags::kHasSavedScalar;
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;

    // The Init POST returned early on g_takeObjInFlight, so MarkPropElement was not called from
    // that path. Mint the Prop Element here, so this container-extracted actor has a Registry
    // shadow. Mark may RE-KEY a duplicate under host key authority, so rebuild p.key -- filled
    // above -- from the enrolled key.
    keyStr = PT::MarkPropElement(spawnedActor, keyStr, cls, PT::EnrollSource::kExpressSeam);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);
    }
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(spawnedActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    UE_LOGI("grab_hook[takeObj POST]: SPAWN broadcast cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) heavy=%d frozen=%d eid=%u",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0,
            p.elementId);
    s->SendPropSpawn(p);  // channel queues internally
    // Self-claim (see the Init POST site).
    coop::join_membership_sweep::RecordClaimIfTracking(spawnedActor);
}

}  // namespace

// takeObj-in-flight bracket (declared in prop_lifecycle_detail.h). The nested Init POST reads it;
// the POST observer here clears it per dispatch, and prop_lifecycle's OnDisconnect clears it at
// teardown.
//
// It is a std::atomic<bool> rather than a plain one: the PRE and POST observers and the nested Init
// POST all run from parallel-anim worker threads, per game_thread.cpp's header, so plain-bool
// writes in the PRE against reads in the Init POST race under the C++ memory model. The
// PRE-then-POST sequencing within one ProcessEvent dispatch is preserved by same-thread execution
// order, and relaxed memory order is enough, since no other state depends on this bool's visibility
// ordering.
std::atomic<bool> g_takeObjInFlight{false};

void InstallInventory(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    PT::SetSession(session);  // mirror; see SetSession comment in prop_lifecycle.cpp.
    // An atomic latch, matching Install()'s. Without it the 125 Hz pump's InstallGrabObservers call
    // runs R::FindClass -- a full GUObjectArray walk with a std::wstring allocation per entry -- on
    // every tick until propInventory_C loads.
    static std::atomic<bool> s_done{false};
    if (s_done.load(std::memory_order_acquire)) return;
    if (g_inventoryObserverInstalled) {
        s_done.store(true, std::memory_order_release);
        return;
    }
    void* invCls = R::FindClass(P::name::PropInventoryClass);
    if (!invCls) return;  // not loaded yet -- retry next tick
    void* fn = R::FindFunction(invCls, P::name::PropInventoryTakeObjFn);
    if (!fn) {
        UE_LOGW("grab_hook: %ls.%ls UFunction not found -- Bug C disabled permanently this session",
                P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn);
        g_inventoryObserverInstalled = true;  // stop the retry loop
        s_done.store(true, std::memory_order_release);
        return;
    }
    if (!GT::RegisterPostObserver(fn, GrabObserver_PropInventory_TakeObj_POST)) {
        UE_LOGW("grab_hook: failed to register takeObj POST observer (table full?)");
        return;
    }
    if (!GT::RegisterPreObserver(fn, GrabObserver_PropInventory_TakeObj_PRE)) {
        UE_LOGW("grab_hook: failed to register takeObj PRE observer (table full?) -- container extracts may double-broadcast with mismatched keys");
    } else {
        UE_LOGI("grab_hook: registered PRE observer for %ls.%ls (takeObj-in-flight bracket)",
                P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn);
    }
    UE_LOGI("grab_hook: registered POST observer for %ls.%ls @ %p (Bug C inventory drop ready)",
            P::name::PropInventoryClass, P::name::PropInventoryTakeObjFn, fn);
    g_inventoryObserverInstalled = true;
    s_done.store(true, std::memory_order_release);
}

}  // namespace coop::prop_lifecycle

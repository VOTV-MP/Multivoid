// coop/props/prop_container_extract.cpp -- the propInventory_C::takeObj
// container-extract seam of coop::prop_lifecycle (see coop/props/
// prop_lifecycle.h): the PRE/POST observer pair + the in-flight bracket +
// InstallInventory.
//
// Extracted from prop_lifecycle.cpp 2026-07-10 when it passed the 800-LOC
// soft cap (the destroy seam left first -- prop_destroy_seam.cpp; this is the
// second slice). Behavior preserved byte-for-byte.
//
// Storage-container spawn fix (2026-05-25 RE): an Aprop extracted from a
// container spawns INSIDE takeObj, before loadData has restored its saved
// Key -- so the nested Aprop_C::Init POST observer (prop_lifecycle.cpp) must
// defer its broadcast and let the takeObj POST here be the canonical
// broadcaster. g_takeObjInFlight (defined here, shared via
// prop_lifecycle_detail.h) is that bracket.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/element.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_synth_key.h"
#include "coop/props/join_membership_sweep.h"  // self-claim after the wire express
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

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
    // Audit defensive close 2026-05-29 (M-1 extraction post-ship): mirror
    // the Init POST EnsureKeyForBroadcast call here so a non-Aprop_C
    // keyed-interactable extracted from an inventory container also gets
    // a synthetic Key (no-op for Aprop_C lineage whose Key is already set
    // via loadData; just returns currentKey unchanged). Symmetric with
    // the Init POST path; zero cost in the typical Aprop_C inventory drop.
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
    // v54: real scale + identity row + SP-parity bools (same stamp as the
    // Init POST site; a container extraction is a fresh simulating spawn).
    // The flag reads are Aprop_C-gated -- pre-v54 this read the heavy/frozen
    // offsets on non-Aprop_C lineages too, i.e. stray bytes.
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
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;

    // Init POST returned early (g_takeObjInFlight) so MarkPropElement wasn't
    // called from that path. Mint the Prop Element here so this container-
    // extracted actor has a Registry shadow (audit fix 2026-05-28).
    PT::MarkPropElement(spawnedActor, keyStr, cls);
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(spawnedActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
        // (v15 stamped a senderContext byte here; v16 PR-FOUNDATION-1b
        // moved stale-gen defense to the header senderEpoch.)
    }
    UE_LOGI("grab_hook[takeObj POST]: SPAWN broadcast cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) heavy=%d frozen=%d eid=%u",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ,
            (p.physFlags & coop::net::propspawn_flags::kIsHeavy)  ? 1 : 0,
            (p.physFlags & coop::net::propspawn_flags::kFrozen)   ? 1 : 0,
            p.elementId);
    s->SendPropSpawn(p);  // channel queues internally
    // Fork B 2c: self-claim (see the Init POST site).
    coop::join_membership_sweep::RecordClaimIfTracking(spawnedActor);
}

}  // namespace

// takeObj-in-flight bracket (declared in prop_lifecycle_detail.h; the nested
// Aprop_C::Init POST observer in prop_lifecycle.cpp reads it, OnDisconnect
// there clears it).
//
// Audit H12 (2026-05-27): std::atomic<bool> (was plain bool). The PRE/POST
// observers and the nested Init POST observer all run from parallel-anim
// worker threads per game_thread.cpp's header comment; plain-bool writes
// in the PRE + reads in the Init POST race per the C++ memory model. The
// PRE->POST sequencing within a single ProcessEvent dispatch is preserved
// by the same-thread execution order; relaxed memory order is sufficient
// (no other state depends on the bool's visibility ordering).
std::atomic<bool> g_takeObjInFlight{false};

void InstallInventory(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
    PT::SetSession(session);  // mirror; see SetSession comment in prop_lifecycle.cpp.
    // Audit Fix 2 (2026-05-27): atomic latch matches Install()'s. Without
    // this, the 125 Hz pump's InstallGrabObservers call runs R::FindClass
    // (a full GUObjectArray walk with std::wstring alloc per entry) on
    // every tick until propInventory_C loads. The plain-bool guard below
    // is read/written only from the GT, but it doesn't short-circuit the
    // GUObjectArray walk if propInventory_C is slow to load.
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

// coop/world/world_actor_mirror.cpp -- the CLIENT half of the world_actor
// lane (the npc_mirror shape): wire materialize (OnWorldActorSpawn), wire
// destroy (OnWorldActorDestroy) and the per-frame pose apply + drive
// (TickClientWorldActors). The HOST half + Install/lifecycle owner is
// world_actor_sync.cpp; shared internals come through world_actor_detail.h.
// Extracted 2026-07-05 (modular file-size rule; audit-endorsed split at 834
// LOC). Public API unchanged (include/coop/world/world_actor_sync.h; same
// namespace, two TUs).

#include "coop/world/world_actor_sync.h"

#include "world_actor_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/piramid_sync.h"  // v100 auxYaw + v102 auxVec consumers

#include "coop/element/identity_create.h"  // the single WorldActor mirror create funnel (Inc A)
#include "coop/element/mirror_managers.h"  // WaMirrors
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::world_actor_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = coop::world_actor_sync::detail;

using coop::element::WaMirrors;

}  // namespace

void OnWorldActorSpawn(const coop::net::EntitySpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    auto* s = D::Session();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("world-actor[client OnSpawn]: received on host -- dropping (loopback bounce)");
        return;
    }
    // Host-authoritative: the eid must be in the host range (the event_feed senderPeerSlot==0 gate
    // already rejected non-host senders).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("world-actor[client OnSpawn]: eid=%u out of host range [1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    if (payload.className.len == 0 || payload.className.len > 63) {
        UE_LOGW("world-actor[client OnSpawn]: bad className.len=%u -- dropping", payload.className.len);
        return;
    }
    std::wstring classW;
    classW.reserve(payload.className.len);
    for (uint8_t i = 0; i < payload.className.len; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.className.data[i])));
    // Trust-boundary: finite + bounded floats (a NaN must not reach BeginDeferred's SpawnTransform).
    const float vals[6] = {payload.locX, payload.locY, payload.locZ,
                           payload.rotPitch, payload.rotYaw, payload.rotRoll};
    for (float v : vals) {
        if (!std::isfinite(v)) {
            UE_LOGW("world-actor[client OnSpawn]: non-finite float -- dropping (eid=%u)", payload.elementId);
            return;
        }
    }
    if (std::fabs(payload.locX) > coop::net::kMaxCoord ||
        std::fabs(payload.locY) > coop::net::kMaxCoord ||
        std::fabs(payload.locZ) > coop::net::kMaxCoord) {
        UE_LOGW("world-actor[client OnSpawn]: loc out of bounds -- dropping (eid=%u)", payload.elementId);
        return;
    }
    // Trust-boundary allowlist gate: only materialize allowlisted WA class names (a peer could send an
    // arbitrary className; this forces R::FindClass GUObjectArray walks otherwise).
    if (!D::IsAllowlistedClassNameW(classW)) {
        UE_LOGW("world-actor[client OnSpawn]: class '%ls' not on WA allowlist -- rejecting (eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    // Duplicate-eid guard.
    if (WaMirrors().Get(payload.elementId) != nullptr) {
        UE_LOGW("world-actor[client OnSpawn]: eid=%u already mirrored -- dropping duplicate", payload.elementId);
        return;
    }
    const D::SpawnPath sp = D::GetSpawnPath();
    if (!sp.spawnFn || !sp.finishSpawnFn || !sp.gsCdo || sp.returnParamOff < 0) {
        UE_LOGW("world-actor[client OnSpawn]: receiver UFunctions unresolved -- dropping (eid=%u)",
                payload.elementId);
        return;
    }
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("world-actor[client OnSpawn]: class '%ls' not loaded -- dropping (eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("world-actor[client OnSpawn]: no world context -- dropping (eid=%u)", payload.elementId);
        return;
    }
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX; xform.TY = payload.locY; xform.TZ = payload.locZ;
    // v99: spawn-transform scale from the wire (sanitized -- scale-0 = invisible actor). The piramid
    // spawner passes 2.0; a unit-scale mirror renders half-size + floats at the host's scale-2 hover Z.
    xform.SX = coop::net::SanitizeWireScaleAxis(payload.scaleX);
    xform.SY = coop::net::SanitizeWireScaleAxis(payload.scaleY);
    xform.SZ = coop::net::SanitizeWireScaleAxis(payload.scaleZ);

    // Bypass our own client interceptor for THIS spawn, then BeginDeferred + FinishSpawning the mirror.
    D::SetIncomingClass(actorClass);
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(sp.spawnFn);
        if (!begin.valid()) {
            UE_LOGE("world-actor[client OnSpawn]: ParamFrame(BeginDeferred) invalid -- dropping eid=%u",
                    payload.elementId);
            D::ClearIncomingClass();
            return;
        }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(sp.gsCdo, begin)) {
            UE_LOGE("world-actor[client OnSpawn]: BeginDeferred call failed for '%ls' eid=%u",
                    classW.c_str(), payload.elementId);
            D::ClearIncomingClass();
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("world-actor[client OnSpawn]: BeginDeferred returned null for '%ls' eid=%u (suppressor "
                "swallowed it?)", classW.c_str(), payload.elementId);
        D::ClearIncomingClass();
        return;
    }
    {
        ParamFrame finish(sp.finishSpawnFn);
        if (!finish.valid()) {
            UE_LOGE("world-actor[client OnSpawn]: ParamFrame(FinishSpawning) invalid -- forcing K2 on %p",
                    spawned);
            if (sp.k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, sp.k2DestroyFn, nullptr);
            return;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(sp.gsCdo, finish)) {
            UE_LOGE("world-actor[client OnSpawn]: FinishSpawning failed for %p '%ls' eid=%u -- forcing K2",
                    spawned, classW.c_str(), payload.elementId);
            if (sp.k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, sp.k2DestroyFn, nullptr);
            return;
        }
    }

    const coop::element::ElementId eid = static_cast<coop::element::ElementId>(payload.elementId);
    if (!coop::element::CreateOrAdoptWorldActorMirror(eid, spawned, classW, /*senderSlot=*/-1)) {
        UE_LOGW("world-actor[client OnSpawn]: CreateOrAdoptWorldActorMirror(eid=%u) failed -- destroying orphan actor %p",
                payload.elementId, spawned);
        if (sp.k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, sp.k2DestroyFn, nullptr);
        return;
    }
    // Park the mirror so the streamed pose drive is authoritative: GENERIC actor-tick OFF (no CMC read --
    // a WorldActor is a plain AActor). Any residual component tick is overwritten each frame by the
    // pose drive's SetActorLocation/SetActorRotation (the MTA dead-reckoning model).
    E::SetActorTickEnabled(spawned, false);
    UE_LOGI("world-actor[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p loc=(%.0f,%.0f,%.0f) "
            "scale=(%.2f,%.2f,%.2f)", payload.elementId, classW.c_str(), spawned,
            payload.locX, payload.locY, payload.locZ, xform.SX, xform.SY, xform.SZ);
}

void OnWorldActorDestroy(const coop::net::EntityDestroyPayload& payload) {
    auto* s = D::Session();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("world-actor[client OnDestroy]: received on host -- dropping (loopback bounce)");
        return;
    }
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("world-actor[client OnDestroy]: eid=%u out of host range -- dropping", payload.elementId);
        return;
    }
    const coop::element::ElementId eid = static_cast<coop::element::ElementId>(payload.elementId);
    std::unique_ptr<coop::element::WorldActor> drained = WaMirrors().Take(eid);
    if (!drained) {
        UE_LOGI("world-actor[client OnDestroy]: eid=%u not in mirror table -- ignoring", payload.elementId);
        return;
    }
    void* actor = drained->GetActor();
    const D::SpawnPath sp = D::GetSpawnPath();
    if (actor && sp.k2DestroyFn && R::IsLive(actor)) {
        R::CallFunction(actor, sp.k2DestroyFn, nullptr);
        UE_LOGI("world-actor[client OnDestroy]: K2_DestroyActor on mirror eid=%u actor=%p",
                payload.elementId, actor);
    } else if (actor && !R::IsLive(actor)) {
        UE_LOGI("world-actor[client OnDestroy]: mirror eid=%u actor already not-live -- skipping K2",
                payload.elementId);
    }
    // drained's dtor fires here -> Registry::UnregisterMirror(eid).
}

void TickClientWorldActors() {
    auto* s = D::Session();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (host streams, doesn't drive)

    // 1) Apply the latest received batch -> open an interp window per WA. Per-entry float validation is
    //    the trust boundary (a NaN must not reach SetActorLocation/SetActorRotation).
    std::vector<coop::net::WorldActorPoseSnapshot> batch;
    if (s->TakeRemoteWorldActorBatch(batch)) {
        // [WA-TRACE client-apply] 1 Hz per-entry OUTCOME trace (2026-07-05 0s-frozen-pyramid hunt).
        // Every skip branch below was SILENT -- a wrong eid / not-a-mirror / range-clamped entry
        // freezes the mirror with zero evidence. The old "first batch" INFO only proved ARRIVAL.
        static long long s_lastApplyTraceMs = 0;
        const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool trace = !batch.empty() && (nowMs - s_lastApplyTraceMs >= 1000);
        if (trace) s_lastApplyTraceMs = nowMs;
        for (const auto& snap : batch) {
            if (!std::isfinite(snap.x) || !std::isfinite(snap.y) || !std::isfinite(snap.z) ||
                !std::isfinite(snap.pitch) || !std::isfinite(snap.yaw) || !std::isfinite(snap.roll)) {
                if (trace) UE_LOGW("[WA-TRACE client-apply] eid=%u SKIP non-finite pose", snap.elementId);
                continue;
            }
            if (std::fabs(snap.x) > 1.0e6f || std::fabs(snap.y) > 1.0e6f || std::fabs(snap.z) > 1.0e6f) {
                if (trace) UE_LOGW("[WA-TRACE client-apply] eid=%u SKIP out-of-range (%.0f,%.0f,%.0f)",
                                   snap.elementId, snap.x, snap.y, snap.z);
                continue;
            }
            coop::element::WorldActor* el = WaMirrors().Get(snap.elementId);
            if (!el || !el->IsMirror()) {  // not materialized yet (connect gap) / defensive
                if (trace) UE_LOGW("[WA-TRACE client-apply] eid=%u SKIP %s", snap.elementId,
                                   el ? "element-not-mirror" : "no-element");
                continue;
            }
            el->SetTargetPose(snap);
            if (trace) UE_LOGI("[WA-TRACE client-apply] eid=%u wire=(%.0f,%.0f,%.0f) yaw=%.1f aux=%.1f -> SetTargetPose",
                               snap.elementId, snap.x, snap.y, snap.z, snap.yaw, snap.auxYaw);
        }
        static bool s_loggedFirst = false;
        if (!batch.empty() && !s_loggedFirst) { s_loggedFirst = true;
            UE_LOGI("world-actor-pose: client applying %zu WorldActor pose(s) (first batch)", batch.size()); }
    }

    // 2) Advance the interp + drive EVERY live mirror, every frame (smooth between packets). Reused
    //    scratch (no per-tick heap alloc); client game thread only.
    static std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    for (coop::element::WorldActor* el : elems) {
        if (!el || !el->IsMirror()) continue;
        el->Tick();
        // v100 auxYaw + v102 auxVec consumers: the piramid's visible heading lives in its
        // ArrowComponents and its head look target in relLook -- neither is the actor
        // transform the generic drive writes; hand the interp'd/latest values to the lane.
        if (el->HasPose() && el->GetTypeName() == "piramid2_C") {
            void* actor = el->GetActor();
            if (actor && R::IsLiveByIndex(actor, el->GetInternalIdx())) {
                coop::piramid_sync::ApplyMirrorHeadingYaw(actor, el->CurrentAuxYaw());
                float ax = 0.f, ay = 0.f, az = 0.f;
                el->CurrentAuxVec(ax, ay, az);
                coop::piramid_sync::ApplyMirrorRelLook(actor, ax, ay, az);
                // v104: mirror the wispTarget identity -> the native tick's CHASE look-at
                // branch runs both ends during the walk (gathering-owned windows skipped
                // inside).
                coop::piramid_sync::ApplyMirrorWispTarget(actor, el->CurrentAuxTargetEid());
            }
        }
    }
}

}  // namespace coop::world_actor_sync

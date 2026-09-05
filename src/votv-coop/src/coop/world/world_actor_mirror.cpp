// coop/world/world_actor_mirror.cpp -- the client half of the world-actor lane (the NPC mirror
// shape): wire materialise, wire destroy and the per-frame pose apply and drive. The host
// half and the install and lifecycle owner is world_actor_sync.cpp; shared internals come
// through world_actor_detail.h. One public header, one namespace, two translation units.

#include "coop/world/world_actor_sync.h"

#include "world_actor_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/piramid_sync.h"  // the heading, look and wisp-target consumers

#include "coop/element/identity_create.h"  // the single WorldActor mirror create funnel
#include "coop/items/coingun_sync.h"
#include "coop/element/mirror_managers.h"  // WaMirrors
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::world_actor_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = coop::world_actor_sync::detail;

using coop::element::WaMirrors;

}  // namespace

void OnWorldActorSpawn(const coop::net::WorldActorSpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    auto* s = D::Session();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("world-actor[client OnSpawn]: received on host -- dropping (loopback bounce)");
        return;
    }
    // Host-authoritative: the eid must be in the host range (the entity dispatch already rejected
    // non-host senders).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("world-actor[client OnSpawn]: eid=%u out of host range [1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    if (payload.className.len == 0 || payload.className.len > 63) {
        UE_LOGW("world-actor[client OnSpawn]: bad className.len=%u -- dropping", payload.className.len);
        return;
    }
    // Re-validate the birth blob's length at the consumer too. The dispatch already checked it,
    // as it did the class-name length this function re-checks above; a receiver that trusts its
    // caller's range check is one refactor away from not having one.
    if (payload.birthLen > sizeof(payload.birth)) {
        UE_LOGW("world-actor[client OnSpawn]: bad birthLen=%u -- dropping", payload.birthLen);
        return;
    }
    std::wstring classW;
    classW.reserve(payload.className.len);
    for (uint8_t i = 0; i < payload.className.len; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.className.data[i])));
    // Trust boundary: finite and bounded floats (a NaN must not reach the spawn transform).
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
    // Trust-boundary allowlist gate: only allowlisted class names materialise (a peer could send
    // an arbitrary class name, which would otherwise force class-lookup walks of the object
    // array).
    if (!D::IsAllowlistedClassNameW(classW)) {
        UE_LOGW("world-actor[client OnSpawn]: class '%ls' not on WA allowlist -- rejecting (eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    // Duplicate-eid guard. A row whose actor is dead is not a duplicate, it is a stale row: a
    // mirror row outlives its actor by design (the death watch retires the row on a later tick),
    // so testing row presence alone answered a legitimate re-announce of a live host actor with
    // "already mirrored", and that actor stayed invisible on the peer with this warning as the
    // only trace. The guard tests the actor: a live one is a real duplicate and still drops; a
    // dead one is drained here and the spawn below re-materialises it. The drain goes through
    // the same two steps the destroy path uses, take the row and un-note the actor, because
    // dropping the row alone leaks the stale pointer into the mirror-actor set, where a recycled
    // allocation makes an unrelated actor read as a mirror and its pickup gets cancelled.
    if (coop::element::WorldActor* existing = WaMirrors().Get(payload.elementId)) {
        void* prevActor = existing->GetActor();
        if (prevActor && R::IsLiveByIndex(prevActor, existing->GetInternalIdx())) {
            UE_LOGW("world-actor[client OnSpawn]: eid=%u already mirrored by a LIVE actor=%p -- "
                    "dropping duplicate", payload.elementId, prevActor);
            return;
        }
        UE_LOGW("world-actor[client OnSpawn]: eid=%u held a STALE row (actor=%p, not live) -- draining "
                "it and re-materialising rather than dropping this announce as a duplicate. A live "
                "host actor would otherwise have stayed invisible on this peer forever.",
                payload.elementId, prevActor);
        std::unique_ptr<coop::element::WorldActor> stale = WaMirrors().Take(payload.elementId);
        coop::world_actor_sync::NoteMirrorActor(prevActor, /*add=*/false);
        // The stale row's destructor unregisters the mirror eid here, which frees it for the
        // register the materialisation below performs.
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
    // Spawn-transform scale from the wire (sanitised: a zero scale is an invisible actor). The
    // pyramid spawns at a non-unit scale; a unit-scale mirror renders the wrong size and floats
    // at the host's hover height.
    xform.SX = coop::net::SanitizeWireScaleAxis(payload.scaleX);
    xform.SY = coop::net::SanitizeWireScaleAxis(payload.scaleY);
    xform.SZ = coop::net::SanitizeWireScaleAxis(payload.scaleZ);

    // The materialisation window. A mirror's component-bound delegates bind during BeginPlay,
    // inside FinishSpawning, before the mirror row below exists, so the row can never be the
    // sole is-this-a-mirror discriminator: a coin seeded onto a joiner standing where it lies
    // would fire its collect overlap in this window, be judged a non-mirror, and credit that
    // client locally. The window is published (the coin lane tests mirror-or-materialising) and
    // carries the eid being installed, so a consumer that must name the actor being born has one
    // identity source instead of hunting for a row that does not exist yet.
    coop::world_actor_sync::MaterializeScope materializeScope(payload.elementId);

    // Bypass our own client interceptor for this spawn, then deferred-begin and finish-spawn the
    // mirror.
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
    // The actor exists and its mirror row does not: publish it into the open window so a consumer
    // firing inside FinishSpawning's BeginPlay can identify this actor rather than merely observe
    // that some materialisation is in progress.
    coop::world_actor_sync::NoteMaterializingActor(spawned);
    // Seed the birth value before FinishSpawning runs BeginPlay. The coin's BeginPlay reads its
    // points once and sets its material from them (bronze, silver or gold), and the coin gun's
    // sell writes that integer between its own deferred begin and finish. This mirror had nothing
    // here, so every mirrored coin was born at the class default and painted bronze, and the
    // default is also the commonest real denomination, which is why the field report said
    // "different" and not "always wrong". We set the input and let the game paint; nothing here
    // touches a material. Class-scoped, matching the prepare call below: the coin lane owns the
    // coin specifics. A zero length means leave the default alone: writing zero would be an
    // invisible fail-open, painting bronze exactly like the bug.
    if (classW == L"baocoin_C" && payload.birthLen != 0) {
        if (payload.birthLen != sizeof(int32_t)) {
            UE_LOGW("world-actor[client OnSpawn]: baocoin_C eid=%u birthLen=%u, expected %zu -- "
                    "dropping the birth value (a wrong length is a protocol violation, not a short "
                    "coin)", payload.elementId, payload.birthLen, sizeof(int32_t));
        } else {
            int32_t pts = 0;
            std::memcpy(&pts, payload.birth, sizeof(pts));
            coop::coingun_sync::SeedCoinMirror(spawned, pts);
        }
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
    // Park the mirror so the streamed pose drive is authoritative: the generic actor tick off (no
    // movement-component read; a world actor is a plain actor). Any residual component tick is
    // overwritten each frame by the pose drive's location and rotation writes (the MTA
    // dead-reckoning model).
    E::SetActorTickEnabled(spawned, false);
    // A pose-driven mirror must not also simulate: the two fight and the mirror drifts off the
    // host's transform. The coin's sphere simulates physics, the first simulating member of this
    // allowlist; the other classes are event actors that do not. Class-scoped on purpose: the
    // general invariant is named here, not applied.
    if (classW == L"baocoin_C") coop::coingun_sync::PrepareCoinMirror(spawned);
    coop::world_actor_sync::NoteMirrorActor(spawned, /*add=*/true);
    // The instrument. This line fires once per mirror, keyed by eid, so the birth seam is where
    // host and client can be paired; the host logs its own points and material at its enrol.
    // Both halves read the material because producer and instrument read the points through the
    // same offset at the same site, so a wrong read would print agreement while the coins still
    // drew differently; the material is independent evidence, painted by the game from the real
    // value. Liveness-guarded: this runs after FinishSpawning executed the coin's own BeginPlay,
    // and the describe call dereferences the actor and dispatches on its component.
    if (classW == L"baocoin_C" && R::IsLive(spawned)) {
        int32_t pts = -1;
        std::wstring mat;
        coop::coingun_sync::DescribeCoin(spawned, pts, mat);
        UE_LOGI("world-actor[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p "
                "loc=(%.0f,%.0f,%.0f) scale=(%.2f,%.2f,%.2f) birthLen=%u points=%d mat='%ls'",
                payload.elementId, classW.c_str(), spawned, payload.locX, payload.locY, payload.locZ,
                xform.SX, xform.SY, xform.SZ, payload.birthLen, pts,
                mat.empty() ? L"<unresolved>" : mat.c_str());
    } else {
        UE_LOGI("world-actor[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p loc=(%.0f,%.0f,%.0f) "
                "scale=(%.2f,%.2f,%.2f)", payload.elementId, classW.c_str(), spawned,
                payload.locX, payload.locY, payload.locZ, xform.SX, xform.SY, xform.SZ);
    }
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
    coop::world_actor_sync::NoteMirrorActor(actor, /*add=*/false);
    const D::SpawnPath sp = D::GetSpawnPath();
    // Index-validated liveness, not the plain check: a cached mirror actor drained from the table
    // plus a destroy call. If its slot was recycled (this can race a world teardown), the plain
    // check passes the foreign occupant and the call runs on the wrong object.
    if (actor && sp.k2DestroyFn && R::IsLiveByIndex(actor, drained->GetInternalIdx())) {
        R::CallFunction(actor, sp.k2DestroyFn, nullptr);
        UE_LOGI("world-actor[client OnDestroy]: K2_DestroyActor on mirror eid=%u actor=%p",
                payload.elementId, actor);
    } else if (actor && !R::IsLiveByIndex(actor, drained->GetInternalIdx())) {
        UE_LOGI("world-actor[client OnDestroy]: mirror eid=%u actor already not-live -- skipping K2",
                payload.elementId);
    }
    // The drained row's destructor unregisters the mirror eid here.
}

void TickClientWorldActors() {
    auto* s = D::Session();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (host streams, doesn't drive)

    // First, apply the latest received batch: open an interpolation window per actor. The
    // per-entry float validation is the trust boundary (a NaN must not reach the location and
    // rotation writes).
    std::vector<coop::net::WorldActorPoseSnapshot> batch;
    if (s->TakeRemoteWorldActorBatch(batch)) {
        // A once-a-second per-entry outcome trace: every skip branch below was silent, and a wrong
        // eid, a not-a-mirror or a range-clamped entry freezes the mirror with zero evidence; a
        // first-batch line only proves arrival.
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

    // Second, advance the interpolation and drive every live mirror, every frame (smooth between
    // packets). Reused scratch, no per-tick heap allocation; client game thread only.
    static std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    for (coop::element::WorldActor* el : elems) {
        if (!el || !el->IsMirror()) continue;
        el->Tick();
        // The pyramid's visible heading lives in its arrow components and its head look target in
        // its relative look; neither is the actor transform the generic drive writes, so hand the
        // interpolated or latest values to the lane.
        if (el->HasPose() && el->GetTypeName() == "piramid2_C") {
            void* actor = el->GetActor();
            if (actor && R::IsLiveByIndex(actor, el->GetInternalIdx())) {
                coop::piramid_sync::ApplyMirrorHeadingYaw(actor, el->CurrentAuxYaw());
                float ax = 0.f, ay = 0.f, az = 0.f;
                el->CurrentAuxVec(ax, ay, az);
                coop::piramid_sync::ApplyMirrorRelLook(actor, ax, ay, az);
                // Mirror the wisp-target identity: the native tick's chase look-at branch runs on
                // both ends during the walk (gathering-owned windows skipped inside).
                coop::piramid_sync::ApplyMirrorWispTarget(actor, el->CurrentAuxTargetEid());
            }
        }
    }
}

}  // namespace coop::world_actor_sync

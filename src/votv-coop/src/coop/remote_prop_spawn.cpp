// coop/remote_prop_spawn.cpp -- wire-driven PropSpawn receiver
// (extracted from coop/remote_prop.cpp M-1 2026-05-29).
//
// See coop/remote_prop_spawn.h for the public interface + design notes.
//
// Behavior preserved byte-for-byte from the prior in-line implementation
// in remote_prop.cpp: same dedup gates (exact-key, fuzzy-position),
// same drive-skip semantics (PropPose owns position when held), same
// rekey-on-fuzzy-match pattern, same collision-restore-on-natural-spawn-
// class workaround, same fresh-spawn pipeline (BeginDeferred + setKey
// BEFORE FinishSpawningActor), same mirror registration on every path.

#include "coop/remote_prop_spawn.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/prop_echo_suppress.h"
#include "coop/prop_element_tracker.h"
#include "coop/remote_prop.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <cstdint>
#include <string>

namespace coop::remote_prop_spawn {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// PropSpawn UFunction resolution (cached one-shot). Spawn path uses the
// deferred-spawn pair on UGameplayStatics CDO; setKey is on Aprop_C base;
// Conv_StringToName is on UKismetStringLibrary CDO (used to convert wire
// Key string -> live FName for setKey).
void* g_gsCdo            = nullptr;
void* g_beginSpawnFn     = nullptr;
void* g_finishSpawnFn    = nullptr;
void* g_propSetKeyFn     = nullptr;
bool  g_spawnResolved    = false;

bool ResolveSpawnFns() {
    if (g_spawnResolved && R::IsLive(g_gsCdo)) return true;
    g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (!g_gsCdo) return false;
    void* cls = R::ClassOf(g_gsCdo);
    if (!cls) return false;
    g_beginSpawnFn  = R::FindFunction(cls, P::name::BeginDeferredSpawnFn);
    g_finishSpawnFn = R::FindFunction(cls, P::name::FinishSpawningActorFn);
    if (!g_beginSpawnFn || !g_finishSpawnFn) {
        UE_LOGW("remote_prop::OnSpawn: GameplayStatics spawn fns missing (begin=%p finish=%p)",
                g_beginSpawnFn, g_finishSpawnFn);
        return false;
    }
    // setKey on Aprop_C base. The PropClass UClass may not be loaded at the
    // first call (the engine loads BP classes on-demand); on first miss we
    // retry next call.
    if (void* propCls = R::FindClass(P::name::PropClass)) {
        g_propSetKeyFn = R::FindFunction(propCls, P::name::PropSetKeyFn);
    }
    g_spawnResolved = true;
    return true;
}

// Build a wide string from WireClassName. Lossless for ASCII (VOTV class names).
std::wstring ClassNameToWString(const coop::net::WireClassName& cn) {
    std::wstring s;
    s.reserve(cn.len);
    for (uint8_t i = 0; i < cn.len && i < 63; ++i) {
        s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(cn.data[i])));
    }
    return s;
}

// True for prop classes whose locally-spawned instance lands with collision
// disabled (NoCollision) via a natural-spawn pipeline that calls
// spawnedNaturally(), and whose wire-converged copy therefore needs an
// explicit SetCollisionEnabled(QueryAndPhysics) restore. Per the mushroom
// fall-through RE 2026-05-25:
//   AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision.
// Currently only the cap mushroom is confirmed; extend if other natural-
// spawn paths exhibit the same pattern. The check is cheap (single string
// compare) so calling it in OnSpawn's hot path is fine.
//
// ==== RULE 1 RETIREMENT PLAN (audit fix 2026-05-25, issue #4) ====
// This restore is an INTERIM fix at the SYMPTOM (collision write at
// OnSpawn-time). The architecturally correct ROOT-CAUSE fix is to suppress
// AmushroomSpawner_C::Spawn on the client entirely, so the local actor
// never goes through spawnedNaturally() in the first place (Option B from
// the RE doc). Gating criteria for retirement:
//   (1) Phase 5N1 Stream B-Spawners ships -- single BeginDeferredActor
//       SpawnFromClass observer with a class allowlist that includes
//       AmushroomSpawner_C (covers all natural spawners simultaneously).
//   (2) Hands-on test confirms mushrooms sync correctly with this restore
//       call commented out (the test the user is running now is the
//       inverse -- with restore IN, verifying no fall-through).
//   (3) Then remove this helper + the IsCollisionRestoreClass check +
//       PropFoodMushroomClass constant. Per RULE 2 the kerfur-of-the-day
//       fix goes when the proper fix lands; no parallel paths.
// Tracked in [[project-coop-mushroom-state-re]] memory entry under
// "Stream B-Spawners" follow-up scope.
inline bool IsCollisionRestoreClass(const std::wstring& cls) {
    return cls == ue_wrap::profile::name::PropFoodMushroomClass;
}

// Helper for OnSpawn's three convergence/spawn paths: if the class needs
// collision restore, call ue_wrap::prop::ForceRestoreDefaultCollision and
// log the outcome with the path label so a fall-through regression is
// diagnosable from the log. No-op for classes that don't need restore.
void RestoreCollisionIfNeeded(const wchar_t* pathLabel,
                              const std::wstring& classW,
                              void* actor) {
    if (!IsCollisionRestoreClass(classW)) return;
    const bool ok = ue_wrap::prop::ForceRestoreDefaultCollision(actor);
    if (ok) {
        UE_LOGI("remote_prop::OnSpawn[%ls]: collision restored (QueryAndPhysics) on '%ls' actor=%p",
                pathLabel, classW.c_str(), actor);
    } else {
        UE_LOGW("remote_prop::OnSpawn[%ls]: collision restore FAILED on '%ls' actor=%p -- prop may fall through ground",
                pathLabel, classW.c_str(), actor);
    }
}

}  // namespace

void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    const std::wstring classW = ClassNameToWString(payload.className);
    const std::wstring keyW   = coop::remote_prop::KeyToWString(payload.key);
    UE_LOGI("remote_prop::OnSpawn: cls='%ls' key='%ls' loc=(%.1f, %.1f, %.1f) rot=(%.1f, %.1f, %.1f) physFlags=0x%02x",
            classW.c_str(), keyW.c_str(),
            payload.locX, payload.locY, payload.locZ,
            payload.rotPitch, payload.rotYaw, payload.rotRoll,
            static_cast<int>(payload.physFlags));
    // A non-keyable trash CLUMP rides this same pipeline identified by our EID, not a
    // Key (setKey doesn't stick on it). For the clump, key is None but elementId != 0;
    // it's deduped + registered + resolved by eid. Aprop_C keeps the keyed path.
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool eidOnly = (keyW.empty() || keyW == L"None");
    if (classW.empty() || (eidOnly && payload.elementId == 0)) {
        // "None" = FName(NAME_None) stringified. A keyed prop with no key AND no eid is
        // a legacy/in-flight straggler -- reject defensively so it doesn't trigger the
        // dedup spam loop on the receiver.
        UE_LOGW("remote_prop::OnSpawn: unkeyed + no eid (cls='%ls' key='%ls') -- dropping",
                classW.c_str(), keyW.c_str());
        return;
    }
    // Phase 5S0 de-dupe: if a local Aprop_C derivative with the same Key
    // already exists (e.g. both peers loaded the same save and both already
    // have this prop, OR a prior PropSpawn already created it), don't
    // duplicate. Instead force-converge: update the local actor's transform
    // to match the host's authoritative payload. This is the path that
    // corrects mushroom-desync (each peer's spawner placed the same-Key
    // mushroom at a different position; snapshot bootstrap teleports
    // client's to match host's). 2026-05-24.
    bool dedupeFellBack = false;
    void* existing = nullptr;
    if (eidOnly) {
        // Non-keyable clump: dedup by EID (the Registry mirror binding), not key.
        if (auto* e = coop::element::Registry::Get().Get(payload.elementId)) {
            void* a = e->GetActor();
            if (a && R::IsLive(a)) existing = a;
        }
    } else {
        existing = coop::prop_element_tracker::ResolveLiveActorByKey(keyW, &dedupeFellBack);
        if (dedupeFellBack) {
            // The key index was STALE for this prop (a world-change purged the indexed
            // actors and the slow steady-state re-seed hasn't caught up) -- this
            // de-dupe paid an O(N) GUObjectArray scan. During the connect snapshot
            // ~2300 PropSpawns arrive in a burst; without intervention EACH would scan
            // -> O(N_props x N_objects) -> the client RSS balloons multi-GB. Self-heal:
            // drain dead + re-seed the index NOW (throttled to once / 200 ms) so the
            // rest of this same-tick burst resolves O(1). [[project-bug-prop-resnapshot-leak]]
            coop::prop_element_tracker::ReconcileIndexThrottled();
        }
    }
    if (existing) {
        // Skip the convergence write if THIS prop
        // is currently under active kinematic drive by the PropPose stream
        // (host is holding it). Otherwise the SetActorLocation here would
        // stomp the active drive for one frame, producing a visible
        // teleport-pop until the next PropPose Tick corrects it. The
        // PropPose stream is authoritative for held props; let it own
        // position while held.
        if (coop::remote_prop::IsActorUnderAnyDrive(existing)) {
            UE_LOGI("remote_prop::OnSpawn: key '%ls' is under active kinematic drive (some slot) -- skipping convergence (PropPose owns position)",
                    keyW.c_str());
            // A2 (2026-05-29): still register the mirror so subsequent
            // PropDestroy from sender resolves via eid (we just declined
            // to teleport-pop, but the wire identity binding is still
            // useful).
            coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: key '%ls' already resolves to live actor %p -- de-duping, converging transform to host (loc=(%.1f,%.1f,%.1f))",
                keyW.c_str(), existing, payload.locX, payload.locY, payload.locZ);
        ue_wrap::engine::SetActorLocation(existing,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(existing,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        // 2026-05-25 mushroom fall-through fix: client's local copy may have
        // gone through spawnedNaturally() in its own AmushroomSpawner_C::Spawn
        // path, leaving NoCollision on the StaticMesh. Wire convergence
        // reuses the actor in-place (skipping a fresh Init); we must
        // explicitly restore default collision so subsequent host PropPose
        // releases don't drop the body into the void.
        RestoreCollisionIfNeeded(L"exact-key", classW, existing);
        // A2 mirror binding: associate sender's wire eid with the resolved
        // local actor so future PropDestroy / eid-routed lookups land
        // correctly.
        coop::remote_prop::RegisterPropMirror(payload.elementId, existing, keyW, classW, senderSlot);
        return;
    }
    // Phase 5S0 Gap I-1 (2026-05-24): exact-Key match failed. Try fuzzy
    // de-dupe for divergent-key same-position spawns. Per-peer-divergent
    // natural spawners (AmushroomMaster_C, AmushroomSpawner_C,
    // AundergroundGarbageSpawner_C) place same logical entities with
    // DIFFERENT Keys at slightly-different positions on each peer; without
    // this, the host's broadcast would spawn a NEW prop adjacent to the
    // client's pre-existing one. Conservative 30 cm radius + same-class
    // gate. Mismatches in dense-prop areas surface in the log.
    constexpr float kFuzzyRadiusCm = 30.f;
    // Non-keyable clump: skip the key/position fuzzy dedup (it rekeys the match via
    // setKey, meaningless for the clump) -- go straight to a fresh eid-bound spawn.
    void* fuzzy = eidOnly ? nullptr : ue_wrap::prop::FindNearbySameClass(
            classW,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ},
            kFuzzyRadiusCm);
    if (fuzzy) {
        // Mirror of the exact-Key guard: if the fuzzy match resolves to the
        // actor currently under active kinematic drive, skip convergence.
        // Otherwise the SetActorLocation here
        // would stomp the PropPose stream for one frame -- same
        // teleport-pop bug as the exact-Key path.
        if (coop::remote_prop::IsActorUnderAnyDrive(fuzzy)) {
            UE_LOGI("remote_prop::OnSpawn: Gap-I-1 fuzzy match '%ls' -> active drive actor (some slot) -- skipping (PropPose owns position)",
                    classW.c_str());
            // A2 audit fix (2026-05-29): mirror binding for the drive-skip
            // fuzzy path -- symmetric with the exact-key drive-skip path
            // above. Without this, Registry::Get(eid) on this peer would
            // never resolve for a fuzzy+drive-skipped prop.
            coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
            return;
        }
        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 FUZZY MATCH '%ls' (wire key '%ls') -> existing actor %p within %.1f cm -- de-duping, converging transform + rekeying",
                classW.c_str(), keyW.c_str(), fuzzy, kFuzzyRadiusCm);
        ue_wrap::engine::SetActorLocation(fuzzy,
            ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(fuzzy,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
        // REKEY the fuzzy-matched actor to the
        // host's wire Key. Without this, the actor keeps its client-local
        // NewGuid Key (K_c) while host PropPose packets carry K_h --
        // FindByKeyString(K_h) on client misses, host's grab/move stream
        // would be silently dropped (UE_LOGW "no local match"). The
        // result would be a prop that successfully de-duped (no visible
        // duplicate) but is permanently invisible to the host's
        // authoritative position stream -- a worse regression than the
        // duplicate it prevented. Aprop_C.setKey updates
        // mainGamemode.keyObj_key/obj cross-peer; the old K_c orphan
        // entry is harmless (no host packet references it).
        if (ResolveSpawnFns() && g_propSetKeyFn) {
            const R::FName keyFName = ue_wrap::fname_utils::StringToFName(keyW);
            if (keyFName.ComparisonIndex != 0) {
                ParamFrame sk(g_propSetKeyFn);
                if (sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
                    if (Call(fuzzy, sk)) {
                        UE_LOGI("remote_prop::OnSpawn: Gap-I-1 rekey ok -- actor %p now has wire key '%ls' (FName idx=%u); subsequent PropPose resolves correctly",
                                fuzzy, keyW.c_str(), keyFName.ComparisonIndex);
                    } else {
                        UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey setKey ProcessEvent call failed -- prop unreachable via host PropPose");
                    }
                } else {
                    UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey setKey 'Key' param not found");
                }
            } else {
                UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey StringToFName('%ls') -> NAME_None; skipped",
                        keyW.c_str());
            }
        } else {
            UE_LOGW("remote_prop::OnSpawn: Gap-I-1 rekey skipped -- setKey UFunction unresolved");
        }
        // Index the fuzzy-matched actor under the WIRE key, unconditionally --
        // resolution binds via our key->actor index, independent of whether the
        // engine-level setKey write above succeeded. The fuzzy match (same class
        // within 30 cm) already established this actor IS the host's prop, so the
        // host's subsequent PropPose/PropDestroy under the wire key must resolve
        // here O(1). Doing this only on setKey success (the prior placement) left
        // a rare setKey failure falling back to a per-frame GUObjectArray scan for
        // every packet on that held prop. keyW is validated non-empty/non-None at
        // OnSpawn entry; fuzzy is live.
        coop::prop_element_tracker::IndexActorKey(fuzzy, keyW);
        // 2026-05-25 mushroom fall-through fix (Gap-I-1 path): same reason as
        // exact-key. The fuzzy-matched local actor came from client's own
        // AmushroomSpawner_C::Spawn -> spawnedNaturally() -> NoCollision.
        // Without this restore, host's release-edge PropPose-driven mushroom
        // sits on top of the floor briefly then PhysX drops it through
        // because the body has NoCollision.
        RestoreCollisionIfNeeded(L"fuzzy", classW, fuzzy);
        // A2 (2026-05-29) mirror binding: the fuzzy-matched actor was just
        // rekey'd to the wire Key above, so future PropPose / PropDestroy
        // from sender resolves via key OR eid lookup.
        coop::remote_prop::RegisterPropMirror(payload.elementId, fuzzy, keyW, classW, senderSlot);
        return;
    }
    if (!ResolveSpawnFns()) {
        UE_LOGW("remote_prop::OnSpawn: spawn UFunctions unresolved -- dropping");
        return;
    }
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("remote_prop::OnSpawn: class '%ls' not found in GUObjectArray -- dropping (likely cooked-content class not loaded)",
                classW.c_str());
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("remote_prop::OnSpawn: no world context -- dropping");
        return;
    }
    // Build FTransform from wire rotation/scale/location. ue_wrap::FTransform
    // (types.h) is the canonical 48-byte layout matching engine FTransform;
    // RULE 2: no parallel local FTransform48 type.
    ue_wrap::FTransform xform{};  // ctor defaults: identity rot, zero loc, unit scale
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX;
    xform.TY = payload.locY;
    xform.TZ = payload.locZ;
    xform.SX = payload.scaleX;
    xform.SY = payload.scaleY;
    xform.SZ = payload.scaleZ;
    // Phase 1: BeginDeferredActorSpawnFromClass -> uninitialized AActor*.
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_beginSpawnFn);
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("remote_prop::OnSpawn: BeginDeferredActorSpawnFromClass call failed");
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("remote_prop::OnSpawn: BeginDeferred returned null");
        return;
    }
    // Phase 2 (CRITICAL): set the Key on the spawned actor BEFORE
    // FinishSpawningActor. Aprop_C.Init() runs inside FinishSpawningActor's
    // UserConstructionScript and, when ResetKey=true or no Key is set, calls
    // KismetGuidLibrary::NewGuid -> FName -> self->Key. Writing Key FIRST
    // (via the BP-callable setKey UFunction) skips that overwrite branch and
    // the prop ends up with our wire Key + registered in
    // mainGamemode.keyObj_key/obj cross-peer.
    // Audit Fix 4 (2026-05-27): resolve setKey ON THE ACTUAL SPAWNED CLASS,
    // not the cached Aprop_C.setKey UFunction*. ChipPile/clump/trashBits
    // have their OWN setKey UFunctions on different UClasses with possibly
    // different parameter layouts (per UE4 ProcessEvent, dispatching a
    // foreign-class UFunction* on an actor of a different class can
    // silently corrupt memory or crash). Per-class FindFunction is one
    // GUObjectArray walk per CLASS (not per actor); the result is cached
    // here ad-hoc since we only have ~10 keyed-interactable leaf classes.
    void* setKeyFn = R::FindFunction(actorClass, P::name::PropSetKeyFn);
    if (!setKeyFn) {
        UE_LOGW("remote_prop::OnSpawn: setKey UFunction not found on class '%ls' -- spawn will use auto-generated Key",
                classW.c_str());
    } else {
        // Convert wire Key string -> live FName via Conv_StringToName, then
        // call <class>.setKey(FName) so the receiver's prop carries the
        // SAME Key string as the sender's. Subsequent PropPose updates with
        // this key resolve via prop_wrap::FindByKeyString (which compares by
        // ToString, NOT by ComparisonIndex -- the cross-peer-stable path).
        const R::FName keyFName = ue_wrap::fname_utils::StringToFName(keyW);
        if (keyFName.ComparisonIndex == 0) {
            UE_LOGW("remote_prop::OnSpawn: StringToFName('%ls') -> NAME_None; setKey skipped",
                    keyW.c_str());
        } else {
            ParamFrame sk(setKeyFn);
            if (!sk.SetRaw(L"Key", &keyFName, sizeof(keyFName))) {
                UE_LOGW("remote_prop::OnSpawn: setKey 'Key' param not found on '%ls'",
                        classW.c_str());
            } else if (!Call(spawned, sk)) {
                UE_LOGW("remote_prop::OnSpawn: setKey ProcessEvent call failed on '%ls'",
                        classW.c_str());
            } else {
                UE_LOGI("remote_prop::OnSpawn: setKey('%ls') ok on '%ls' (FName idx=%u)",
                        keyW.c_str(), classW.c_str(), keyFName.ComparisonIndex);
            }
        }
    }
    // v5 Inc2 echo suppression: mark this actor as wire-induced BEFORE
    // FinishSpawningActor (which runs Aprop_C::Init via UserConstructionScript,
    // tripping our Init POST observer in harness.cpp). The observer's call
    // to ConsumeIncomingSpawn will then return true and skip the broadcast
    // back to the sender -> no echo loop.
    coop::prop_echo_suppress::MarkIncomingSpawn(spawned);
    // Phase 3: FinishSpawningActor -> runs UserConstructionScript + BeginPlay.
    {
        ParamFrame finish(g_finishSpawnFn);
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo, finish)) {
            UE_LOGE("remote_prop::OnSpawn: FinishSpawningActor call failed");
            return;
        }
    }
    UE_LOGI("remote_prop::OnSpawn: spawned %p of '%ls' at (%.1f, %.1f, %.1f)",
            spawned, classW.c_str(), payload.locX, payload.locY, payload.locZ);
    // Phase 4: physics state. The mesh is Aprop_C.StaticMesh.
    void* mesh = ue_wrap::prop::GetStaticMesh(spawned);
    if (mesh) {
        const bool sim = (payload.physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
        coop::remote_prop::DriveSimulate(mesh, sim);
        const bool hasLinVel =
            payload.initLinVelX != 0.f || payload.initLinVelY != 0.f || payload.initLinVelZ != 0.f;
        const bool hasAngVel =
            payload.initAngVelX != 0.f || payload.initAngVelY != 0.f || payload.initAngVelZ != 0.f;
        if (hasLinVel) coop::remote_prop::DriveSetLinearVelocity(mesh, payload.initLinVelX, payload.initLinVelY, payload.initLinVelZ);
        if (hasAngVel) coop::remote_prop::DriveSetAngularVelocity(mesh, payload.initAngVelX, payload.initAngVelY, payload.initAngVelZ);
        UE_LOGI("remote_prop::OnSpawn: physics applied (sim=%d hasLinVel=%d hasAngVel=%d)",
                sim ? 1 : 0, hasLinVel ? 1 : 0, hasAngVel ? 1 : 0);
    }
    // 2026-05-25 mushroom fall-through fix (fresh-spawn path): defensive --
    // Aprop_food_mushroom_C::Init runs as part of FinishSpawningActor and
    // CONDITIONALLY writes collision (per Init body: reads, compares to a
    // default, conditionally writes; exact condition not yet RE'd). On a
    // pure wire-spawn (no save reference, no spawnedNaturally trigger) it
    // likely lands correct, but a write here is cheap and idempotent. Keep
    // the restore call symmetric across all 3 OnSpawn convergence paths so
    // a future Init-body change can't silently regress only one path.
    RestoreCollisionIfNeeded(L"fresh-spawn", classW, spawned);
    // A2 (2026-05-29) mirror binding: bind sender's wire eid to the freshly
    // spawned local actor. Subsequent Registry::Get(eid) on this peer
    // resolves to this actor; PropDestroy with the same eid drains the
    // mirror + destroys the actor.
    coop::remote_prop::RegisterPropMirror(payload.elementId, spawned, keyW, classW, senderSlot);
    // Index the mirror's key so a PropPose drive can resolve it O(1). Essential
    // for NON-Aprop_C mirrors (garbageClump/chipPile): the cold FindByKeyString
    // fallback in ResolveLiveActorByKey walks IsDescendantOfProp (Aprop_C only),
    // so a clump mirror that isn't indexed here would never resolve -> the
    // kinematic drive could never start and the clump would appear but not
    // follow the collector's hand. Harmless + faster for Aprop_C mirrors too.
    coop::prop_element_tracker::IndexActorKey(spawned, keyW);
}

}  // namespace coop::remote_prop_spawn

// coop/props/prop_fresh_spawn.cpp -- see coop/props/prop_fresh_spawn.h.
//
// Bodies moved VERBATIM from remote_prop_spawn.cpp's fresh-spawn tail 2026-07-12 (extraction);
// the log prefixes keep the original "remote_prop::OnSpawn" wording so existing log greps stay
// valid. Behavior preserved byte-for-byte; only the parity helpers now resolve via
// coop::prop_wire_parity (their shared extracted home).

#include "coop/props/prop_fresh_spawn.h"

#include "coop/element/mirror_defer.h"        // instant-world: hide the fresh mirror until reveal
#include "coop/props/join_membership_sweep.h" // RecordClaimIfTracking (claim BEFORE any failure return)
#include "coop/props/prop_echo_suppress.h"    // ScopedMirrorSpawn + MarkIncomingSpawn
#include "coop/props/prop_element_tracker.h"  // IndexActorKey
#include "coop/props/prop_wire_parity.h"      // RestoreCollisionIfNeeded / SpParitySimulate
#include "coop/props/remote_prop.h"           // RegisterPropMirror / DriveSimulate / DriveSet*Velocity
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

namespace coop::prop_fresh_spawn {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// PropSpawn UFunction resolution (cached one-shot). Spawn path uses the
// deferred-spawn pair on UGameplayStatics CDO; setKey is on Aprop_C base.
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

}  // namespace

void* PropSetKeyFn() {
    if (!ResolveSpawnFns()) return nullptr;
    return g_propSetKeyFn;
}

void* Materialize(const coop::net::PropSpawnPayload& payload, int senderSlot,
                  const std::wstring& classW, const std::wstring& keyW,
                  const std::wstring& propNameW, bool skipBind) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    if (!ResolveSpawnFns()) {
        UE_LOGW("remote_prop::OnSpawn: spawn UFunctions unresolved -- dropping");
        return nullptr;
    }
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("remote_prop::OnSpawn: class '%ls' not found in GUObjectArray -- dropping (likely cooked-content class not loaded)",
                classW.c_str());
        return nullptr;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("remote_prop::OnSpawn: no world context -- dropping");
        return nullptr;
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
    // ScopedMirrorSpawn: this UFunction call dispatches through ProcessEvent,
    // so the peer-symmetric ambient broadcaster's BeginDeferred POST fires
    // INSIDE it (before the actor exists to MarkIncomingSpawn) -- the scope is
    // its re-entrancy guard (owner-mirror 2026-07-10, see prop_echo_suppress.h).
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn mirrorScope;
        ParamFrame begin(g_beginSpawnFn);
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("remote_prop::OnSpawn: BeginDeferredActorSpawnFromClass call failed");
            return nullptr;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("remote_prop::OnSpawn: BeginDeferred returned null");
        return nullptr;
    }
    // P2 claim -- BEFORE any later failure return (audit CRITICAL 2026-06-10):
    // a fresh wire spawn of an RNG-divergent class is itself a live actor of a
    // sweep-target class. Claiming here (not after FinishSpawningActor) means
    // a FinishSpawningActor failure leaks one claimed half-spawned actor --
    // the exact pre-P2 behavior of that failure path -- instead of leaving an
    // unclaimed half-CONSTRUCTED actor for the sweep to K2_DestroyActor
    // mid-construction (no BeginPlay, unregistered PhysX body -> crash risk).
    coop::join_membership_sweep::RecordClaimIfTracking(spawned);
    // Phase 2 (CRITICAL): set the Key on the spawned actor BEFORE
    // FinishSpawningActor. Aprop_C.Init() runs inside FinishSpawningActor's
    // UserConstructionScript and, when ResetKey=true or no Key is set, calls
    // KismetGuidLibrary::NewGuid -> FName -> self->Key. Writing Key FIRST
    // (via the BP-callable setKey UFunction) skips that overwrite branch and
    // the prop ends up with our wire Key + registered in
    // mainGamemode.keyObj_key/obj cross-peer.
    // Audit Fix 4 (2026-05-27): resolve setKey ON THE ACTUAL SPAWNED CLASS
    // first -- ChipPile/clump/trashBits have their OWN setKey UFunctions on
    // different UClasses with possibly different parameter layouts (per UE4
    // ProcessEvent, dispatching a foreign-class UFunction* on an actor of a
    // different class can silently corrupt memory or crash).
    // 2026-07-11 crowbar-dupe RCA: FindFunction is EXACT-OWNER (no SuperStruct
    // climb, [[lesson-findfunction-exact-owner-no-superstruct-climb]]), so the
    // leaf-only resolve MISSED every Aprop_C subclass that does not redeclare
    // setKey (prop_crowbar_C): the mirror spawned keyless -> Init minted a
    // NewGuid -> the actor's FIELD key diverged from its wire binding -> the
    // client's later pickup-destroy broadcast carried the minted key -> the
    // host found no match -> its authoritative copy survived = the host-side
    // dupe. Fall back to the base-resolved g_propSetKeyFn for Aprop_C
    // descendants (its declaring class; the fuzzy-rekey path already calls it
    // on leaf instances -- proven safe live at 11:54:45 in the RCA logs). See
    // research/findings/props-lifecycle/votv-crowbar-mirror-key-divergence-RCA-2026-07-11.md.
    void* setKeyFn = R::FindFunction(actorClass, P::name::PropSetKeyFn);
    if (!setKeyFn && ue_wrap::prop::IsClassDescendantOfProp(actorClass)) {
        setKeyFn = g_propSetKeyFn;
        if (setKeyFn)
            UE_LOGI("remote_prop::OnSpawn: setKey resolved on the Aprop_C base for leaf '%ls'",
                    classW.c_str());
    }
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
    // v54 SP-parity identity (white-cube root cause, RE 2026-06-10): write the
    // list_props row `Name` + the Static/removeWOrespawn/frozen/sleep bools on
    // the DEFERRED actor BEFORE FinishSpawningActor -- Finish runs the UCS ->
    // Aprop_C::init() pass which resolves list_props[Name] into the true mesh/
    // mass/collision/SetSimulatePhysics(!(Static||frozen||sleep)). Without the
    // Name, a generic prop_C mirror constructs as the CDO 'cube' row (the
    // host's broken-cubicle wall panels mirrored as white cubes). Same field
    // set + ordering SP's own loadObjects->loadData->init() achieves, minus
    // the cube flash (SP writes AFTER Finish and re-runs init()).
    if (ue_wrap::prop::IsDescendantOfProp(spawned)) {
        R::FName nameRow{0, 0};
        if (!propNameW.empty() && propNameW != L"None") {
            nameRow = ue_wrap::fname_utils::StringToFName(propNameW);
            if (nameRow.ComparisonIndex == 0) {
                UE_LOGW("remote_prop::OnSpawn: StringToFName(propName '%ls') -> NAME_None; "
                        "mirror keeps its class-default Name (may render as the CDO mesh)",
                        propNameW.c_str());
            }
        }
        namespace pf = coop::net::propspawn_flags;
        ue_wrap::prop::WriteSpParityIdentity(
            spawned, nameRow,
            (payload.physFlags & pf::kStatic) != 0,
            (payload.physFlags & pf::kRemoveWOrespawn) != 0,
            (payload.physFlags & pf::kFrozen) != 0,
            (payload.physFlags & pf::kSleep) != 0);
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
            return nullptr;
        }
    }
    UE_LOGI("remote_prop::OnSpawn: spawned %p of '%ls' at (%.1f, %.1f, %.1f)",
            spawned, classW.c_str(), payload.locX, payload.locY, payload.locZ);
    // Ambient owner-effect mirrors (pinecone/stick/crystal, 2026-07-10): the
    // NATIVE prop self-expires via the spawner's SetLifeSpan(600); the mirror's
    // despawn normally arrives via the owner's death-watch PropDestroy, but an
    // owner that DISCONNECTS leaves the mirror orphaned forever. SP-parity
    // backstop: give the mirror its own lifespan (900 s > the native 600 s, so
    // the wire destroy always wins in the normal case and an orphan self-reaps).
    if (payload.key.len == 0) {
        for (size_t i = 0; i < P::name::kAmbientPropSpawnMirrorClassesSize; ++i) {
            if (classW != P::name::kAmbientPropSpawnMirrorClasses[i]) continue;
            // SetLifeSpan lives on AActor -- FindFunction is exact-owner (no
            // SuperStruct climb; audit 2026-07-10 CRITICAL: the leaf-class
            // lookup returned null every call AND paid a futile full-array
            // walk per mirror). Resolve ONCE on the Actor class, latch.
            static void* s_lifeFn = nullptr;
            static bool  s_lifeTried = false;
            if (!s_lifeTried) {   // GT-only path (event drain) -- plain statics
                s_lifeTried = true;
                if (void* actorCls = R::FindClass(P::name::ActorClassName))
                    s_lifeFn = R::FindFunction(actorCls, L"SetLifeSpan");
                if (!s_lifeFn)
                    UE_LOGW("remote_prop::OnSpawn: Actor.SetLifeSpan not resolved -- "
                            "ambient-mirror orphan backstop disabled");
            }
            if (s_lifeFn) {
                ParamFrame life(s_lifeFn);
                if (life.Set<float>(L"InLifespan", 900.f)) Call(spawned, life);
            }
            break;
        }
    }
    // Stamp the trash VARIANT + keep the mirror clump from self-converting.
    //
    // The former position-based "consume the co-located source pile" here was REMOVED
    // 2026-06-08 (RULE 2): chipPiles are NOT co-located cross-peer -- AundergroundGarbage
    // Spawner places them with the global UNSEEDED RNG, and the client boots a blank save
    // -- so FindNearestChipPile here matched the WRONG pile (or none), which was the
    // dominant clump-DUPE source (it destroyed an unrelated pile while the landed pile
    // spawned anyway -> net multiplication). v52: the ball->pile convert is now ONE atomic
    // PropConvert (destroy ball by oldEid + spawn pile by newEid), and a re-grabbed pile
    // drops its cross-peer mirror by IDENTITY via the trash_collect_sync mirror-pile
    // death-watch -> PropDestroy(eid). The wire chipType (read off the held clump at grab
    // time) is authoritative for the variant.
    // research/findings/piles-trash/votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md.
    const uint8_t variant = payload.chipType;
    if (classW.find(L"garbageClump") != std::wstring::npos) {
        // Silence THIS mirror clump's own ground-hit -> turn-to-pile handler.
        // FinishSpawningActor (above) auto-binds the StaticMesh OnComponentHit delegate
        // (prop_garbageClump ubergraph 2702 -> BeginDeferredActorSpawnFromClass(pile)). On
        // release we re-enable collision+physics+throw velocity so the mirror flies + lands
        // HERE; without this guard that handler fires -> a SECOND pile atop the owner's
        // authoritative one. Disabling hit-notify lets the mirror land visually but never
        // self-convert; the owner's death-watch (trash_collect_sync) stays the sole pile
        // source. (canConvert=false@0x024C does NOT work -- the hit handler re-sets it true.)
        ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(spawned, false);
    }
    // Stamp the variant after FinishSpawningActor (actor fully constructed). No-op for
    // non-trash classes (SetChipType reflection-gates on a chipType property, so it never
    // touches an Aprop_C's StaticMesh ptr at the same 0x0238 offset). Repaints via setTex().
    if (variant != 0) {
        ue_wrap::prop::SetChipType(spawned, variant);
        UE_LOGI("remote_prop::OnSpawn: applied chipType=%u variant to '%ls'",
                static_cast<unsigned>(variant), classW.c_str());
    }
    // (v52 RULE 1+2: the former kFreshLanded -> turnToPile(landingVel) call here was a
    // CATASTROPHIC BUG, removed. The comment claimed turnToPile "operates on `this`, spawns
    // nothing" -- the disassembly proves the OPPOSITE: actorChipPile_C::turnToPile is the
    // pile->clump GRAB morph -- it BeginDeferred-spawns a clump (Max:=2.0), throws it with the
    // velocity, and K2_DestroyActor's SELF. So calling it on a freshly-spawned landed pile
    // DESTROYED that pile (-> ResolveLiveActorByEid failed -> the mirror was unwatched + an
    // incoming PropDestroy found "no local actor") AND spawned a stray, untracked, self-
    // converting clump -> the persistent clump DUPE. A landed pile needs no morph: it just
    // spawns and sits. The impact dust+sound is a deferred polish (needs the correct verb, NOT
    // the grab morph). kFreshLanded + TurnChipPileToPile retired with it.)
    // Phase 4: physics state. The mesh is Aprop_C.StaticMesh.
    void* mesh = ue_wrap::prop::GetStaticMesh(spawned);
    if (mesh) {
        // SP-parity (2026-06-10 grabbability fix): a settled normal prop is
        // simulate-ENABLED + asleep in SP -- forcing the mirror kinematic made
        // it ungrabbable on this peer. kSimulatePhysics (host body live-awake)
        // implies the formula's true case; static/frozen/sleep stay disabled.
        const bool sim = coop::prop_wire_parity::SpParitySimulate(payload.physFlags);
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
    coop::prop_wire_parity::RestoreCollisionIfNeeded(L"fresh-spawn", classW, spawned);
    // (kAtRest sleep REVERTED 2026-06-09 -- see the exact-key branch in remote_prop_spawn.)
    // v81 MORPH V2: a convert re-skins eid E in place and the rebind path is local-vs-mirror
    // specific (RebindLocalElementActor vs RegisterPropMirror rebindInPlace), so OnConvert binds
    // explicitly -- the caller passes skipBind=true and binds the returned actor itself.
    if (skipBind) return spawned;
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
    // instant-world: hide the freshly-spawned + registered prop mirror until reveal (AFTER the bind so it is
    // always enumerable). Real prop -> collisionOff=true (hide alone leaves collision on; a hidden mirror
    // must not be grab-trace-hittable / physics-active). hasMatchPos => a save-time-keyed form whose local
    // twin is still visible -> HOLD to quiescence; else (host-only/derived form, no twin) reveal at the lift.
    coop::mirror_defer::OnMirrorSpawned(payload.elementId, spawned, /*collisionOff=*/true,
                                        /*holdUntilQuiescence=*/payload.hasMatchPos != 0);
    // (The chipPile MIRROR used to be enrolled in the mirror-pile death-watch here. RETIRED
    // 2026-06-17, RULE 1+2: the death-watch's near-camera "grabbed" inference was unsound -- a peer
    // bumping a pile until it died near the camera read as a grab and wiped the pile on both peers
    // ("piles destroyed when touched a lot"). A peer's later grab of THIS mirror is now caught by the
    // InpActEvt_use PRE observer (trash_collect_sync) reading lookAtActor=pile -> ResolveMirrorEidByActor
    // (RegisterPropMirror bound it to payload.elementId) -> PropDestroy(eid). Covers the
    // pre-existing / snapshot pile case too, since the resolve works for any registered mirror. Fires
    // only on a real E-press, never a bump/stream-out/physics-death.)
    return spawned;
}

}  // namespace coop::prop_fresh_spawn

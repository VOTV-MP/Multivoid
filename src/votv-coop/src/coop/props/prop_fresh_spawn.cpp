// coop/props/prop_fresh_spawn.cpp -- see coop/props/prop_fresh_spawn.h. The log prefixes keep
// the remote_prop::OnSpawn wording so log greps stay valid; the parity helpers resolve through
// prop_wire_parity.

#include "coop/props/prop_fresh_spawn.h"

#include "coop/element/mirror_defer.h"        // instant-world: hide the fresh mirror until reveal
#include "coop/props/join_membership_sweep.h" // RecordClaimIfTracking (claim BEFORE any failure return)
#include "coop/props/prop_echo_suppress.h"    // ScopedMirrorSpawn + MarkIncomingSpawn
#include "coop/props/prop_element_tracker.h"  // IndexActorKey
#include "coop/props/prop_wire_parity.h"      // RestoreCollisionIfNeeded / SpParitySimulate
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"           // RegisterPropMirror / DriveSimulate / DriveSet*Velocity
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

namespace coop::prop_fresh_spawn {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// The spawn UFunction resolution, cached one-shot: the deferred-spawn pair on the
// GameplayStatics CDO, and setKey on the prop base.
ue_wrap::CachedObjRef g_gsCdo;   // a slot-validated self-healing cache
void* g_beginSpawnFn     = nullptr;
void* g_finishSpawnFn    = nullptr;
void* g_propSetKeyFn     = nullptr;
bool  g_spawnResolved    = false;

bool ResolveSpawnFns() {
    if (g_spawnResolved && g_gsCdo.Alive()) return true;
    g_gsCdo.Set(R::FindClassDefaultObject(P::name::GameplayStaticsClass));
    if (!g_gsCdo.Raw()) return false;
    void* cls = R::ClassOf(g_gsCdo.Raw());
    if (!cls) return false;
    g_beginSpawnFn  = R::FindFunction(cls, P::name::BeginDeferredSpawnFn);
    g_finishSpawnFn = R::FindFunction(cls, P::name::FinishSpawningActorFn);
    if (!g_beginSpawnFn || !g_finishSpawnFn) {
        UE_LOGW("remote_prop::OnSpawn: GameplayStatics spawn fns missing (begin=%p finish=%p)",
                g_beginSpawnFn, g_finishSpawnFn);
        return false;
    }
    // setKey on the prop base. The class may not be loaded at the first call (the engine loads
    // blueprint classes on demand); a miss is retried on the next call.
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
    // Build the transform from the wire rotation, scale and location; the wrapper's FTransform is
    // the canonical 48-byte engine layout.
    ue_wrap::FTransform xform{};  // ctor defaults: identity rot, zero loc, unit scale
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX;
    xform.TY = payload.locY;
    xform.TZ = payload.locZ;
    xform.SX = payload.scaleX;
    xform.SY = payload.scaleY;
    xform.SZ = payload.scaleZ;
    // Phase 1: the deferred spawn, an uninitialised actor. The mirror-spawn scope: this UFunction
    // call dispatches through ProcessEvent, so the ambient broadcaster's spawn POST observer fires
    // inside it, before the actor exists to be marked incoming; the scope is its re-entrancy guard
    // (see prop_echo_suppress.h).
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
        if (!Call(g_gsCdo.Raw(), begin)) {
            UE_LOGE("remote_prop::OnSpawn: BeginDeferredActorSpawnFromClass call failed");
            return nullptr;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("remote_prop::OnSpawn: BeginDeferred returned null");
        return nullptr;
    }
    // The claim, before any later failure return: a fresh wire spawn of a divergent class is
    // itself a live actor of a sweep-target class. Claiming here rather than after the finish
    // means a finish failure leaks one claimed half-spawned actor, instead of leaving an
    // unclaimed half-constructed actor for the sweep to destroy mid-construction (no BeginPlay,
    // an unregistered physics body, a crash risk).
    coop::join_membership_sweep::RecordClaimIfTracking(spawned);
    // Phase 2: set the key on the spawned actor before the finish. The prop's init runs inside
    // the finish's construction script and, with no key set, mints a random key into the field;
    // writing the key first through the blueprint-callable setter skips that branch, and the prop
    // ends up with our wire key, registered in the gamemode's key maps cross-peer. setKey is
    // resolved on the actual spawned class first, since the pile, clump and trash classes have
    // their own setKey UFunctions on different classes with possibly different parameter layouts,
    // and dispatching a foreign class's UFunction on an actor can corrupt memory. The lookup is
    // exact-owner, no superclass climb, so a leaf that does not redeclare setKey (the crowbar)
    // falls back to the base-resolved one for prop descendants; without the fallback such a
    // mirror spawned keyless, the init minted a random key, the actor's field key diverged from its
    // wire binding, the client's later pickup-destroy carried the minted key, and the host's
    // authoritative copy survived, a host-side dupe.
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
        // Convert the wire key string to a live FName, then call the class's setKey, so the
        // receiver's prop carries the same key string as the sender's. Later pose updates with this
        // key resolve by string compare, the cross-peer-stable path, not by comparison index.
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
    // The single-player-parity identity: write the props-table row name and the static,
    // remove-without-respawn, frozen and sleep flags on the deferred actor before the finish,
    // whose construction pass resolves the table row into the true mesh, mass, collision and
    // physics state. Without the name a generic prop mirror constructs as the default cube row
    // (wall panels mirrored as white cubes). The same field set and ordering the game's own load
    // achieves, minus the cube flash: the game writes after the finish and re-runs the init.
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
    // Echo suppression: mark this actor as wire-induced before the finish, which runs the prop's
    // init through the construction script and trips our init POST observer; the observer's
    // consume then returns true and skips the broadcast back to the sender, so there is no echo
    // loop.
    coop::prop_echo_suppress::MarkIncomingSpawn(spawned);
    // Phase 3: the finish, which runs the construction script and BeginPlay.
    {
        ParamFrame finish(g_finishSpawnFn);
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo.Raw(), finish)) {
            UE_LOGE("remote_prop::OnSpawn: FinishSpawningActor call failed");
            return nullptr;
        }
    }
    UE_LOGI("remote_prop::OnSpawn: spawned %p of '%ls' at (%.1f, %.1f, %.1f)",
            spawned, classW.c_str(), payload.locX, payload.locY, payload.locZ);
    // Ambient owner-effect mirrors (the pinecone, stick and crystal): the native prop
    // self-expires by the spawner's lifespan, and the mirror's despawn normally arrives through
    // the owner's death-watch destroy, but an owner that disconnects would leave the mirror
    // orphaned forever. So the mirror gets its own lifespan, longer than the native one, so the
    // wire destroy wins in the normal case and an orphan self-reaps.
    if (payload.key.len == 0) {
        for (size_t i = 0; i < P::name::kAmbientPropSpawnMirrorClassesSize; ++i) {
            if (classW != P::name::kAmbientPropSpawnMirrorClasses[i]) continue;
            // SetLifeSpan lives on the actor base and the lookup is exact-owner, so a leaf-class
            // lookup returned null every call and paid a futile full-array walk per mirror.
            // Resolved once on the actor class, latched.
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
    // Stamp the trash variant and keep the mirror clump from self-converting. The wire chipType,
    // read off the held clump at grab time, is authoritative for the variant; chipPiles are not
    // co-located cross-peer (the spawner places them with the unseeded RNG and the client boots a
    // blank save), so nothing here matches a source pile by position, and the ball-to-pile convert
    // is one atomic PropConvert.
    const uint8_t variant = payload.chipType;
    if (classW.find(L"garbageClump") != std::wstring::npos) {
        // Silence this mirror clump's own ground-hit-to-pile handler. The finish auto-binds the
        // mesh's hit delegate; on release the mirror's collision, physics and throw velocity are
        // re-enabled so it flies and lands here, and without this guard that handler would spawn a
        // second pile atop the owner's authoritative one. Disabling hit notification lets the
        // mirror land visually but never self-convert; the owner stays the sole pile source.
        // Clearing the convert flag does not work, since the hit handler sets it again.
        ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(spawned, false);
    }
    // Stamp the variant after the finish, the actor fully constructed. A no-op for non-trash
    // classes: the setter reflection-gates on the chipType property, so it never touches a plain
    // prop's mesh pointer at the same offset. Repaints through the texture setter.
    if (variant != 0) {
        ue_wrap::prop::SetChipType(spawned, variant);
        UE_LOGI("remote_prop::OnSpawn: applied chipType=%u variant to '%ls'",
                static_cast<unsigned>(variant), classW.c_str());
    }
    // The prop's own save record, if it is already parked for this Key. This is the declared
    // readiness point: the record's own key field is the saved identity, so a mirror that takes it
    // ends up with the state AND the identity the authority has, rather than a fresh mint from the
    // spawn seam. A record still in flight lands on this actor by Key when it arrives.
    if (!keyW.empty()) coop::prop_save_data::ApplyParked(spawned, keyW);
    // A landed pile needs no morph: it spawns and sits. turnToPile is the pile-to-clump grab morph
    // (it spawns a clump, throws it and destroys the pile), so calling it on a fresh pile
    // destroyed the pile and spawned a stray self-converting clump, the persistent clump dupe.
    // The impact dust and sound are deferred polish. Phase 4: the physics state; the mesh is the
    // prop's static mesh.
    void* mesh = ue_wrap::prop::GetStaticMesh(spawned);
    if (mesh) {
        // Single-player parity: a settled normal prop is simulate-enabled and asleep, and forcing
        // the mirror kinematic made it ungrabbable on this peer. A live-awake host body implies
        // simulation; static, frozen and sleep stay disabled.
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
    // The mushroom fall-through guard, defensive: the mushroom's init runs as part of the finish
    // and conditionally writes collision. A pure wire spawn likely lands correct, but the write is
    // cheap and idempotent, and the restore call is kept symmetric across all three spawn
    // convergence paths so an init-body change cannot regress only one.
    coop::prop_wire_parity::RestoreCollisionIfNeeded(L"fresh-spawn", classW, spawned);
    // A convert re-skins an eid in place and the rebind path is local-versus-mirror specific, so
    // the convert receiver binds explicitly: the caller passes skipBind and binds the returned
    // actor itself.
    if (skipBind) return spawned;
    // Bind the sender's wire eid to the freshly spawned local actor: a Registry lookup by eid on
    // this peer resolves to it, and a destroy with the same eid drains the mirror and destroys the
    // actor.
    coop::remote_prop::RegisterPropMirror(payload.elementId, spawned, keyW, classW, senderSlot);
    // Index the mirror's key so a pose drive resolves it in constant time. Essential for the
    // non-prop mirrors (clumps and piles): the cold fallback walks prop descendants only, so an
    // unindexed clump mirror would never resolve, the kinematic drive could never start, and the
    // clump would appear but not follow the collector's hand. Harmless and faster for prop
    // mirrors too.
    coop::prop_element_tracker::IndexActorKey(spawned, keyW);
    // Hide the freshly spawned and registered mirror until the reveal, after the bind so it is
    // always enumerable. A real prop: collision off too (hiding alone leaves collision on, and a
    // hidden mirror must not be grab-trace-hittable or physics-active). A matched position means
    // a save-time-keyed form whose local twin is still visible, so hold to quiescence; otherwise
    // reveal at the lift.
    coop::mirror_defer::OnMirrorSpawned(payload.elementId, spawned, /*collisionOff=*/true,
                                        /*holdUntilQuiescence=*/payload.hasMatchPos != 0);
    // A peer's later grab of this mirror is caught by the use-press observer (trash_collect_sync)
    // reading the looked-at pile, resolving the mirror's eid from its registration and
    // broadcasting a destroy by eid. It covers the pre-existing and snapshot pile cases too, since
    // the resolve works for any registered mirror, and fires only on a real press, never a bump, a
    // stream-out or a physics death.
    return spawned;
}

}  // namespace coop::prop_fresh_spawn

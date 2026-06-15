// ue_wrap/engine_attach.cpp -- generic actor root-physics substrate (Principle 7
// engine wrapper; no network/gameplay state).
//
// These operate on an actor's ROOT primitive component via K2_GetRootComponent --
// NEVER the Aprop_C-specific StaticMesh @0x238 -- so they work on the non-Aprop_C
// trash clump (ue_wrap::prop::GetStaticMesh returns null for it, the 2a-UAF safety
// gate). The held-clump mirror (coop::remote_prop) uses these to go kinematic while
// held (SetSimulatePhysics false) and re-enable physics + apply the throw velocity on
// release -- the mannequin model for the non-keyable clump.
// [[project-bug-trash-chippile-uaf-crash]]
//
// (History: this file also hosted a hand-attach model -- AttachActorToPuppetHand etc.
// -- retired 2026-06-03 / v26 / RULE 2 when the clump moved to the prop pose pipeline.
// The generic physics helpers below are the part that survived; only the receiver's
// physics toggle needed them.)

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

// Cached UFunctions (resolve once; re-resolve if the owning class is freed). Plain
// void* caches, game-thread only.
void* g_getRootFn = nullptr;  // Actor::K2_GetRootComponent
void* g_setSimFn  = nullptr;  // PrimitiveComponent::SetSimulatePhysics
void* g_getLinFn  = nullptr;  // PrimitiveComponent::GetPhysicsLinearVelocity
void* g_getAngFn  = nullptr;  // PrimitiveComponent::GetPhysicsAngularVelocityInDegrees
void* g_setLinFn  = nullptr;  // PrimitiveComponent::SetPhysicsLinearVelocity
void* g_setAngFn  = nullptr;  // PrimitiveComponent::SetPhysicsAngularVelocityInDegrees
void* g_setCollFn = nullptr;  // PrimitiveComponent::SetCollisionEnabled
void* g_setNotifyHitFn = nullptr;  // PrimitiveComponent::SetNotifyRigidBodyCollision
void* g_isAwakeFn = nullptr;  // PrimitiveComponent::IsAnyRigidBodyAwake
void* g_putSleepFn = nullptr;  // PrimitiveComponent::PutRigidBodyToSleep
void* g_getMatFn  = nullptr;  // PrimitiveComponent::GetMaterial
void* g_getPhysMatFn = nullptr;  // MaterialInterface::GetPhysicalMaterial
void* g_primCompClass = nullptr;  // the PrimitiveComponent UClass (root-type gate)
void* g_attachCompFn = nullptr;  // Actor::K2_AttachToComponent
void* g_detachFn     = nullptr;  // Actor::K2_DetachFromActor

void* ActorFn(void** cache, const wchar_t* name) {
    if (!*cache) {
        if (void* c = R::FindClass(L"Actor")) *cache = R::FindFunction(c, name);
    }
    return *cache;
}
void* PrimFn(void** cache, const wchar_t* name) {
    if (!*cache) {
        if (void* c = R::FindClass(L"PrimitiveComponent")) *cache = R::FindFunction(c, name);
    }
    return *cache;
}

// Resolve `actor`'s root component (USceneComponent*) via K2_GetRootComponent.
// null on failure. Game thread.
void* RootComponentOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* fn = ActorFn(&g_getRootFn, L"K2_GetRootComponent");
    if (!fn) return nullptr;
    ParamFrame f(fn);
    if (!Call(actor, f)) return nullptr;
    void* root = f.Get<void*>(L"ReturnValue");
    return (root && R::IsLive(root)) ? root : nullptr;
}

}  // namespace

bool SetActorSimulatePhysics(void* actor, bool simulate) {
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* fn = PrimFn(&g_setSimFn, L"SetSimulatePhysics");
    if (!fn) { UE_LOGW("engine: SetSimulatePhysics unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<bool>(L"bSimulate", simulate);
    return Call(root, f);
}

bool GetActorRootPhysicsVelocity(void* actor, FVector& outLin, FVector& outAng) {
    outLin = FVector{}; outAng = FVector{};
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* getLin = PrimFn(&g_getLinFn, L"GetPhysicsLinearVelocity");
    void* getAng = PrimFn(&g_getAngFn, L"GetPhysicsAngularVelocityInDegrees");
    if (!getLin || !getAng) return false;
    // BoneName defaults to NAME_None (the root body) -- ParamFrame zero-inits it.
    { ParamFrame f(getLin); if (!Call(root, f)) return false; outLin = f.Get<FVector>(L"ReturnValue"); }
    { ParamFrame f(getAng); if (!Call(root, f)) return false; outAng = f.Get<FVector>(L"ReturnValue"); }
    return true;
}

bool SetActorRootCollisionEnabled(void* actor, uint8_t collisionType) {
    // collisionType: 0=NoCollision 1=QueryOnly 2=PhysicsOnly 3=QueryAndPhysics.
    // The thrown clump mirror needs 3 so it collides with the world + lands (the
    // bare-spawned mirror otherwise sinks through the floor on release).
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* fn = PrimFn(&g_setCollFn, L"SetCollisionEnabled");
    if (!fn) { UE_LOGW("engine: SetCollisionEnabled unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<uint8_t>(L"NewType", collisionType);
    return Call(root, f);
}

bool SetActorRootNotifyRigidBodyCollision(void* actor, bool notify) {
    // Toggle the root primitive's OnComponentHit notification. Disabling it stops the BP's
    // ComponentHit-bound events from firing without changing whether the body physically
    // collides. Used to silence a mirror trash-clump's own ground-hit handler (which would
    // otherwise BeginDeferredActorSpawnFromClass(pile) on landing -> a DUPLICATE pile on top
    // of the host's authoritative one). The clump's StaticMesh is its root (the same body the
    // root-based collision/velocity setters drive on release), so this targets it. Game thread.
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* fn = PrimFn(&g_setNotifyHitFn, L"SetNotifyRigidBodyCollision");
    if (!fn) { UE_LOGW("engine: SetNotifyRigidBodyCollision unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<bool>(L"bNewNotifyRigidBodyCollision", notify);
    return Call(root, f);
}

bool IsActorRootBodyAtRest(void* actor) {
    // True ONLY if we positively confirmed the root rigid body is at rest (no body
    // awake -- covers both an asleep simulating body and a non-simulating one).
    // Returns false on ANY resolution failure: the host uses this to decide whether
    // to stamp kAtRest, and we must NEVER claim a rest we couldn't verify (a false
    // kAtRest would tell the client to sleep a body the host actually has moving).
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* fn = PrimFn(&g_isAwakeFn, L"IsAnyRigidBodyAwake");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!Call(root, f)) return false;
    return !f.Get<bool>(L"ReturnValue");
}

bool PutActorRootBodyToSleep(void* actor) {
    // Force the root rigid body to sleep (PutRigidBodyToSleep, BoneName=NAME_None =
    // the root body). Used on the client right after teleport-converging a kAtRest
    // prop so it lands at the host's authoritative position and immediately returns
    // to the rest state the host observed -- no transient settle, no permanent
    // active-island physics cost. The body stays a DYNAMIC physics body (still
    // grabbable / collidable / wakes-on-touch) -- this is NOT SetSimulatePhysics(false)
    // (that path is the kinematic grab-drive, scoped to held props). No-op + false
    // on failure. Generic (root component, not the Aprop_C mesh offset). Game thread.
    void* root = RootComponentOf(actor);
    if (!root) return false;
    // SAFETY GATE (2026-06-09, smoke-caught AV): PutRigidBodyToSleep is only valid on a
    // DYNAMIC simulating body -- in PhysX putToSleep on a static/kinematic/bodyless actor
    // AVs. The host stamps kAtRest from IsAnyRigidBodyAwake==false, which is ALSO true for
    // static/kinematic props (no dynamic body) -- so we must NOT blindly sleep every
    // kAtRest prop. Only an AWAKE body is guaranteed dynamic-simulating, AND an awake body
    // is exactly what we want to quiet (the teleport-WOKEN divergent prop). A not-awake
    // body is already asleep or non-dynamic -> nothing to do. So gate the sleep on awake.
    void* awakeFn = PrimFn(&g_isAwakeFn, L"IsAnyRigidBodyAwake");
    if (!awakeFn) return false;
    { ParamFrame fa(awakeFn);
      if (!Call(root, fa) || !fa.Get<bool>(L"ReturnValue")) return false; }
    void* fn = PrimFn(&g_putSleepFn, L"PutRigidBodyToSleep");
    if (!fn) { UE_LOGW("engine: PutRigidBodyToSleep unresolved"); return false; }
    ParamFrame f(fn);
    // BoneName defaults to NAME_None (the root body) -- ParamFrame zero-inits it.
    return Call(root, f);
}

bool SetActorRootPhysicsVelocity(void* actor, const FVector& lin, const FVector& ang) {
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* setLin = PrimFn(&g_setLinFn, L"SetPhysicsLinearVelocity");
    void* setAng = PrimFn(&g_setAngFn, L"SetPhysicsAngularVelocityInDegrees");
    if (!setLin || !setAng) return false;
    { ParamFrame f(setLin);
      f.Set<FVector>(L"NewVel", lin);
      f.Set<bool>(L"bAddToCurrent", false);
      Call(root, f); }
    { ParamFrame f(setAng);
      f.Set<FVector>(L"NewAngVel", ang);
      f.Set<bool>(L"bAddToCurrent", false);
      Call(root, f); }
    return true;
}

void* GetActorRootPhysicalMaterial(void* actor) {
    // Root surface -> physical material, the same resolution a blocking trace's
    // hit.PhysMat does on the hit face. Feeds lib_C::physSound for actors that are
    // NOT prop_C descendants (the trash clump derives from plain Actor -- it has no
    // physicsImpact component to read a cached PhysMat from).
    void* root = RootComponentOf(actor);
    if (!root) return nullptr;
    // Type-gate: GetMaterial is a PrimitiveComponent UFunction; PE-dispatching it
    // on a plain SceneComponent root would thunk into a bad P_THIS cast. (The
    // physics setters above skip this gate because their targets are exclusively
    // our own clump mirrors; this one receives arbitrary wire-grabbed actors.)
    // Cache safety: PrimitiveComponent is a NATIVE ENGINE UClass (RF_Native |
    // RF_Standalone) -- never GC'd, so the latched pointer cannot dangle. Do NOT
    // copy this latch shape for a BLUEPRINT class (those reload on level travel;
    // they need the reflection.cpp stale-detect approach instead).
    if (!g_primCompClass) g_primCompClass = R::FindClass(L"PrimitiveComponent");
    if (!g_primCompClass ||
        !R::IsDescendantOfAny(R::ClassOf(root), &g_primCompClass, 1)) return nullptr;
    void* getMat = PrimFn(&g_getMatFn, L"GetMaterial");
    if (!getMat) return nullptr;
    void* mat = nullptr;
    { ParamFrame f(getMat);
      f.Set<int32_t>(L"ElementIndex", 0);
      if (!Call(root, f)) return nullptr;
      mat = f.Get<void*>(L"ReturnValue"); }
    if (!mat || !R::IsLive(mat)) return nullptr;
    // GetPhysicalMaterial is virtual on UMaterialInterface (instance walks to its
    // parent material's assignment) -- the native thunk handles the dispatch.
    if (!g_getPhysMatFn) {
        if (void* c = R::FindClass(L"MaterialInterface"))
            g_getPhysMatFn = R::FindFunction(c, L"GetPhysicalMaterial");
    }
    if (!g_getPhysMatFn) return nullptr;
    ParamFrame f(g_getPhysMatFn);
    if (!Call(mat, f)) return nullptr;
    void* physmat = f.Get<void*>(L"ReturnValue");
    return (physmat && R::IsLive(physmat)) ? physmat : nullptr;
}

bool AttachActorToComponentSocket(void* actor, void* component, const wchar_t* socket) {
    // Generic socket-attach (distinct from AttachActorToRagdollBody, which resolves a body
    // skeletal mesh + pelvis bone): attach `actor` directly to `component` at the named
    // SOCKET, SNAPPED to the socket transform so it follows the socket each frame. Used for
    // the Killer Wisp grab-hold (victim puppet -> wisp mesh 'playerGrab' socket). KeepWorld
    // scale so the attached actor does not inherit the wisp's scale. Game thread.
    if (!actor || !R::IsLive(actor) || !component || !R::IsLive(component)) return false;
    void* fn = ActorFn(&g_attachCompFn, L"K2_AttachToComponent");
    if (!fn) { UE_LOGW("engine: K2_AttachToComponent unresolved -- cannot socket-attach"); return false; }
    R::FName sock = socket ? ue_wrap::fname_utils::StringToFName(socket) : R::FName{};
    ParamFrame f(fn);
    f.Set<void*>(L"Parent", component);
    f.SetRaw(L"SocketName", &sock, sizeof(sock));
    f.Set<uint8_t>(L"LocationRule", uint8_t{2});       // EAttachmentRule::SnapToTarget (sit at the socket)
    f.Set<uint8_t>(L"RotationRule", uint8_t{2});       // SnapToTarget (follow the socket orientation)
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld (do NOT inherit the wisp scale)
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(actor, f);
}

bool DetachActorFromParent(void* actor) {
    // Generic detach (K2_DetachFromActor, KeepWorld) -- leaves `actor` where the attach
    // left it so a subsequent pose-drive / ragdoll takes over cleanly. Game thread.
    if (!actor || !R::IsLive(actor)) return false;
    void* fn = ActorFn(&g_detachFn, L"K2_DetachFromActor");
    if (!fn) { UE_LOGW("engine: K2_DetachFromActor unresolved -- cannot detach"); return false; }
    ParamFrame f(fn);
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});       // EDetachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});       // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld
    return Call(actor, f);
}

}  // namespace ue_wrap::engine

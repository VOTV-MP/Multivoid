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

}  // namespace ue_wrap::engine

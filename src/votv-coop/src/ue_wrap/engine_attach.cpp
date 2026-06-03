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

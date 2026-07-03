// ue_wrap/engine_playerragdoll.cpp -- AplayerRagdoll_C engine substrate (Principle 7).
//
// VOTV's `playerRagdoll_C` is the "plushie" ragdoll body that ragdollMode spawns
// when a player faints/ragdolls -- the visible flopping kel body (its SkeletalMesh
// @0x230 self-configures to `kel_lmao` / `inst_kel_body`, natively rigged to the
// full 6-bone chain lowlegs-thighs-pelvis-chest-head-head_end). We spawn it
// OURSELVES on a puppet (coop/remote_player) as the VISIBLE flop display (the
// 2026-07-03 visible-plushy rework -- the caller hides the puppet's kel meshes for
// the flop), WITHOUT calling ragdollMode -- which is GLOBALLY scoped and KILLS the
// host (death event regardless of params; 2026-06-01 RE, [[project-ragdoll-sync]]).
//
// Proven recipe (harness/autotest_ragdoll_spawn_probe.cpp SP-solo probe, commit
// 01aad35):
//   BeginDeferredSpawn(playerRagdoll_C) -> write Player @0x248 (Expose-On-Spawn,
//   BEFORE Finish so ReceiveBeginPlay self-configures the visible kel mesh) ->
//   FinishDeferredSpawn -> StartBodySim (collision + SetAllBodiesSimulatePhysics +
//   SetSimulatePhysics; BeginPlay builds the bodies but leaves them FROZEN, so the
//   caller must start the sim -- ragdollMode does exactly this after its spawn).
// The spawn is DEATH-FREE: it never touches the owner's dead/isRagdoll fields.
//
// No network / gameplay state here (P7): coop/remote_player owns the lifecycle
// (spawn on the ragdoll wire-bit edge, track, DestroyActor on recover).

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

// CXX SDK (Game_0.9.0n): AplayerRagdoll_C::Player @0x0248 (AmainPlayer_C*);
// Aragdoll_C::SkeletalMesh @0x0230 (USkeletalMeshComponent*, the body mesh).
// AmainPlayer_C::ragdollActor @0x0C40 (AplayerRagdoll_C*, the NATIVE ragdoll the
// C-key/faint spawns -- the v22 sender reads its pelvis physics). Offset cited to
// the CXX dump + autotest_ragdoll_spawn_probe.cpp (which read the same field).
constexpr size_t kPlayerRagdoll_Player  = 0x0248;
constexpr size_t kAragdoll_SkeletalMesh = 0x0230;
constexpr size_t kMainPlayer_ragdollActor = 0x0C40;

// Cached UClass + sim UFunctions (resolve once; playerRagdoll_C loads with
// gameplay and is always resident per the probe's residency check). Plain void*
// caches -- no atomics/mutex (the incremental-link DLL-corruption rule does not
// structurally trigger), consistent with the engine_mainplayer.cpp precedent.
void* g_ragdollClass     = nullptr;
void* g_setAllBodiesSimFn = nullptr;
void* g_setSimPhysFn      = nullptr;
void* g_setCollisionFn    = nullptr;
// Pelvis reads: the SENDER samples its native ragdoll's pelvis loc/rot/velocity for
// the v22 wire (ReadLocalRagdollPelvisPhysics). The "pelvis" FName is stable across
// all ragdoll bodies (one GNames entry), so cache the 8 bytes once + the fns.
void* g_getSocketRotFn   = nullptr;
uint8_t g_pelvisFName[8] = {};
bool g_havePelvisFName   = false;
// v22 ragdoll physics sync caches (resolve once; SceneComponent::GetSocketLocation +
// the 4 UPrimitiveComponent velocity ops -- the same pair the prop pipeline uses).
void* g_getSocketLocFn = nullptr;
void* g_getLinVelFn    = nullptr;  // UPrimitiveComponent::GetPhysicsLinearVelocity
void* g_getAngVelFn    = nullptr;  // ::GetPhysicsAngularVelocityInDegrees
void* g_setLinVelFn    = nullptr;  // ::SetPhysicsLinearVelocity
void* g_setAngVelFn    = nullptr;  // ::SetPhysicsAngularVelocityInDegrees

void* ResolveRagdollClass() {
    if (g_ragdollClass && R::IsLive(g_ragdollClass)) return g_ragdollClass;
    g_ragdollClass = R::FindClass(L"playerRagdoll_C");
    return g_ragdollClass;
}

// Start PhysX gravity simulation on the ragdoll body's skeletal-mesh component.
// This is the step ragdollMode does AFTER spawn that AplayerRagdoll_C::BeginPlay
// does NOT (the bare body stays frozen at the spawn transform). Proven by the SP
// probe: with this the body falls to the floor + settles; without it, rigid.
// Game thread only.
void StartBodySim(void* comp) {
    if (!comp || !R::IsLive(comp)) return;
    if (!g_setCollisionFn) {
        if (void* c = R::FindClass(L"PrimitiveComponent")) g_setCollisionFn = R::FindFunction(c, L"SetCollisionEnabled");
    }
    if (g_setCollisionFn) {  // QueryAndPhysics(3) so the bodies have a floor to rest on
        ParamFrame f(g_setCollisionFn); f.Set<uint8_t>(L"NewType", uint8_t{3}); Call(comp, f);
    }
    if (!g_setAllBodiesSimFn) {
        if (void* c = R::FindClass(L"SkeletalMeshComponent")) g_setAllBodiesSimFn = R::FindFunction(c, L"SetAllBodiesSimulatePhysics");
    }
    if (g_setAllBodiesSimFn) {
        ParamFrame f(g_setAllBodiesSimFn); f.Set<bool>(L"bNewSimulate", true); Call(comp, f);
    }
    if (!g_setSimPhysFn) {
        if (void* c = R::FindClass(L"PrimitiveComponent")) g_setSimPhysFn = R::FindFunction(c, L"SetSimulatePhysics");
    }
    if (g_setSimPhysFn) {
        ParamFrame f(g_setSimPhysFn); f.Set<bool>(L"bSimulate", true); Call(comp, f);
    }
}

// Find the 8-byte FName of the bone named `wantName` on a skeletal-mesh component
// (enumerate bones + match by string -- we can't construct an FName for an arbitrary
// string without the engine's name table). Writes NAME_None (zeros) into outName and
// returns false if not found. Game thread only.
bool FindBoneFName(void* meshComp, const wchar_t* wantName, uint8_t outName[8]) {
    std::memset(outName, 0, 8);
    if (!meshComp || !R::IsLive(meshComp)) return false;
    void* sk = R::FindClass(L"SkinnedMeshComponent");
    if (!sk) return false;
    void* numFn  = R::FindFunction(sk, L"GetNumBones");
    void* nameFn = R::FindFunction(sk, L"GetBoneName");
    if (!numFn || !nameFn) return false;
    int32_t n = 0;
    { ParamFrame f(numFn); if (Call(meshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    for (int32_t i = 0; i < n; ++i) {
        uint8_t name[8] = {};
        ParamFrame nf(nameFn); nf.Set<int32_t>(L"BoneIndex", i);
        if (!Call(meshComp, nf)) continue;
        nf.GetRaw(L"ReturnValue", name, sizeof(name));
        if (R::ToString(*reinterpret_cast<const R::FName*>(name)) == wantName) {
            std::memcpy(outName, name, 8);
            return true;
        }
    }
    return false;
}

// Ensure g_pelvisFName holds the "pelvis" bone FName (resolve once from any ragdoll
// mesh -- the name is one GNames entry, identical across the sender's native ragdoll
// and our spawned mirror body). Returns false until it resolves. Game thread.
bool EnsurePelvisFName(void* mesh) {
    if (g_havePelvisFName) return true;
    g_havePelvisFName = FindBoneFName(mesh, L"pelvis", g_pelvisFName);
    return g_havePelvisFName;
}

// Resolve the SceneComponent::GetSocketLocation + the 4 UPrimitiveComponent velocity
// UFunctions once. Returns false if the engine classes/functions can't be found.
bool ResolvePhysicsFns() {
    if (!g_getSocketLocFn) {
        if (void* sc = R::FindClass(L"SceneComponent")) g_getSocketLocFn = R::FindFunction(sc, L"GetSocketLocation");
    }
    if (!g_getSocketRotFn) {
        if (void* sc = R::FindClass(L"SceneComponent")) g_getSocketRotFn = R::FindFunction(sc, L"GetSocketRotation");
    }
    // Resolve the 4 UPrimitiveComponent velocity ops. Cache the class pointer once so
    // a single still-unresolved fn (e.g. a transient FindFunction miss) re-runs only
    // its own FindFunction, NOT a fresh FindClass GUObjectArray walk every frame (the
    // per-pointer-independent guard pattern the sibling SceneComponent lookups use).
    if (!g_getLinVelFn || !g_getAngVelFn || !g_setLinVelFn || !g_setAngVelFn) {
        static void* sPrimCompCls = nullptr;
        if (!sPrimCompCls || !R::IsLive(sPrimCompCls)) sPrimCompCls = R::FindClass(L"PrimitiveComponent");
        if (sPrimCompCls) {
            if (!g_getLinVelFn) g_getLinVelFn = R::FindFunction(sPrimCompCls, L"GetPhysicsLinearVelocity");
            if (!g_getAngVelFn) g_getAngVelFn = R::FindFunction(sPrimCompCls, L"GetPhysicsAngularVelocityInDegrees");
            if (!g_setLinVelFn) g_setLinVelFn = R::FindFunction(sPrimCompCls, L"SetPhysicsLinearVelocity");
            if (!g_setAngVelFn) g_setAngVelFn = R::FindFunction(sPrimCompCls, L"SetPhysicsAngularVelocityInDegrees");
        }
    }
    return g_getSocketLocFn && g_getSocketRotFn &&
           g_getLinVelFn && g_getAngVelFn && g_setLinVelFn && g_setAngVelFn;
}

// Resolve a ragdoll actor's body SkeletalMesh @0x230 (live-checked). null on miss.
void* RagdollMeshOf(void* ragdollActor) {
    if (!ragdollActor || !R::IsLive(ragdollActor)) return nullptr;
    void* mesh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ragdollActor) + kAragdoll_SkeletalMesh);
    return (mesh && R::IsLive(mesh)) ? mesh : nullptr;
}

}  // namespace

void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation) {
    if (!ownerPlayer || !R::IsLive(ownerPlayer)) return nullptr;
    void* cls = ResolveRagdollClass();
    if (!cls) { UE_LOGW("ragdoll_body: playerRagdoll_C class unresolved -- cannot spawn"); return nullptr; }
    void* body = BeginDeferredSpawn(cls, location, rotation);
    if (!body) { UE_LOGW("ragdoll_body: BeginDeferredSpawn returned null"); return nullptr; }
    // Expose-On-Spawn: stamp Player @0x248 BEFORE FinishDeferredSpawn runs
    // ReceiveBeginPlay, so the actor self-configures its visible kel body mesh from
    // the owning player (the same way the K2 SpawnActorFromClass node exposes
    // construction params -- mirrors the deferred variantState pattern in
    // remote_prop). Raw UPROPERTY* write into the uninitialized actor.
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + kPlayerRagdoll_Player) = ownerPlayer;
    FinishDeferredSpawn(body, location, rotation);
    if (!R::IsLive(body)) { UE_LOGW("ragdoll_body: body died during FinishDeferredSpawn"); return nullptr; }
    // BeginPlay built the rigid bodies but left them frozen -- start the sim so it
    // actually flops (proven necessary by the probe). The body's own mesh stays
    // VISIBLE: it IS the display (2026-07-03 visible-plushy rework) -- the game's own
    // plushie ragdoll, its full 6-bone chain natively rigged to the physics, exactly
    // what SP shows in mirrors. (History: v22 hid it and tumbled the pelvis-attached
    // kel instead -- a rigid one-piece look; a master-pose probe then coupled the kel
    // meshes but only 4/6 bones mapped (the skin skeleton lacks thighs/lowlegs) and
    // the user refuted it by eye. The caller hides the puppet's kel meshes for the
    // flop instead -- no double-image, no skeleton mapping at all.)
    void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + kAragdoll_SkeletalMesh);
    if (comp && R::IsLive(comp)) StartBodySim(comp);
    UE_LOGI("ragdoll_body: spawned VISIBLE playerRagdoll_C flop body @%p (owner=%p) at (%.0f,%.0f,%.0f), sim started",
            body, ownerPlayer, location.X, location.Y, location.Z);
    return body;
}

// Attach `actor` (its RootComponent) to the ragdoll `body`'s mesh @0x230 at the
// PELVIS bone, so the actor rigidly follows the flopping ragdoll's pelvis per-frame --
// the user's "pelvis to pelvis attachment". KeepWorld rules: the actor keeps its
// current world transform at attach time (it spawned co-located with the ragdoll), then
// follows the pelvis's motion deltas, so it tumbles WITH the flop. No shared skeleton
// needed (rigid transform follow, not pose copy). Falls back to NAME_None (the
// component root) if the pelvis bone isn't found. Game thread only.
bool AttachActorToRagdollBody(void* actor, void* body) {
    if (!actor || !R::IsLive(actor) || !body || !R::IsLive(body)) return false;
    void* mesh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + kAragdoll_SkeletalMesh);
    if (!mesh || !R::IsLive(mesh)) return false;
    void* fn = R::FindFunction(R::FindClass(L"Actor"), L"K2_AttachToComponent");
    if (!fn) { UE_LOGW("ragdoll_body: K2_AttachToComponent unresolved"); return false; }
    uint8_t socket[8] = {};                            // pelvis FName, or NAME_None on miss
    const bool gotPelvis = FindBoneFName(mesh, L"pelvis", socket);
    if (!gotPelvis) UE_LOGW("ragdoll_body: 'pelvis' bone not found on the ragdoll mesh -- attaching to the root");
    ParamFrame f(fn);
    f.Set<void*>(L"Parent", mesh);
    f.SetRaw(L"SocketName", socket, sizeof(socket));
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});       // EAttachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});       // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(actor, f);
}

// Detach `actor` from its parent (the ragdoll body), KeepWorld so it stays where the
// flop left it (the next pose-drive then takes over). Game thread only.
bool DetachActorFromRagdollBody(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* fn = R::FindFunction(R::FindClass(L"Actor"), L"K2_DetachFromActor");
    if (!fn) { UE_LOGW("ragdoll_body: K2_DetachFromActor unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});       // EDetachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});       // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld
    return Call(actor, f);
}

void* GetRagdollBodyMesh(void* ragdollActor) {
    return RagdollMeshOf(ragdollActor);  // null-safe + liveness-guarded inside
}

void* GetLocalRagdollBodyMesh(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    void* ragdollActor =
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + kMainPlayer_ragdollActor);
    return RagdollMeshOf(ragdollActor);
}

bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel) {
    outLoc = FVector{}; outRot = FRotator{}; outLinVel = FVector{}; outAngVel = FVector{};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    // The native ragdoll the C-key/faint spawned. Null until the player ragdolls;
    // cleared on recover -- so a non-null + live value IS the "is ragdolling" signal.
    void* ragdollActor =
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + kMainPlayer_ragdollActor);
    void* mesh = RagdollMeshOf(ragdollActor);
    if (!mesh) return false;
    if (!ResolvePhysicsFns()) return false;
    if (!EnsurePelvisFName(mesh)) return false;
    // Pelvis WORLD location + rotation (the kel-tumble anchor).
    { ParamFrame f(g_getSocketLocFn); f.SetRaw(L"InSocketName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outLoc = f.Get<FVector>(L"ReturnValue"); }
    { ParamFrame f(g_getSocketRotFn); f.SetRaw(L"InSocketName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outRot = f.Get<FRotator>(L"ReturnValue"); }
    // Pelvis linear (cm/s) + angular (deg/s) velocity -- the "physics properties".
    { ParamFrame f(g_getLinVelFn); f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outLinVel = f.Get<FVector>(L"ReturnValue"); }
    { ParamFrame f(g_getAngVelFn); f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outAngVel = f.Get<FVector>(L"ReturnValue"); }
    return true;
}

void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel) {
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return;
    if (!ResolvePhysicsFns()) return;
    if (!EnsurePelvisFName(mesh)) return;
    // Overwrite (bAddToCurrent=false) the pelvis body's velocity with the sender's --
    // the body slaves its gross motion to the real ragdoll. The constrained limb bodies
    // sim freely (invisible); only the pelvis trajectory feeds the visible pelvis-
    // attached kel. Same SetPhysics{Linear,Angular}Velocity pair as PropRelease.
    { ParamFrame f(g_setLinVelFn);
      f.Set<FVector>(L"NewVel", linVel);
      f.Set<bool>(L"bAddToCurrent", false);
      f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      Call(mesh, f); }
    { ParamFrame f(g_setAngVelFn);
      f.Set<FVector>(L"NewAngVel", angVel);
      f.Set<bool>(L"bAddToCurrent", false);
      f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      Call(mesh, f); }
}

}  // namespace ue_wrap::engine

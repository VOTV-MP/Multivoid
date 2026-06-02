// ue_wrap/engine_attach.cpp -- generic actor-attach + root-physics substrate for
// the held-clump mirror (Principle 7 engine wrapper; no network/gameplay state).
//
// The trash ball / chipPile-clump (prop_garbageClump_C) is non-Aprop_C AND
// non-keyable (autotest 33e7f25 proved setKey doesn't stick), so it cannot ride
// the keyed PropSpawn/PropPose path the mannequin uses. coop/held_clump_sync
// instead SPAWNS a mirror clump on each peer and ATTACHES it to the holder
// puppet's hand bone (the MTA attach model -- the puppet hand is already pose-
// synced, so the clump follows it for free). On release the mirror detaches +
// re-enables physics + inherits the holder's throw velocity ("physics like the
// mannequin"). [[project-bug-trash-chippile-uaf-crash]]
//
// These are the engine primitives that lifecycle needs:
//   AttachActorToPuppetHand   -- K2_AttachToComponent to mesh_playerVisible @0x4F8 hand bone
//   DetachActorFromParent     -- K2_DetachFromActor (KeepWorld)
//   SetActorSimulatePhysics   -- root primitive SetSimulatePhysics (kinematic while held)
//   Get/SetActorRootPhysicsVelocity -- root primitive velocity (release throw)
//
// They are GENERIC (resolve the root via K2_GetRootComponent; NO Aprop_C mesh
// offset), so they never deref the non-Aprop_C clump's mesh -- keeping the
// reverted-2a use-after-free permanently out of reach (ue_wrap::prop::GetStaticMesh
// still returns null for non-Aprop_C; nothing here calls it). Distinct from
// AttachActorToRagdollBody (engine_playerragdoll.cpp), which is playerRagdoll_C-
// specific (pelvis bone, @0x230 mesh). Minor duplication of the K2_AttachToComponent
// call shape is deliberate -- decoupling the two attach sites avoids a refactor of
// the shipped+working ragdoll path.

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

// CXX SDK (Game_0.9.0n): AmainPlayer_C::mesh_playerVisible @0x04F8
// (USkeletalMeshComponent*, the authoritative third-person body the puppet
// renders + the AnimBP poses; the native ACharacter::Mesh @0x0280 is typically
// hidden). Hardcoded locally with the CXX cite -- same self-contained pattern as
// engine_playerragdoll.cpp's offset constants (sdk_profile.h:166 is the source).
constexpr size_t kMainPlayer_mesh_playerVisible = 0x04F8;

// Cached UFunctions (resolve once; re-resolve if the owning class is freed). Plain
// void* caches, game-thread only -- consistent with engine_playerragdoll.cpp.
void* g_attachFn   = nullptr;  // Actor::K2_AttachToComponent
void* g_detachFn   = nullptr;  // Actor::K2_DetachFromActor
void* g_getRootFn  = nullptr;  // Actor::K2_GetRootComponent
void* g_setSimFn   = nullptr;  // PrimitiveComponent::SetSimulatePhysics
void* g_getLinFn   = nullptr;  // PrimitiveComponent::GetPhysicsLinearVelocity
void* g_getAngFn   = nullptr;  // PrimitiveComponent::GetPhysicsAngularVelocityInDegrees
void* g_setLinFn   = nullptr;  // PrimitiveComponent::SetPhysicsLinearVelocity
void* g_setAngFn   = nullptr;  // PrimitiveComponent::SetPhysicsAngularVelocityInDegrees

// Hand-bone resolution. The hand FName is one GNames entry (identical across every
// puppet of the same skeleton), so resolve it ONCE from the first puppet mesh + cache
// the 8 bytes. g_dumpedBones gates the one-time full bone-list log (verify-not-guess).
uint8_t g_handFName[8] = {};
bool    g_haveHandFName = false;
bool    g_dumpedBones   = false;
// Latched once the candidate scan ran over a FULLY-POPULATED skeleton and matched
// nothing (vs. a transient "mesh not ready / 0 bones" which keeps retrying). Without
// it the no-match path would re-run the full O(bones x candidates) scan on EVERY
// grab forever. Dead on VOTV (hand_R is always present), but keeps the no-match path
// from being an unlatched hot-path scan. Audit finding (2026-06-02).
bool    g_boneResolveFailed = false;

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

// Read bone i's FName off a skeletal-mesh component. false on failure.
bool BoneNameAt(void* meshComp, void* numFn, void* nameFn, int32_t i, uint8_t out[8]) {
    std::memset(out, 0, 8);
    (void)numFn;
    ParamFrame nf(nameFn);
    nf.Set<int32_t>(L"BoneIndex", i);
    if (!Call(meshComp, nf)) return false;
    nf.GetRaw(L"ReturnValue", out, 8);
    return true;
}

// Enumerate `meshComp` bones; (once) log them all; resolve the hand bone into
// g_handFName from a candidate list. Returns true once a hand bone (or, after the
// candidate list is exhausted, NAME_None as a deliberate root fallback) is settled.
// We cache g_haveHandFName=true only on a real candidate match so a later puppet
// with a richer skeleton can still upgrade from the root fallback.
bool ResolveHandBone(void* meshComp) {
    if (g_haveHandFName || g_boneResolveFailed) return true;
    if (!meshComp || !R::IsLive(meshComp)) return false;
    void* sk = R::FindClass(L"SkinnedMeshComponent");
    if (!sk) return false;
    void* numFn  = R::FindFunction(sk, L"GetNumBones");
    void* nameFn = R::FindFunction(sk, L"GetBoneName");
    if (!numFn || !nameFn) return false;
    int32_t n = 0;
    { ParamFrame f(numFn); if (Call(meshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    if (n <= 0) return false;

    // One-time full dump so the real skeleton names land in the log (the hand-bone
    // name is the one RE unknown for this feature; this makes it verifiable).
    if (!g_dumpedBones) {
        g_dumpedBones = true;
        UE_LOGI("attach: puppet mesh_playerVisible skeleton has %d bones -- dumping:", n);
        for (int32_t i = 0; i < n; ++i) {
            uint8_t bn[8] = {};
            if (!BoneNameAt(meshComp, numFn, nameFn, i, bn)) continue;
            UE_LOGI("attach:   bone[%d] = '%ls'", i,
                    R::ToString(*reinterpret_cast<const R::FName*>(bn)).c_str());
        }
    }

    // Candidate hand bones, first match wins. CONFIRMED by the 2026-06-02 LAN bone
    // dump: VOTV's kel/kerfur puppet skeleton (mesh_playerVisible, 101 bones) names
    // the right hand 'hand_R' (bone index 29) and the left 'hand_L' (52) -- those two
    // lead. The rest are robustness fallbacks for a future re-rig. If none match,
    // NAME_None (the mesh root) is the fallback so the clump still follows the puppet
    // BODY (degraded but visible, never a crash); we do NOT latch in that case, so a
    // confirmed hand-bone name can still replace the root. Right hand preferred (most
    // carrying is right-handed).
    static const wchar_t* kCandidates[] = {
        L"hand_R", L"hand_L",                              // VOTV kel/kerfur (confirmed)
        L"hand_r", L"Hand_R", L"hand_l", L"Hand_L",        // case fallbacks
        L"RightHand", L"righthand", L"LeftHand", L"ik_hand_r", L"b_hand_r",
        L"hand", L"wrist_r", L"hand_rSocket",
    };
    for (int32_t i = 0; i < n; ++i) {
        uint8_t bn[8] = {};
        if (!BoneNameAt(meshComp, numFn, nameFn, i, bn)) continue;
        const std::wstring s = R::ToString(*reinterpret_cast<const R::FName*>(bn));
        for (const wchar_t* cand : kCandidates) {
            if (s == cand) {
                std::memcpy(g_handFName, bn, 8);
                g_haveHandFName = true;
                UE_LOGI("attach: resolved puppet HAND bone = '%ls' (bone index %d)", cand, i);
                return true;
            }
        }
    }
    // No candidate matched over a fully-populated skeleton -- latch the failure so we
    // don't re-scan every grab, and fall back to NAME_None (the mesh root: the clump
    // follows the body, not the hand). g_handFName stays zeroed.
    g_boneResolveFailed = true;
    UE_LOGW("attach: no candidate hand bone matched the puppet skeleton -- attaching to "
            "the mesh ROOT (clump follows the body, not the hand). Check the bone dump "
            "above and add the real hand-bone name to kCandidates.");
    return true;  // settled (root fallback)
}

}  // namespace

bool AttachActorToPuppetHand(void* actor, void* puppetActor) {
    if (!actor || !R::IsLive(actor) || !puppetActor || !R::IsLive(puppetActor)) return false;
    void* mesh = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(puppetActor) + kMainPlayer_mesh_playerVisible);
    if (!mesh || !R::IsLive(mesh)) {
        UE_LOGW("attach: puppet %p mesh_playerVisible missing/dead -- cannot attach clump", puppetActor);
        return false;
    }
    void* fn = ActorFn(&g_attachFn, L"K2_AttachToComponent");
    if (!fn) { UE_LOGW("attach: K2_AttachToComponent unresolved"); return false; }
    ResolveHandBone(mesh);                              // dumps once + fills g_handFName (or root)
    ParamFrame f(fn);
    f.Set<void*>(L"Parent", mesh);
    f.SetRaw(L"SocketName", g_handFName, sizeof(g_handFName));  // hand bone, or NAME_None root
    // SnapToTarget for location+rotation: the clump is spawned at the puppet's PIVOT
    // (feet), so KeepWorld would leave it offset from the hand -- SnapToTarget zeroes
    // the relative transform so the clump sits exactly AT the hand bone and follows it.
    // ScaleRule=KeepWorld so the clump keeps its own size (SnapToTarget would inherit
    // the bone's scale). EAttachmentRule { KeepRelative=0, KeepWorld=1, SnapToTarget=2 }.
    f.Set<uint8_t>(L"LocationRule", uint8_t{2});        // SnapToTarget (sit at the hand)
    f.Set<uint8_t>(L"RotationRule", uint8_t{2});        // SnapToTarget (orient with the hand)
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});        // KeepWorld (keep the clump's own size)
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(actor, f);
}

bool DetachActorFromParent(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* fn = ActorFn(&g_detachFn, L"K2_DetachFromActor");
    if (!fn) { UE_LOGW("attach: K2_DetachFromActor unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});        // EDetachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});        // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});        // KeepWorld
    return Call(actor, f);
}

// Resolve `actor`'s root component (USceneComponent*) via K2_GetRootComponent.
// null on failure. Game thread.
static void* RootComponentOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* fn = ActorFn(&g_getRootFn, L"K2_GetRootComponent");
    if (!fn) return nullptr;
    ParamFrame f(fn);
    if (!Call(actor, f)) return nullptr;
    void* root = f.Get<void*>(L"ReturnValue");
    return (root && R::IsLive(root)) ? root : nullptr;
}

bool SetActorSimulatePhysics(void* actor, bool simulate) {
    void* root = RootComponentOf(actor);
    if (!root) return false;
    void* fn = PrimFn(&g_setSimFn, L"SetSimulatePhysics");
    if (!fn) { UE_LOGW("attach: SetSimulatePhysics unresolved"); return false; }
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

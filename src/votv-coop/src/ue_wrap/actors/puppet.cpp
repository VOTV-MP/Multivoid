#include "ue_wrap/actors/puppet.h"
#include "puppet_internal.h"  // offset templates, g_meshComp, LiveAnimInstance

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/reflected_offset.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ue_wrap::puppet {

namespace P = profile;
namespace R = reflection;
namespace E = engine;
namespace GT = game_thread;

// Puppet actor -> its cached SkeletalMeshComponent, so Drive() does not walk GUObjectArray
// every frame.
std::unordered_map<void*, ue_wrap::CachedObjRef> g_meshComp;

// The live AnimInstance running on a SkeletalMeshComponent.
void* LiveAnimInstance(void* skeletalMeshComponent) {
    return ReadPtr(skeletalMeshComponent, P::off::USkeletalMesh_AnimScriptInstance);
}

namespace {

// The puppet's animation is driven by writing Velocity + MovementMode on its OWN
// CharacterMovement each tick; the CMC tick is parked, so those fields are ours to own. The
// AnimBP's BlueprintUpdateAnimation reads them through Pawn -> Pawn.CMC on its own, exactly as
// it does for the possessed local player, so speed and the IK gate behave the same with no
// AnimInstance pointer plumbing.

// ---- kerfur head-look --------------------------------------------------------
// Guard for every lookAt / customLookAt access: is this AnimInstance a kerfur-family AnimBP?
// A different AnimBP holds unrelated fields at those offsets, so an unguarded write would
// corrupt foreign state. The class pointer resolves once and caches; it re-resolves while
// still null, because the BP class loads with the level and may arrive after the first NPC
// streams in.
bool IsKerfurAnimBP(void* anim) {
    if (!anim || !R::IsLive(anim)) return false;  // guard the raw ClassOf read against a GC'd AnimInstance
    static void* kerfurAnimClass = nullptr;
    if (!kerfurAnimClass) kerfurAnimClass = R::FindClass(P::name::AnimBPKerfurRegularClass);
    if (!kerfurAnimClass) return false;
    void* cls = R::ClassOf(anim);
    if (!cls) return false;
    if (cls == kerfurAnimClass) return true;
    void* kerfurBases[1] = { kerfurAnimClass };  // match the codebase's void* const* IsDescendantOfAny pattern
    return R::IsDescendantOfAny(cls, kerfurBases, 1);  // skerfuro / skeleton AnimBP variants subclass it
}

// An NPC's body skeletal-mesh component, the native ACharacter::Mesh slot. Real kerfur NPCs
// run the AnimBP on that slot and show their visible body there -- their own component list
// holds only particles, static meshes and outfits -- unlike mainPlayer_C, which uses
// mesh_playerVisible. Null if not resolvable.
void* NpcBodyMesh(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* mesh = ReadPtr(actor, P::off::ACharacter_Mesh);
    if (!mesh || !R::IsLive(mesh)) return nullptr;
    return mesh;
}

// That mesh's live AnimInstance. Null if not resolvable.
void* NpcBodyAnimInstance(void* actor) {
    void* mesh = NpcBodyMesh(actor);
    return mesh ? LiveAnimInstance(mesh) : nullptr;
}

// Read and write the kerfur head-look on an already-resolved AnimInstance. Class-gated, and
// the offsets are reflection-resolved so a recook cannot silently move them. The write also
// sets customLookAt, which stops the AnimInstance's own BlueprintUpdateAnimation from
// overwriting lookAt with the local player's camera.
bool ReadLookAtOnAnim(void* anim, FVector& out) {
    if (!IsKerfurAnimBP(anim)) return false;
    const int32_t off = ue_wrap::reflected_offset::AnimBP_kerfur_lookAt();
    if (off < 0) return false;
    out = ReadAt<FVector>(anim, static_cast<size_t>(off));
    return true;
}
void WriteLookAtOnAnim(void* anim, const FVector& target) {
    if (!IsKerfurAnimBP(anim)) return;
    const int32_t lookOff   = ue_wrap::reflected_offset::AnimBP_kerfur_lookAt();
    const int32_t customOff = ue_wrap::reflected_offset::AnimBP_kerfur_customLookAt();
    if (lookOff < 0 || customOff < 0) return;
    WriteAt<FVector>(anim, static_cast<size_t>(lookOff), target);
    WriteAt<bool>(anim, static_cast<size_t>(customOff), true);
    static bool s_loggedDrive = false;
    if (!s_loggedDrive) { s_loggedDrive = true;
        UE_LOGI("puppet: kerfur head-look drive active -- wrote lookAt=(%.0f,%.0f,%.0f) + customLookAt=true (first; class-gate passed)",
                target.X, target.Y, target.Z); }
}

// The head-look state-gate defeat hook lives in puppet_spawn.cpp, beside its only install
// site on the spawn path.
}  // namespace

void* GetMeshPlayerVisibleAsset(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return nullptr;
    void* comp = ReadPtr(mainPlayerPawn, P::off::AmainPlayer_mesh_playerVisible);
    if (!comp) {
        UE_LOGW("puppet: local mesh_playerVisible component null");
        return nullptr;
    }
    void* meshAsset = ReadPtr(comp, P::off::USkinnedMesh_SkeletalMesh);
    UE_LOGI("puppet: local skin = %ls (comp=%p asset=%p)",
            R::ClassNameOf(meshAsset).c_str(), comp, meshAsset);
    // Snapshot the local working body's AnimBP state so the puppet dump taken at spawn can be
    // diffed against it: if the puppet renders as a stick, the diff names the variable to set.
    DumpAnimState(L"local", comp);
    return meshAsset;
}

void* GetMeshPlayerVisibleComponent(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return nullptr;
    return ReadPtr(mainPlayerPawn, P::off::AmainPlayer_mesh_playerVisible);
}

void* GetMeshPlayerVisibleAnimClass(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return nullptr;
    void* comp = ReadPtr(mainPlayerPawn, P::off::AmainPlayer_mesh_playerVisible);
    if (!comp) return nullptr;
    void* animClass = ReadPtr(comp, P::off::USkeletalMesh_AnimClass);
    UE_LOGI("puppet: local AnimClass = %ls (%p)", R::ClassNameOf(animClass).c_str(), animClass);
    return animClass;
}

void* GetNativeBodyMeshComponent(void* mainPlayerActor) {
    if (!mainPlayerActor) return nullptr;
    return ReadPtr(mainPlayerActor, P::off::ACharacter_Mesh);
}

void* GetComponentSkeletalMeshAsset(void* skinnedComponent) {
    if (!skinnedComponent) return nullptr;
    return ReadPtr(skinnedComponent, P::off::USkinnedMesh_SkeletalMesh);
}

void* GetSkeletalMeshComponent(void* puppetActor) {
    if (!puppetActor) return nullptr;
    // A destroyed puppet (a level change) takes its component with it. Drop the stale entry
    // rather than return a dangling pointer that the caller would read AnimScriptInstance from.
    if (!R::IsLive(puppetActor)) { g_meshComp.erase(puppetActor); return nullptr; }
    auto it = g_meshComp.find(puppetActor);
    if (it != g_meshComp.end()) {
        // The actor can outlive its child component for a tick mid-teardown, because UE finalises
        // sub-objects first. A live actor with a dying cached component must not hand back the
        // dying pointer; treat it as a cache miss, drop the entry, and re-resolve below.
        if (void* liveComp = it->second.Get()) return liveComp;  // slot-validated
        g_meshComp.erase(it);
    }
    // On a cache miss, read mesh_playerVisible directly. mainPlayer_C carries four
    // SkeletalMeshComponents (the native ACharacter::Mesh slot, mesh_playerVisible, arms and
    // playermodel), and a child-object scan returns whichever comes first in GUObjectArray order
    // -- on this class the native Mesh slot, which is typically hidden and carries no AnimBP. The
    // driver would then dispatch to the wrong AnimInstance. MainPlayer is the only puppet kind, so
    // the direct read is the whole resolver.
    void* comp = ReadPtr(puppetActor, P::off::AmainPlayer_mesh_playerVisible);
    if (comp && !R::IsLive(comp)) comp = nullptr;
    if (comp) g_meshComp[puppetActor].Set(comp);  // fresh + just-IsLive'd above
    return comp;
}

void DumpKerfurHeadGraph(void* skeletalMeshComponent) {
    void* anim = LiveAnimInstance(skeletalMeshComponent);
    if (!anim) { UE_LOGW("puppet: DumpKerfurHeadGraph: no AnimInstance"); return; }
    auto bn = reinterpret_cast<uint8_t*>(anim);
    // BoneToModify's FName sits at the head of the FBoneReference.
    auto boneName = [bn](size_t nodeOff) {
        return *reinterpret_cast<R::FName*>(bn + nodeOff + P::anim::LookAtMod_BoneToModify);
    };
    auto alpha = [bn](size_t nodeOff) {
        return *reinterpret_cast<float*>(bn + nodeOff + P::anim::SkelCtl_Alpha);
    };
    auto alphaBool = [bn](size_t nodeOff) {
        return *reinterpret_cast<bool*>(bn + nodeOff + P::anim::SkelCtl_bAlphaBoolEnabled);
    };
    auto lookAtTargetComp = [bn](size_t nodeOff) {
        // FBoneSocketTarget's first qword is the TWeakObjectPtr<USkeletalMeshComponent>.
        return *reinterpret_cast<void**>(bn + nodeOff + P::anim::LookAt_LookAtTarget);
    };
    auto lookAtLoc = [bn](size_t nodeOff) {
        return *reinterpret_cast<FVector*>(bn + nodeOff + P::anim::LookAt_LookAtLocation);
    };
    UE_LOGI("puppet: DumpKerfurHeadGraph anim=%p", anim);
    UE_LOGI("  LookAt_1  @0x%04zX BoneToModify='%ls' alpha=%.2f boolEnabled=%d "
            "lookAtTargetComp=%p lookAtLoc=(%.0f,%.0f,%.0f)",
            P::anim::kKerfurLookAt_1, R::ToString(boneName(P::anim::kKerfurLookAt_1)).c_str(),
            alpha(P::anim::kKerfurLookAt_1), (int)alphaBool(P::anim::kKerfurLookAt_1),
            lookAtTargetComp(P::anim::kKerfurLookAt_1),
            lookAtLoc(P::anim::kKerfurLookAt_1).X, lookAtLoc(P::anim::kKerfurLookAt_1).Y, lookAtLoc(P::anim::kKerfurLookAt_1).Z);
    UE_LOGI("  LookAt    @0x%04zX BoneToModify='%ls' alpha=%.2f boolEnabled=%d "
            "lookAtTargetComp=%p lookAtLoc=(%.0f,%.0f,%.0f)",
            P::anim::kKerfurLookAt, R::ToString(boneName(P::anim::kKerfurLookAt)).c_str(),
            alpha(P::anim::kKerfurLookAt), (int)alphaBool(P::anim::kKerfurLookAt),
            lookAtTargetComp(P::anim::kKerfurLookAt),
            lookAtLoc(P::anim::kKerfurLookAt).X, lookAtLoc(P::anim::kKerfurLookAt).Y, lookAtLoc(P::anim::kKerfurLookAt).Z);
    const size_t mbOffs[7] = {
        P::anim::kKerfurModifyBone_6, P::anim::kKerfurModifyBone_5, P::anim::kKerfurModifyBone_4,
        P::anim::kKerfurModifyBone_3, P::anim::kKerfurModifyBone_2, P::anim::kKerfurModifyBone_1,
        P::anim::kKerfurModifyBone,
    };
    const char* mbLabels[7] = {"ModifyBone_6","ModifyBone_5","ModifyBone_4","ModifyBone_3","ModifyBone_2","ModifyBone_1","ModifyBone"};
    for (int i = 0; i < 7; ++i) {
        const float* rot = reinterpret_cast<float*>(bn + mbOffs[i] + P::anim::ModBone_Rotation);
        const uint8_t mode = *(bn + mbOffs[i] + P::anim::ModBone_RotationMode);
        UE_LOGI("  %-12s @0x%04zX BoneToModify='%ls' alpha=%.2f boolEnabled=%d rot=(P=%.1f Y=%.1f R=%.1f) rotMode=%u",
                mbLabels[i], mbOffs[i], R::ToString(boneName(mbOffs[i])).c_str(),
                alpha(mbOffs[i]), (int)alphaBool(mbOffs[i]),
                rot[0], rot[1], rot[2], static_cast<unsigned>(mode));
    }
}

void DumpAnimState(const wchar_t* label, void* skeletalMeshComponent) {
    void* anim = LiveAnimInstance(skeletalMeshComponent);
    if (!anim) {
        UE_LOGW("puppet: [%ls] AnimInstance NULL (comp=%p) -> mesh has no live AnimBP "
                "(would render as a static/reference-pose stick)", label, skeletalMeshComponent);
        return;
    }
    const float spd = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_spd());
    const float walkSpeed = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_walkSpeed());
    // animWalkAlpha and animWalkRate are dumped as observable AnimBP state only. They do not gate
    // idle-versus-walk: the local body walks with animWalkAlpha at 0. spd is the locomotion
    // driver, the BlendSpace X input.
    const float animWalkAlpha = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_animWalkAlpha());
    const float animWalkRate = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_animWalkRate());
    void* pawn = ReadPtr(anim, ue_wrap::reflected_offset::AnimBP_kerfur_Pawn());
    void* ctrl = ReadPtr(anim, ue_wrap::reflected_offset::AnimBP_kerfur_Controller());
    void* movement = ReadPtr(anim, ue_wrap::reflected_offset::AnimBP_kerfur_Movement());
    void* kerfur = ReadPtr(anim, ue_wrap::reflected_offset::AnimBP_kerfur_kerfur());
    const bool useLegIK = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_useLegIK());
    const bool isFace = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_isFace());
    const bool lookingAtPlayer = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer());
    UE_LOGI("puppet: [%ls] AnimInstance=%ls(%p) spd=%.1f walkSpeed=%.1f "
            "animWalkAlpha=%.2f animWalkRate=%.2f "
            "Pawn=%p Controller=%p Movement=%p kerfur=%p "
            "useLegIK=%d isFace=%d lookingAtPlayer=%d",
            label, R::ClassNameOf(anim).c_str(), anim, spd, walkSpeed,
            animWalkAlpha, animWalkRate,
            pawn, ctrl, movement, kerfur, useLegIK, isFace, lookingAtPlayer);
}

void DriveHeadLookAtWorld(void* puppetActor, const FVector& worldTarget) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    // The actor slot can still pass IsLive for a tick while its child component is being torn
    // down, so re-check the component before reading its AnimScriptInstance.
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;

    // Drive the head through the kerfur native lookAt pipeline. The visible head and neck twist
    // comes from two FAnimNode_LookAt nodes -- head at alpha 1.0, neck at 0.5, 45 degrees of clamp
    // each -- aiming at the AnimBP's lookAt vector, which a native PropertyAccess fast-path copies
    // into LookAtLocation every tick. Writing lookAt with customLookAt set therefore makes our
    // world target the nodes' input, and stops BlueprintUpdateAnimation from re-aiming lookAt at
    // the local PlayerCameraManager, which would make every puppet head track the observer.
    // This is the same write DriveKerfurLookAt uses for an NPC mirror, applied to the puppet's own
    // instances, so NPC head-follow is untouched.
    WriteLookAtOnAnim(anim, worldTarget);

    // The puppet renders two overlapped bodies: mesh_playerVisible is attached to the native
    // ACharacter::Mesh slot with the same skin asset, and each ticks its own kerfur AnimInstance.
    // In single-player the two stay identical because both auto-aim lookAt at the same local
    // camera; driving only one breaks that, and the un-driven head keeps following the observer.
    // Drive both with the same target. Each write is class-gated, and the dedupe guard covers a
    // future single-mesh refactor.
    void* meshSlot = ReadPtr(puppetActor, P::off::ACharacter_Mesh);
    if (meshSlot && meshSlot != comp && R::IsLive(meshSlot)) {
        if (void* slotAnim = LiveAnimInstance(meshSlot)) {
            if (slotAnim != anim) WriteLookAtOnAnim(slotAnim, worldTarget);
        }
    }
}

bool ReadPuppetHeadLookProbe(void* puppetActor, PuppetHeadLookProbe& out) {
    out = {};
    void* comp = GetSkeletalMeshComponent(puppetActor);
    if (!comp || !R::IsLive(comp)) return false;
    // LookAt clamp, in degrees, off the puppet's own kerfur AnimInstance: one value per
    // FAnimNode_LookAt node, head and neck.
    void* anim = LiveAnimInstance(comp);
    if (anim && IsKerfurAnimBP(anim)) {
        out.headClampDeg = ReadAt<float>(anim, P::anim::kKerfurLookAt_1 + P::anim::LookAt_Clamp);
        out.neckClampDeg = ReadAt<float>(anim, P::anim::kKerfurLookAt   + P::anim::LookAt_Clamp);
        out.haveClamp = true;
        // Gate diagnostics: the node alphas say whether the look is blended out when the head
        // freezes, lookingAtPlayer is the dot-product state gate, and customLookAt says whether our
        // drive is still pinned or BlueprintUpdateAnimation reclaimed lookAt.
        out.headAlpha = ReadAt<float>(anim, P::anim::kKerfurLookAt_1 + P::anim::SkelCtl_Alpha);
        out.neckAlpha = ReadAt<float>(anim, P::anim::kKerfurLookAt   + P::anim::SkelCtl_Alpha);
        out.lookingAtPlayer = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer());
        out.customLookAt    = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_customLookAt());
        out.haveGates = true;
    }
    // Resolved world rotation of the head and neck bones: the twist actually rendered.
    ue_wrap::FRotator hr{}, nr{};
    if (E::GetBoneWorldRotationByName(comp, L"head", hr)) {
        out.headWorldYaw = hr.Yaw; out.headWorldPitch = hr.Pitch; out.haveHead = true;
    }
    if (E::GetBoneWorldRotationByName(comp, L"neck", nr)) {
        out.neckWorldYaw = nr.Yaw; out.haveNeck = true;
    }
    return out.haveClamp || out.haveHead;
}

bool ReadKerfurLookAt(void* npcActor, FVector& outWorldTarget) {
    return ReadLookAtOnAnim(NpcBodyAnimInstance(npcActor), outWorldTarget);
}

void DriveKerfurLookAt(void* npcActor, const FVector& worldTarget) {
    WriteLookAtOnAnim(NpcBodyAnimInstance(npcActor), worldTarget);
}

bool ReadKerfurBodyYaw(void* npcActor, float& outYaw) {
    // The kerfur actor BP aims the visible body by rotating the mesh's world rotation, decoupled
    // from the actor root, per peer toward the local player. Read the resolved mesh world yaw on
    // the host so the mirror can reproduce it. Kerfur-family only.
    void* mesh = NpcBodyMesh(npcActor);
    if (!mesh || !IsKerfurAnimBP(LiveAnimInstance(mesh))) return false;
    outYaw = ue_wrap::engine::GetComponentWorldRotation(mesh).Yaw;
    return true;
}

void DriveKerfurBodyYaw(void* npcActor, float yaw) {
    // Drive a mirror kerfur's body facing by setting the mesh's world rotation to the streamed
    // yaw. The mirror's actor tick is off, so the BP's per-tick mesh rotation never runs there and
    // cannot clobber this. Must be called AFTER SetActorRotation: moving the actor root re-bases
    // this child mesh's world transform.
    void* mesh = NpcBodyMesh(npcActor);
    if (!mesh || !IsKerfurAnimBP(LiveAnimInstance(mesh))) return;
    ue_wrap::engine::SetComponentWorldRotation(mesh, ue_wrap::FRotator{0.f, yaw, 0.f});
}

void DriveCharacterMovement(void* puppetActor,
                            const FVector& worldVelocity,
                            bool inAir) {
    if (!puppetActor || !R::IsLive(puppetActor)) return;
    void* cmc = ReadPtr(puppetActor, P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return;
    // UMovementComponent::Velocity. The offset is hardcoded here rather than promoted into
    // sdk_profile.h so that every raw memory write stays inside ue_wrap; coop/ callers see only
    // this typed API.
    constexpr size_t kUMovementComponent_Velocity = 0xC4;
    WriteAt<FVector>(cmc, kUMovementComponent_Velocity, worldVelocity);
    const uint8_t mm = inAir ? P::off::kMOVE_Falling : uint8_t{1};  // MOVE_Walking
    WriteAt<uint8_t>(cmc, P::off::UCharacterMovement_MovementMode, mm);
}

void DriveSprintWalkSpeed(void* puppetActor, bool sprinting) {
    if (!puppetActor || !R::IsLive(puppetActor)) return;
    void* cmc = ReadPtr(puppetActor, P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return;
    // MaxWalkSpeed, for footstep-loudness parity. lib_C::step scales the step sound by
    // clamp(CMC.MaxWalkSpeed / speedVolume, 0.5, 2) with speedVolume defaulting to 400 -- it reads
    // the SETTING, not Velocity. A parked puppet CMC never runs mainPlayer's updateSpeed, so
    // without this write a sprinting remote sounds walk-quiet. Mirror the native knob: the class
    // default while walking, doubled while sprinting, as updateSpeed does (its further agility
    // lerp, up to a quarter more, is deliberately skipped).
    // Player puppets only. It is a separate function rather than a DriveCharacterMovement
    // parameter because npc_pose_drive shares that drive, and an NPC's MaxWalkSpeed must be
    // neither captured as nor overwritten with the mainPlayer class default. The default is
    // latched from the first player puppet CMC, where updateSpeed has never run.
    constexpr size_t kCMC_MaxWalkSpeed = 0x18C;
    static float sDefaultMaxWalk = 0.f;
    if (sDefaultMaxWalk <= 0.f) sDefaultMaxWalk = ReadAt<float>(cmc, kCMC_MaxWalkSpeed);
    if (sDefaultMaxWalk > 0.f) {
        WriteAt<float>(cmc, kCMC_MaxWalkSpeed,
                       sprinting ? sDefaultMaxWalk * 2.f : sDefaultMaxWalk);
    }
}

bool ReadCharacterIsFalling(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return false;
    const uint8_t mode = ReadAt<uint8_t>(cmc, P::off::UCharacterMovement_MovementMode);
    return mode == P::off::kMOVE_Falling;
}

void DisableCharacterTicks(void* actor) {
    if (!actor || !R::IsLive(actor)) return;
    DisableMovementTick(actor);
    // Actor tick off, to suppress the BP ReceiveTick graph -- for an NPC mirror, that graph is its
    // AI state machine. The AnimBP still ticks on the mesh, so it reads our per-tick CMC velocity
    // write.
    E::SetActorTickEnabled(actor, false);
}

void DisableMovementTick(void* actor) {
    if (!actor || !R::IsLive(actor)) return;
    // CMC tick off, to stop gravity and velocity integration so the networked SetActorLocation
    // drive is authoritative; we own CMC velocity and movement mode.
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        if (R::IsLive(cmc)) E::SetComponentTickEnabled(cmc, false);
    }
}

}  // namespace ue_wrap::puppet

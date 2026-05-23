#include "ue_wrap/puppet.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <unordered_map>

namespace ue_wrap::puppet {

namespace P = profile;
namespace R = reflection;
namespace E = engine;

namespace {

// Read/write a UObject* / POD at a fixed byte offset (raw engine memory access is
// allowed in the wrapper layer; offsets come from sdk_profile.h, verified vs the
// CXX dump).
inline void* ReadPtr(void* base, size_t off) {
    return base ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) : nullptr;
}
template <class T>
inline T ReadAt(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}
template <class T>
inline void WriteAt(void* base, size_t off, T value) {
    *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off) = value;
}

// puppet actor -> its cached SkeletalMeshComponent (avoid a GUObjectArray walk
// per Drive() frame).
std::unordered_map<void*, void*> g_meshComp;

// The live AnimInstance running on a SkeletalMeshComponent (comp + AnimScriptInstance).
void* LiveAnimInstance(void* skeletalMeshComponent) {
    return ReadPtr(skeletalMeshComponent, P::off::USkeletalMesh_AnimScriptInstance);
}

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
    // Snapshot the LOCAL working body's AnimBP state so SpawnPuppet's puppet dump
    // can be diffed against it (the "diff observable state" rule): if the puppet
    // is still a stick, the diff pinpoints which variable to set.
    DumpAnimState(L"local", comp);
    return meshAsset;
}

void* GetMeshPlayerVisibleComponent(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return nullptr;
    return ReadPtr(mainPlayerPawn, P::off::AmainPlayer_mesh_playerVisible);
}

float GetCapsuleHalfHeight(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return 0.f;
    void* capsule = ReadPtr(mainPlayerPawn, P::off::ACharacter_CapsuleComponent);
    if (!capsule) return 0.f;
    return ReadAt<float>(capsule, P::off::UCapsuleComponent_CapsuleHalfHeight);
}

void* GetMeshPlayerVisibleAnimClass(void* mainPlayerPawn) {
    if (!mainPlayerPawn) return nullptr;
    void* comp = ReadPtr(mainPlayerPawn, P::off::AmainPlayer_mesh_playerVisible);
    if (!comp) return nullptr;
    void* animClass = ReadPtr(comp, P::off::USkeletalMesh_AnimClass);
    UE_LOGI("puppet: local AnimClass = %ls (%p)", R::ClassNameOf(animClass).c_str(), animClass);
    return animClass;
}

void* GetSkeletalMeshComponent(void* puppetActor) {
    if (!puppetActor) return nullptr;
    // If the puppet was destroyed (level change), its cached component is freed
    // too -- drop the stale entry instead of returning a dangling pointer that
    // DriveAnimBP would then read at +0x6B0 (AV).
    if (!R::IsLive(puppetActor)) { g_meshComp.erase(puppetActor); return nullptr; }
    auto it = g_meshComp.find(puppetActor);
    if (it != g_meshComp.end()) return it->second;
    void* comp = nullptr;
    for (const auto& c : R::ChildObjectsOf(puppetActor)) {
        if (c.className == P::name::SkeletalMeshComponentClass) { comp = c.object; break; }
    }
    if (comp) g_meshComp[puppetActor] = comp;
    return comp;
}

void DumpAnimState(const wchar_t* label, void* skeletalMeshComponent) {
    void* anim = LiveAnimInstance(skeletalMeshComponent);
    if (!anim) {
        UE_LOGW("puppet: [%ls] AnimInstance NULL (comp=%p) -> mesh has no live AnimBP "
                "(would render as a static/reference-pose stick)", label, skeletalMeshComponent);
        return;
    }
    const float spd = ReadAt<float>(anim, P::off::AnimBP_kerfur_spd);
    const float walkSpeed = ReadAt<float>(anim, P::off::AnimBP_kerfur_walkSpeed);
    void* pawn = ReadPtr(anim, P::off::AnimBP_kerfur_Pawn);
    void* ctrl = ReadPtr(anim, P::off::AnimBP_kerfur_Controller);
    void* kerfur = ReadPtr(anim, P::off::AnimBP_kerfur_kerfur);
    const bool useLegIK = ReadAt<bool>(anim, P::off::AnimBP_kerfur_useLegIK);
    const bool isFace = ReadAt<bool>(anim, P::off::AnimBP_kerfur_isFace);
    const bool lookingAtPlayer = ReadAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer);
    UE_LOGI("puppet: [%ls] AnimInstance=%ls(%p) spd=%.1f walkSpeed=%.1f "
            "Pawn=%p Controller=%p kerfur=%p useLegIK=%d isFace=%d lookingAtPlayer=%d",
            label, R::ClassNameOf(anim).c_str(), anim, spd, walkSpeed,
            pawn, ctrl, kerfur, useLegIK, isFace, lookingAtPlayer);
}

void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass) {
    if (!skeletalMeshAsset) {
        UE_LOGE("puppet: SpawnPuppet no skin asset");
        return nullptr;
    }
    void* cls = R::FindClass(P::name::SkeletalMeshActorClass);
    if (!cls) {
        UE_LOGE("puppet: SkeletalMeshActor class not found");
        return nullptr;
    }
    // A SkeletalMeshActor is NOT a pawn -> no auto-possess, no hijack surface.
    void* actor = E::SpawnActor(cls, loc, /*inertPawn=*/false);
    if (!actor) {
        UE_LOGE("puppet: SpawnActor(SkeletalMeshActor) failed");
        return nullptr;
    }
    void* comp = GetSkeletalMeshComponent(actor);
    if (!comp) {
        UE_LOGE("puppet: spawned actor %p has no SkeletalMeshComponent", actor);
        return actor;
    }

    // Wear the local player's skin + run the body AnimBP. SetAnimClass also flips
    // AnimationMode to UseAnimBlueprint and instantiates the AnimInstance. animClass
    // is read off the LOCAL working body (lookup-by-name fails for BP-generated
    // classes), so it's guaranteed valid.
    E::SetSkeletalMesh(comp, skeletalMeshAsset);
    if (animClass) {
        E::SetAnimClass(comp, animClass);
    } else {
        UE_LOGW("puppet: no AnimClass -> body renders in reference pose (T-pose)");
    }
    E::SetAnimTickAlways(comp);          // pose even when not the rendered viewpoint
    E::SetComponentVisible(comp, true);

    // Suppress the floor-trace leg IK (it needs world/owner context a bare actor
    // lacks; left on it splays the legs). Idle by default (spd/walkSpeed = 0).
    // Also disable the head-look IK: the AnimBP defaults lookingAtPlayer=1 which
    // makes the head track "the player", but on a puppet there's no valid local
    // player to read -> the head ends up twisted to the side. The PUPPET's head
    // should follow the SOURCE PLAYER's view direction (already encoded in the
    // streamed yaw), not chase a phantom local-player target.
    if (void* anim = LiveAnimInstance(comp)) {
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_useLegIK, false);
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer, false);
        WriteAt<float>(anim, P::off::AnimBP_kerfur_walkSpeedMultiplier, 1.f);
    }

    UE_LOGI("puppet: spawned actor=%p comp=%p at (%.0f,%.0f,%.0f)",
            actor, comp, loc.X, loc.Y, loc.Z);
    DumpAnimState(L"puppet", comp);
    return actor;
}

void DriveAnimBP(void* puppetActor, float speed, float headPitch, float headYawDelta) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    // The actor slot can still pass IsLive for a tick while its child component is
    // already being torn down (UE finalizes sub-objects first). Re-check the
    // component before reading AnimScriptInstance @ +0x6B0.
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;
    WriteAt<float>(anim, P::off::AnimBP_kerfur_walkSpeed, speed);
    WriteAt<float>(anim, P::off::AnimBP_kerfur_spd, speed);
    // Drive the puppet head bone from the streamed view.
    // AnimBP_kerfur_headLookAt is the AnimBP-exposed FRotator for head IK.
    //   Pitch: streamed view pitch -- head tilts up/down with the source.
    //   Yaw:   head yaw delta vs body. Currently 0 -- the source's body lag
    //          already encodes the head-leads-body lean; replicate it by
    //          streaming actor yaw (not camera yaw) into bodyRotation. The
    //          parameter is kept for a future "free-look" mode.
    // With lookingAtPlayer=false (set in SpawnPuppet), the AnimBP graph uses
    // headLookAt as the explicit head-look rotation rather than chasing a
    // phantom camera target.
    const FRotator headLook{headPitch, headYawDelta, 0.f};
    WriteAt<FRotator>(anim, P::off::AnimBP_kerfur_headLookAt, headLook);
}

}  // namespace ue_wrap::puppet

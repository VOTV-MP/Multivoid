#include "ue_wrap/puppet.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace ue_wrap::puppet {

namespace P = profile;
namespace R = reflection;
namespace E = engine;
namespace GT = game_thread;

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

// Plan B1 (BUA interceptor) RETIRED 2026-05-23 in favour of Plan B2 (satellite
// Character drives the AnimBP via real Pawn->Movement->Velocity pull). The
// satellite makes BUA's normal velocity-pull work; intercepting and skipping
// it caused the AnimInstance to be vastly under-populated (no Movement, no
// IK fields), which kept the state machine stuck in idle even when spd was
// being written. See [[project-bug2-locomotion-anim]] memo for the full
// investigation trace.

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
    if (it != g_meshComp.end()) {
        // The actor can outlive its child component for a tick mid-tear-down
        // (UE finalizes sub-objects first). A live actor with a dying cached
        // component must NOT return the dying pointer -- the caller would then
        // read AnimScriptInstance @+0x6B0 on freed memory (AV). Treat as a
        // cache miss; drop the entry and fall through to re-resolve. Post-ship
        // audit catch: callers shouldn't have to repeat this guard themselves
        // (current single caller DriveAnimBP does, but a future caller would
        // not know to).
        if (R::IsLive(it->second)) return it->second;
        g_meshComp.erase(it);
    }
    void* comp = nullptr;
    for (const auto& c : R::ChildObjectsOf(puppetActor)) {
        if (c.className == P::name::SkeletalMeshComponentClass) { comp = c.object; break; }
    }
    if (comp) g_meshComp[puppetActor] = comp;
    return comp;
}

// Bug 2 deep diagnostic 2026-05-23: BUA interceptor is firing every frame writing
// correct spd values (290 cm/s while remote walks), but the puppet STILL doesn't
// animate -- so spd isn't reaching the BlendSpace, or a state machine gates the
// transition. Dump the live FAnimNode_* memory regions (offsets per the CXX dump:
// BlendSpacePlayer @ 0x1180 sz 0xE8; StateMachine @ 0x1AC0 sz 0xB0;
// StateMachine_1 @ 0x1CC8 sz 0xB0) and log all non-trivial floats+ints. Compare
// LOCAL (walking, animates correctly) vs PUPPET (sliding) to find the offset
// where walking-speed appears on local and to see if puppet state index is
// stuck on idle.
void DumpAnimNodeRegions(const wchar_t* label, void* skeletalMeshComponent) {
    void* anim = LiveAnimInstance(skeletalMeshComponent);
    if (!anim) return;
    struct Region { const char* name; size_t start; size_t end; };
    const Region regions[] = {
        {"BlendSpacePlayer", 0x1180, 0x1268},
        {"StateMachine_1",   0x1AC0, 0x1B70},
        {"StateMachine",     0x1CC8, 0x1D78},
        // The AnimBP's INSTANCE-LEVEL public variables block (post-AnimGraphNode
        // tail, per CXX dump line 82+). Bug 2 deep dive 2026-05-23: the state
        // machine differs (idx 1 vs 2) between local-walking and puppet-sliding,
        // but DumpAnimState only logs the KNOWN AnimBP vars from sdk_profile.
        // Dump the WHOLE block (size_t from 0x2D60 through end-of-class 0x2E4A,
        // covering ALL kerfur-AnimBP variables + any padding/inherited tail)
        // so any field difference between local and puppet shows up.
        {"AnimBP_vars_all", 0x2D60, 0x2E50},
    };
    for (const Region& r : regions) {
        std::string out;
        for (size_t off = r.start; off < r.end; off += 4) {
            uint8_t* p = static_cast<uint8_t*>(anim) + off;
            const float fv = *reinterpret_cast<float*>(p);
            const int32_t iv = *reinterpret_cast<int32_t*>(p);
            // log only non-zero values, in either float or int form (engine
            // values are typically floats or small ints; pointers would show as
            // huge ints that we filter by upper bound).
            const bool floatNontrivial = std::isfinite(fv) && std::fabs(fv) > 0.0001f && std::fabs(fv) < 1.0e6f;
            const bool intNontrivial = (iv != 0 && iv > -1000000 && iv < 1000000);
            if (floatNontrivial || intNontrivial) {
                char buf[96];
                snprintf(buf, sizeof(buf), "    +0x%04zX: f=%9.3f  i=%d\n", off, fv, iv);
                out += buf;
            }
        }
        UE_LOGI("anim[%ls] %s @ +0x%04zX-+0x%04zX:\n%s",
                label, r.name, r.start, r.end, out.c_str());
    }
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
    // animWalkAlpha + animWalkRate: kept in the dump as observable AnimBP state.
    // The Plan A hypothesis that animWalkAlpha gates idle-vs-walk was DISPROVED
    // by the 2026-05-23 spawn diagnostic -- the LOCAL has animWalkAlpha=0.00
    // while WALKING. spd is the actual locomotion driver (BlendSpace X input),
    // which Plan B1's BUA interceptor pushes from the network speed.
    const float animWalkAlpha = ReadAt<float>(anim, P::off::AnimBP_kerfur_animWalkAlpha);
    const float animWalkRate = ReadAt<float>(anim, P::off::AnimBP_kerfur_animWalkRate);
    void* pawn = ReadPtr(anim, P::off::AnimBP_kerfur_Pawn);
    void* ctrl = ReadPtr(anim, P::off::AnimBP_kerfur_Controller);
    void* movement = ReadPtr(anim, P::off::AnimBP_kerfur_Movement);
    void* kerfur = ReadPtr(anim, P::off::AnimBP_kerfur_kerfur);
    const bool useLegIK = ReadAt<bool>(anim, P::off::AnimBP_kerfur_useLegIK);
    const bool isFace = ReadAt<bool>(anim, P::off::AnimBP_kerfur_isFace);
    const bool lookingAtPlayer = ReadAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer);
    UE_LOGI("puppet: [%ls] AnimInstance=%ls(%p) spd=%.1f walkSpeed=%.1f "
            "animWalkAlpha=%.2f animWalkRate=%.2f "
            "Pawn=%p Controller=%p Movement=%p kerfur=%p "
            "useLegIK=%d isFace=%d lookingAtPlayer=%d",
            label, R::ClassNameOf(anim).c_str(), anim, spd, walkSpeed,
            animWalkAlpha, animWalkRate,
            pawn, ctrl, movement, kerfur, useLegIK, isFace, lookingAtPlayer);
}

void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass,
                  void* srcMeshComp) {
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

    // Mirror the LOCAL mainPlayer_C::mesh_playerVisible's parent-relative
    // transform onto the puppet's SkeletalMeshComponent. The player BP authors
    // mesh_playerVisible with a non-trivial RelativeLocation (the Z shim between
    // the actor centre and the mesh asset's root bone -- the source of the
    // float-above-ground that's existed since the start of the project) AND a
    // non-trivial RelativeRotation (yaw -90 reconciling the mesh's "mesh +Y
    // forward" authoring with UE4's actor "+X forward" convention -- what made
    // puppets visually 90 deg sideways without manual compensation).
    //
    // Both peers run mainPlayer_C, so the LOCAL on the receiving machine carries
    // the same BP-authored RelLoc/RelRot the source's mesh_playerVisible has.
    // Reading it at runtime makes the puppet visually identical to the source's
    // body, no hard-coded magic numbers, robust to mesh-asset re-authoring
    // across game versions. Written here BEFORE the next SetActorLocation in
    // RemotePlayer::Spawn -> the world transform refresh propagates through
    // AttachChildren and picks up the new RelLoc/RelRot naturally.
    if (srcMeshComp) {
        const FVector srcRelLoc = ReadAt<FVector>(srcMeshComp, P::off::USceneComponent_RelativeLocation);
        const FRotator srcRelRot = ReadAt<FRotator>(srcMeshComp, P::off::USceneComponent_RelativeRotation);
        WriteAt<FVector>(comp, P::off::USceneComponent_RelativeLocation, srcRelLoc);
        WriteAt<FRotator>(comp, P::off::USceneComponent_RelativeRotation, srcRelRot);
        UE_LOGI("puppet: mirrored src mesh RelLoc=(%.1f,%.1f,%.1f) RelRot=(P=%.1f,Y=%.1f,R=%.1f) onto puppet comp=%p",
                srcRelLoc.X, srcRelLoc.Y, srcRelLoc.Z,
                srcRelRot.Pitch, srcRelRot.Yaw, srcRelRot.Roll, comp);
    } else {
        UE_LOGW("puppet: no source mesh component -> puppet renders at default mesh RelLoc/RelRot "
                "(may float above ground / face sideways)");
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

void DriveAnimBP(void* puppetActor, float /*speed*/, float headPitch, float headYawDelta) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    // The actor slot can still pass IsLive for a tick while its child component is
    // already being torn down (UE finalizes sub-objects first). Re-check the
    // component before reading AnimScriptInstance @ +0x6B0.
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;

    // spd is now driven by BUA reading the satellite Character's
    // CharacterMovementComponent.Velocity (Plan B2). headLookAt remains driven
    // here because BUA does not write it (the local-vs-puppet diff confirmed
    // it stays at whatever we set), and it's directly the head-bone target.

    // Drive the puppet head bone from the streamed view. headLookAt is NOT
    // written by BUA (BUA only touches velocity-derived vars), so end-of-frame
    // writes here survive into the next AnimGraph evaluation.
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

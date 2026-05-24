#include "ue_wrap/puppet.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>  // GetEnvironmentVariableW

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

void DumpKerfurHeadGraph(void* skeletalMeshComponent) {
    void* anim = LiveAnimInstance(skeletalMeshComponent);
    if (!anim) { UE_LOGW("puppet: DumpKerfurHeadGraph: no AnimInstance"); return; }
    auto bn = reinterpret_cast<uint8_t*>(anim);
    // Read FName at BoneToModify (FBoneReference.BoneName @ +0 of the struct).
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

bool IsMainPlayerPuppetKind() {
    // Read VOTVCOOP_PUPPET_KIND env var ONCE on first call; cache the result.
    // Default true (mainPlayer_C path). Any value starting with 's' (skelmesh,
    // skel, smc) flips to the SkeletalMeshActor backup path.
    static bool resolved = false;
    static bool isMainPlayer = true;
    if (!resolved) {
        wchar_t buf[64] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"VOTVCOOP_PUPPET_KIND", buf, 64);
        if (n > 0 && n < 64) {
            const wchar_t c = static_cast<wchar_t>(::towlower(buf[0]));
            if (c == L's') isMainPlayer = false;
        }
        UE_LOGI("puppet: puppet-kind = %ls (VOTVCOOP_PUPPET_KIND=%ls)",
                isMainPlayer ? L"MainPlayer (mainPlayer_C orphan)"
                             : L"SkelMesh (ASkeletalMeshActor backup)",
                n > 0 ? buf : L"<unset, default mainplayer>");
        resolved = true;
    }
    return isMainPlayer;
}

float GetSpawnMeshOffsetZ(void* localPlayer) {
    if (IsMainPlayerPuppetKind()) return 0.f;
    if (!localPlayer || !R::IsLive(localPlayer)) return 0.f;
    return -E::GetActorCharacterHalfHeight(localPlayer);
}

// Backup path: spawn ASkeletalMeshActor wearing the local player's skin +
// running the body AnimBP. RULE 2 retirement: this whole function goes when
// the MainPlayer path is hands-on-verified working (criteria documented in
// puppet.h).
static void* SpawnPuppetSkelMesh(const FVector& loc, void* skeletalMeshAsset, void* animClass) {
    if (!skeletalMeshAsset) {
        UE_LOGE("puppet[SkelMesh]: no skin asset");
        return nullptr;
    }
    void* cls = R::FindClass(P::name::SkeletalMeshActorClass);
    if (!cls) {
        UE_LOGE("puppet[SkelMesh]: SkeletalMeshActor class not found");
        return nullptr;
    }
    // A SkeletalMeshActor is NOT a pawn -> no auto-possess, no hijack surface.
    void* actor = E::SpawnActor(cls, loc, /*inertPawn=*/false);
    if (!actor) {
        UE_LOGE("puppet[SkelMesh]: SpawnActor failed");
        return nullptr;
    }
    void* comp = GetSkeletalMeshComponent(actor);
    if (!comp) {
        UE_LOGE("puppet[SkelMesh]: spawned actor %p has no SkeletalMeshComponent", actor);
        return actor;
    }
    E::SetSkeletalMesh(comp, skeletalMeshAsset);
    if (animClass) {
        E::SetAnimClass(comp, animClass);
    } else {
        UE_LOGW("puppet[SkelMesh]: no AnimClass -> body renders in reference pose (T-pose)");
    }
    E::SetAnimTickAlways(comp);
    E::SetComponentVisible(comp, true);
    // Suppress floor-trace leg IK (needs Character context a bare ASkeletalMesh
    // Actor lacks; on it splays the legs) + decouple head from local-player
    // track.
    if (void* anim = LiveAnimInstance(comp)) {
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_useLegIK, false);
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer, false);
        WriteAt<float>(anim, P::off::AnimBP_kerfur_walkSpeedMultiplier, 1.f);
    }
    UE_LOGI("puppet[SkelMesh]: spawned actor=%p comp=%p at (%.0f,%.0f,%.0f)",
            actor, comp, loc.X, loc.Y, loc.Z);
    DumpAnimState(L"puppet", comp);
    return actor;
}

// New path: spawn mainPlayer_C orphan. The class's built-in mesh_playerVisible
// carries the player body skin + IK leg bones + the AnimBP all pre-wired by
// class defaults. We just neuter the per-screen systems (PostProcess
// components affect the local view; FP arms are camera-space attached) and
// flip a few AnimBP flags. 2026-05-25 per
// research/findings/votv-puppet-mainplayer-body-RE-2026-05-25.md.
static void* SpawnPuppetMainPlayer(const FVector& loc) {
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) {
        UE_LOGE("puppet[MainPlayer]: mainPlayer_C class not found");
        return nullptr;
    }
    // 2026-05-25 audit: BEFORE spawning the orphan, capture the running
    // gamemode's mainPlayer reference. The orphan's BeginPlay runs
    // intComs_gamemodeBeginPlay which (per the BP pattern) writes
    // gamemode.mainPlayer = caller. We restore the captured pointer
    // after spawn so the gamemode's save/sleep/damage paths keep
    // operating on the REAL local player.
    void* gamemode = R::FindObjectByClass(P::name::GamemodeClass);
    void* gmMainPlayerBefore = nullptr;
    if (gamemode) {
        gmMainPlayerBefore = ReadPtr(gamemode, P::off::mainGamemode_mainPlayer);
    } else {
        UE_LOGW("puppet[MainPlayer]: no live gamemode found -- cannot capture mainPlayer pointer for restore");
    }
    // inertPawn=true zeros AutoPossessPlayer/AutoPossessAI/AutoReceiveInput
    // and sets bBlockInput in the deferred-spawn window BEFORE BeginPlay,
    // so no 2nd PlayerController auto-possesses and no local input gets
    // hijacked (RULE 1 root-cause prevention).
    void* actor = E::SpawnActor(cls, loc, /*inertPawn=*/true);
    if (!actor) {
        UE_LOGE("puppet[MainPlayer]: SpawnActor(mainPlayer_C) failed");
        return nullptr;
    }
    // 2026-05-25 audit fix (CRITICAL #1): restore gamemode.mainPlayer if
    // the orphan's BeginPlay overwrote it (intComs_gamemodeBeginPlay
    // pattern). Without this, the gamemode's autosave timer would
    // serialize the orphan's position as the save's player transform,
    // corrupting the save.
    if (gamemode) {
        void* gmMainPlayerAfter = ReadPtr(gamemode, P::off::mainGamemode_mainPlayer);
        if (gmMainPlayerAfter != gmMainPlayerBefore) {
            E::WriteObjectField(gamemode, P::off::mainGamemode_mainPlayer, gmMainPlayerBefore);
            UE_LOGI("puppet[MainPlayer]: gamemode.mainPlayer was overwritten by orphan (%p -> %p); restored to %p",
                    gmMainPlayerBefore, gmMainPlayerAfter, gmMainPlayerBefore);
        }
    }
    // 2026-05-25 audit fix (CRITICAL #2): null the orphan's cached
    // GameMode pointer so subsequent ReceiveTick BP graphs that read it
    // (and the standard VOTV null-check pattern returns early) don't
    // re-overwrite gamemode.mainPlayer or invoke gamemode methods.
    E::WriteObjectField(actor, P::off::AmainPlayer_GameMode, nullptr);
    UE_LOGI("puppet[MainPlayer]: nulled orphan.GameMode @0x0C80 (disconnected from gamemode)");

    // Read mesh_playerVisible by DIRECT OFFSET (not ChildObjectsOf search):
    // mainPlayer_C has multiple SkeletalMeshComponents (ACharacter::Mesh
    // @0x0280 + mesh_playerVisible @0x04F8 + arms @0x05F8 + playermodel
    // @0x0638). Child-search would return whichever loads first -- not
    // necessarily the body we want.
    void* meshComp = ReadPtr(actor, P::off::AmainPlayer_mesh_playerVisible);
    if (!meshComp) {
        UE_LOGE("puppet[MainPlayer]: actor %p has no mesh_playerVisible @0x04F8", actor);
        return actor;
    }
    // Cache for GetSkeletalMeshComponent (the DriveAnimBP path).
    g_meshComp[actor] = meshComp;

    // 2026-05-25 audit fix (CRITICAL #3): disable the orphan's
    // CharacterMovementComponent tick. CMC runs gravity+walking
    // integration each tick and would fight ApplyToEngine's
    // SetActorLocation writes (rubber-banding the puppet between our
    // wire-driven position and CMC's gravity-integrated position).
    // Mirror the satellite's CMC disable.
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        E::SetComponentTickEnabled(cmc, false);
        UE_LOGI("puppet[MainPlayer]: disabled orphan CMC tick @ %p (puppet driven by SetActorLocation, not physics)", cmc);
    }

    // Neuter per-screen post-processing -- both PostProcessComponents drive
    // the LOCAL camera's color/exposure/gamma; leaving them alive on a
    // puppet would corrupt whichever screen renders the puppet.
    if (void* pp1 = ReadPtr(actor, P::off::AmainPlayer_PostProcess_overlays_OBSOLETE)) {
        if (E::DestroyComponent(pp1, actor)) {
            UE_LOGI("puppet[MainPlayer]: destroyed PostProcess_overlays_OBSOLETE @ %p", pp1);
        }
    }
    if (void* pp2 = ReadPtr(actor, P::off::AmainPlayer_PostProcess_pl)) {
        if (E::DestroyComponent(pp2, actor)) {
            UE_LOGI("puppet[MainPlayer]: destroyed PostProcess_pl @ %p", pp2);
        }
    }
    // 2026-05-25 audit fix (HIGH #4): destroy the mic
    // UAudioCaptureComponent. Without this, the orphan captures audio
    // from the default input device into a sink nobody reads (latent
    // device hold + wasted resource).
    if (void* mic = ReadPtr(actor, P::off::AmainPlayer_mic)) {
        if (E::DestroyComponent(mic, actor)) {
            UE_LOGI("puppet[MainPlayer]: destroyed mic UAudioCaptureComponent @ %p", mic);
        }
    }
    // Hide FP arms viewmodel (camera-space attached -- wrong on a 3rd-
    // person-observed puppet). SetComponentVisible(false) sets BOTH
    // SetVisibility(false) and SetHiddenInGame(true) (propagated).
    if (void* arms = ReadPtr(actor, P::off::AmainPlayer_arms)) {
        E::SetComponentVisible(arms, false);
        UE_LOGI("puppet[MainPlayer]: hid FP arms @ %p", arms);
    }
    // 2026-05-25 audit (HIGH #5): hide playermodel if it carries a
    // visible mesh asset. This is the legacy equipment-overlay slot;
    // typically null/hidden on the player but verify per build.
    if (void* playermodel = ReadPtr(actor, P::off::AmainPlayer_playermodel)) {
        E::SetComponentVisible(playermodel, false);
        UE_LOGI("puppet[MainPlayer]: hid playermodel @ %p (legacy equipment overlay)", playermodel);
    }
    // Hide the ACharacter native Mesh slot if distinct from mesh_playerVisible.
    if (void* nativeMesh = ReadPtr(actor, P::off::ACharacter_Mesh)) {
        if (nativeMesh != meshComp) {
            E::SetComponentVisible(nativeMesh, false);
            UE_LOGI("puppet[MainPlayer]: hid ACharacter::Mesh @ %p (the unused native slot)", nativeMesh);
        }
    }
    // mesh_playerVisible: force always-tick + visible. Class default may
    // have VisibilityBasedAnimTick=OnlyTickPoseWhenRendered which would
    // collapse the puppet to a stick when not on screen.
    E::SetAnimTickAlways(meshComp);
    E::SetComponentVisible(meshComp, true);

    // AnimBP setup: IK legs ON (real Character has floor-trace context via
    // the satellite Plan B2), removeArms ON (avoid grab-pose arm flail),
    // head-look-at-player OFF (the streamed yaw via DriveAnimBP drives the
    // head bone directly).
    if (void* anim = LiveAnimInstance(meshComp)) {
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_useLegIK,        true);
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_removeArms,      true);
        WriteAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer, false);
        WriteAt<float>(anim, P::off::AnimBP_kerfur_walkSpeedMultiplier, 1.f);
    }
    UE_LOGI("puppet[MainPlayer]: spawned actor=%p mesh_playerVisible=%p at (%.0f,%.0f,%.0f)",
            actor, meshComp, loc.X, loc.Y, loc.Z);
    DumpAnimState(L"puppet", meshComp);
    return actor;
}

void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass) {
    // Branch on env-var-controlled puppet kind. Default MainPlayer per user
    // directive 2026-05-25 (puppet must be full body+legs+IK, not bare
    // SkeletalMeshActor).
    if (IsMainPlayerPuppetKind()) {
        // skin + animClass params unused on this path (class defaults set
        // them); kept on the public API while SkelMesh backup remains. RULE
        // 2 retirement drops both params + the SkelMesh branch.
        (void)skeletalMeshAsset; (void)animClass;
        return SpawnPuppetMainPlayer(loc);
    }
    return SpawnPuppetSkelMesh(loc, skeletalMeshAsset, animClass);
}

void DriveAnimBP(void* puppetActor, float /*speed*/, float headPitch, float headYawDelta) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    // The actor slot can still pass IsLive for a tick while its child component is
    // already being torn down (UE finalizes sub-objects first). Re-check the
    // component before reading AnimScriptInstance @ +0x6B0.
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;

    // spd is driven by BUA reading the satellite Character's
    // CharacterMovementComponent.Velocity (Plan B2).
    //
    // The visible head twist on the kerfur AnimBP comes from TWO native-engine
    // FAnimNode_LookAt skeletal-control nodes (proven by IDA agent 2026-05-23 PM
    // -- offsets 0x1730 / 0x18E0 in the AnimInstance, target bones 'head' /
    // 'neck' respectively). These nodes evaluate their LookAtLocation each
    // frame and rotate the bones toward it; the AnimBP property
    // 'lookingAtPlayer' does NOT control whether they're active (live diagnostic
    // dump confirmed Alpha=1.0 / 0.5 on a puppet that had lookingAtPlayer=false
    // at spawn). To DECOUPLE the puppet's head from the local-player track:
    //
    //   1. Zero each LookAt node's Alpha + clear bAlphaBoolEnabled. The
    //      FAnimNode_LookAt::Evaluate still runs but contributes zero blend
    //      weight -- the head stays at the BlendSpace pose.
    //
    //   2. Drive AnimGraphNode_ModifyBone @0x2C60 (the one targeting 'head' --
    //      confirmed via DumpKerfurHeadGraph) with the streamed view pitch
    //      and head-yaw-delta. RotationMode = Replace so our value overrides
    //      whatever the BlendSpace put on the head bone.
    //
    // This is RULE-1 surgical: we touch exactly the AnimGraph nodes that drive
    // the head bone, not broad AnimBP-property writes that don't work.
    {
        // (1) Disable both LookAt nodes (head + neck).
        auto bn = reinterpret_cast<uint8_t*>(anim);
        *reinterpret_cast<float*>(bn + P::anim::kKerfurLookAt_1 + P::anim::SkelCtl_Alpha) = 0.f;
        *reinterpret_cast<bool*> (bn + P::anim::kKerfurLookAt_1 + P::anim::SkelCtl_bAlphaBoolEnabled) = false;
        *reinterpret_cast<float*>(bn + P::anim::kKerfurLookAt   + P::anim::SkelCtl_Alpha) = 0.f;
        *reinterpret_cast<bool*> (bn + P::anim::kKerfurLookAt   + P::anim::SkelCtl_bAlphaBoolEnabled) = false;
        // (2) Drive head-bone ModifyBone (instance @0x2C60 -- the one whose
        //     BoneToModify == 'head' per the live diagnostic). RotationMode =
        //     Replace (2) so our absolute view direction overrides the base
        //     animation pose. Roll = 0 (head doesn't tilt sideways).
        *reinterpret_cast<FRotator*>(bn + P::anim::kKerfurModifyBone + P::anim::ModBone_Rotation) =
            FRotator{headPitch, headYawDelta, 0.f};
        *(bn + P::anim::kKerfurModifyBone + P::anim::ModBone_RotationMode) = 2;  // BMM_Replace
    }

    // Belt-and-braces: zero the AnimBP-property lookingAtPlayer too. It may not
    // gate the LookAt nodes directly, but the BP graph elsewhere might use it
    // (e.g. selecting between target sources). Cheap, no downside.
    WriteAt<bool>(anim, P::off::AnimBP_kerfur_lookingAtPlayer, false);
    // headLookAt remains a useful auxiliary write -- if any other graph path
    // reads it (e.g. additive blend on top of the ModifyBone), the value is
    // consistent with what we wrote to ModifyBone above.
    WriteAt<FRotator>(anim, P::off::AnimBP_kerfur_headLookAt, FRotator{headPitch, headYawDelta, 0.f});
}

}  // namespace ue_wrap::puppet

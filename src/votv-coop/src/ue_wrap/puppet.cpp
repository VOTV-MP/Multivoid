#include "ue_wrap/puppet.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

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

// Plan B1 (BUA interceptor) and Plan B2 (satellite ACharacter feeding the
// AnimBP's Pawn pointer) are both retired. v2 (2026-05-27, see
// research/findings/votv-local-anim-drive-RE-2026-05-27.md) writes
// Velocity + MovementMode directly on the puppet's OWN CMC each tick
// (CMC tick is parked, so we own those fields). BUA reads them naturally
// via Pawn=orphan -> CMC=orphan.CMC, exactly like the LOCAL player's
// possessed CMC -- same spd / IK gate behaviour with zero AnimInstance
// pointer-redirect plumbing.

// The live AnimInstance running on a SkeletalMeshComponent (comp + AnimScriptInstance).
void* LiveAnimInstance(void* skeletalMeshComponent) {
    return ReadPtr(skeletalMeshComponent, P::off::USkeletalMesh_AnimScriptInstance);
}

// ---- kerfur head-look (v39) ----------------------------------------------
// Guard for every lookAt/customLookAt access: is this AnimInstance a kerfur-family AnimBP?
// A different AnimBP would have unrelated fields at 0x2D90/0x2E49, so an unguarded write
// there would corrupt foreign state. The class ptr resolves once + caches; re-resolves while
// still null (the BP class loads with the level, possibly after the first NPC streams).
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

// An NPC's body skeletal-mesh COMPONENT: ACharacter::Mesh @0x0280. Real kerfur NPCs run the
// AnimBP on (and show their visible body as) the native ACharacter mesh slot -- their own
// component list has no body skeletal mesh, only particles/static/outfits -- unlike mainPlayer_C
// which uses mesh_playerVisible @0x04F8. Null if not resolvable.
void* NpcBodyMesh(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* mesh = ReadPtr(actor, P::off::ACharacter_Mesh);
    if (!mesh || !R::IsLive(mesh)) return nullptr;
    return mesh;
}

// That mesh's live AnimInstance (AnimScriptInstance @0x6B0). Null if not resolvable.
void* NpcBodyAnimInstance(void* actor) {
    void* mesh = NpcBodyMesh(actor);
    return mesh ? LiveAnimInstance(mesh) : nullptr;
}

// Read/write the kerfur head-look on an already-resolved AnimInstance (class-gated; offsets
// reflection-resolved, recook-proof). WriteLookAtOnAnim also sets customLookAt=true so the
// AnimInstance's own BUA stops overwriting lookAt with its local player camera.
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
        // cache miss; drop the entry and fall through to re-resolve.
        if (R::IsLive(it->second)) return it->second;
        g_meshComp.erase(it);
    }
    // 2026-05-25 audit fix (post-ship CRITICAL-3): on cache miss for the
    // mainPlayer_C puppet path, read mesh_playerVisible @0x04F8
    // DIRECTLY. mainPlayer_C has FOUR SkeletalMeshComponents (the
    // ACharacter::Mesh native slot @0x0280, mesh_playerVisible @0x04F8,
    // arms @0x05F8, playermodel @0x0638); ChildObjectsOf returns
    // whichever appears first in GUObjectArray order, which on this
    // class is the native Mesh slot (typically hidden + has no AnimBP).
    // DriveAnimBP then dispatches to the wrong AnimInstance.
    // Audit H9 (2026-05-27): MainPlayer is the only puppet kind. Read
    // mesh_playerVisible @0x04F8 directly. ChildObjectsOf fallback removed
    // (was the SkelMesh path's single-skel-comp resolver, which can no
    // longer happen).
    void* comp = ReadPtr(puppetActor, P::off::AmainPlayer_mesh_playerVisible);
    if (comp && !R::IsLive(comp)) comp = nullptr;
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
    // Offsets named in sdk_profile.h::anim (audit M19, 2026-05-27 -- moved
    // out of inline magic numbers so a future VOTV recook surfaces here
    // via the same sdk_profile.h::anim block other AnimBP offsets live in).
    const Region regions[] = {
        {"BlendSpacePlayer", P::anim::kKerfurBlendSpacePlayer_Start, P::anim::kKerfurBlendSpacePlayer_End},
        {"StateMachine_1",   P::anim::kKerfurStateMachine1_Start,    P::anim::kKerfurStateMachine1_End},
        {"StateMachine",     P::anim::kKerfurStateMachine_Start,     P::anim::kKerfurStateMachine_End},
        // AnimBP INSTANCE-LEVEL public variables block (post-AnimGraphNode
        // tail). Bug 2 deep dive 2026-05-23: state machine differs (idx 1 vs 2)
        // between local-walking and puppet-sliding; this region covers all
        // kerfur AnimBP vars + padding so any field difference shows up.
        {"AnimBP_vars_all",  P::anim::kKerfurAnimBPVarsAll_Start,    P::anim::kKerfurAnimBPVarsAll_End},
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
    const float spd = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_spd());
    const float walkSpeed = ReadAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_walkSpeed());
    // animWalkAlpha + animWalkRate: kept in the dump as observable AnimBP state.
    // The Plan A hypothesis that animWalkAlpha gates idle-vs-walk was DISPROVED
    // by the 2026-05-23 spawn diagnostic -- the LOCAL has animWalkAlpha=0.00
    // while WALKING. spd is the actual locomotion driver (BlendSpace X input),
    // which Plan B1's BUA interceptor pushes from the network speed.
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

// Audit H9 (2026-05-27): GetSpawnMeshOffsetZ kept as a stub returning 0
// because all callers reference it. The legacy SkelMesh code path -- which
// was the only branch that returned non-zero -- is gone. `localPlayer` is
// unused now but kept in the signature for ABI stability across the
// remote_player ↔ puppet boundary.
float GetSpawnMeshOffsetZ(void* /*localPlayer*/) {
    return 0.f;
}

// New path: spawn mainPlayer_C orphan. The class's built-in mesh_playerVisible
// carries the player body skin + IK leg bones + the AnimBP all pre-wired by
// class defaults. We neuter the per-screen systems (PostProcess components
// affect the local view; FP arms are camera-space attached) and flip a few
// AnimBP flags. 2026-05-25 per
// research/findings/votv-puppet-mainplayer-body-RE-2026-05-25.md.
//
// 2026-05-25 v2 hands-on fix: user reported "remote player has no visible
// model applied to him". Diagnosis: VOTV's mainPlayer_C class default may
// not carry a SkeletalMesh asset on `mesh_playerVisible` -- the skin is
// applied at runtime by save-load / equipment BP graphs that we suppressed
// (GameMode nulled + actor tick disabled). Fix: explicitly copy the LOCAL
// player's CURRENT mesh + AnimClass onto the orphan's mesh_playerVisible,
// like the SkelMesh backup path does. Caller passes `skeletalMeshAsset` +
// `animClass` from Pup::GetMeshPlayerVisibleAsset / GetMeshPlayerVisible
// AnimClass on the live local player.
static void* SpawnPuppetMainPlayer(const FVector& loc,
                                   void* skeletalMeshAsset,
                                   void* animClass) {
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
    // 2026-05-25 audit fix (post-ship IMPORTANT-5): IsLive guard before
    // dereferencing the gamemode pointer. FindObjectByClass scans by
    // class name without a liveness flag check, so a level-reload edge
    // could surface a PendingKill gamemode -- reading the mainPlayer
    // slot off freed memory.
    void* gmMainPlayerBefore = nullptr;
    if (gamemode && R::IsLive(gamemode)) {
        gmMainPlayerBefore = ReadPtr(gamemode, P::off::mainGamemode_mainPlayer);
    } else {
        UE_LOGW("puppet[MainPlayer]: no live gamemode (ptr=%p IsLive=%d) -- cannot capture mainPlayer pointer for restore",
                gamemode, gamemode ? (int)R::IsLive(gamemode) : 0);
        gamemode = nullptr;  // unify the post-spawn check
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
    // 2026-05-27 v2 anim drive ALSO depends on the disabled tick: we
    // own the CMC.Velocity + MovementMode fields per-tick (the local
    // BUA-mirror path), so the CMC must not overwrite them.
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        E::SetComponentTickEnabled(cmc, false);
        UE_LOGI("puppet[MainPlayer]: disabled orphan CMC tick @ %p (puppet driven by SetActorLocation, not physics)", cmc);
    }
    // 2026-05-25 audit fix (post-ship CRITICAL-2): disable the orphan's
    // actor-level tick. mainPlayer_C ReceiveTick runs SP logic each
    // frame (HUD updates, lookAt traces, hunger/thirst countdowns,
    // wind-sound positioning) we DO NOT want on a puppet. Even with
    // GameMode nulled, ReceiveTick branches reading other fields can
    // still execute. Disabling the actor tick stops the BP graph
    // entirely. The mesh AnimBP keeps ticking (we explicitly
    // SetAnimTickAlways on mesh_playerVisible below); only the
    // actor-level BP EventTick is suppressed.
    E::SetActorTickEnabled(actor, false);
    UE_LOGI("puppet[MainPlayer]: disabled orphan actor tick (ReceiveTick BP graph suppressed; AnimBP still ticks on mesh)");

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
    // 2026-05-25 v3 hands-on root-cause fix (user: "remote player has
    // collision, remote player has no visible model"):
    //
    // The prior version called SetComponentVisible(comp, false) with
    // bPropagateToChildren=true (the historical default) on `arms`,
    // `playermodel`, AND `ACharacter::Mesh`. If mesh_playerVisible is
    // attached as a child of ANY of those (VOTV's BP can attach the
    // authoritative body to the native ACharacter::Mesh slot rather than
    // to the capsule root), the propagating-hide CASCADES to the body
    // -> puppet renders invisible.
    //
    // Fix: pass propagate=false. We only want to hide the SPECIFIC
    // component, not its scene-graph children. The mesh_playerVisible
    // SetComponentVisible(true) call later (which is fine with
    // propagate=true -- mesh_playerVisible's children are its skeletal
    // sockets, not other meshes) reverses any prior unintended hide.
    //
    // Companion diagnostic below dumps each component's AttachParent +
    // bHiddenInGame state at end of spawn so we can confirm the
    // hierarchy and detect regressions.
    if (void* arms = ReadPtr(actor, P::off::AmainPlayer_arms)) {
        E::SetComponentVisible(arms, /*visible=*/false, /*propagate=*/false);
        UE_LOGI("puppet[MainPlayer]: hid FP arms @ %p (no propagate)", arms);
    }
    if (void* playermodel = ReadPtr(actor, P::off::AmainPlayer_playermodel)) {
        E::SetComponentVisible(playermodel, /*visible=*/false, /*propagate=*/false);
        UE_LOGI("puppet[MainPlayer]: hid playermodel @ %p (no propagate)", playermodel);
    }

    // Phase 5F (flashlight): NO forced visibility writes on lag_fl /
    // light_R at spawn. Earlier attempts unhid them via SetComponentVisible
    // -- user reported a "huge ERROR model" appearing on the puppet
    // (likely a SpringArm debug arrow or a Light BillboardComponent
    // sprite that has no asset in shipping builds and renders as UE4's
    // pink/orange error placeholder). RULE 1: identify the real cone-
    // visibility mechanism rather than crutch around it with unhide
    // hacks. Subsequent ApplyToPuppet calls drive light_R.Intensity via
    // the SetIntensity UFunction, which internally MarkRenderStateDirty's
    // the proxy and (per Agent 1's RE) is the actual mechanism VOTV's BP
    // uses for the flashlight toggle. If the cone STILL doesn't appear,
    // the next iteration should examine bAffectsWorld / IntensityUnits
    // -- NOT force-show visualization components.
    // 2026-05-25 v5 ROOT-CAUSE: do NOT hide ACharacter::Mesh.
    // Diagnostic logs from 95d7d90 proved (after FProperty dump confirmed
    // SetVisibility param name is correct + direct bVisible bit write
    // succeeded yet puppet stayed invisible):
    //   mesh_playerVisible.AttachParent = ACharacter::Mesh
    //   UE4 USceneComponent::IsVisible() cascades through AttachParent.
    //   Parent.bHiddenInGame=1 -> child effectively invisible regardless
    //   of child's own bVisible/bHiddenInGame.
    // My prior hide of ACharacter::Mesh was THE root cause of the
    // "no visible model" symptom. Removing the hide lets the parent
    // chain stay visible (matches local player behavior -- both meshes
    // carry the same skin asset and overlap perfectly when rendered).
    // The native ACharacter::Mesh slot keeps its class-default
    // visibility; mesh_playerVisible inherits a visible parent and
    // renders.
    //
    // NOT hidden: ACharacter::Mesh @0x0280. (The local player has it
    // visible too; both peers see overlapping bodies as ONE body --
    // identical skin asset, both ticking the same AnimBP class.)
    // 2026-05-25 v2 hands-on fix: copy the local player's skin onto the
    // orphan's mesh_playerVisible. VOTV's class default may not carry a
    // SkeletalMesh asset here; the local player gets it via save-load BP
    // (which we suppressed by nulling GameMode + disabling actor tick).
    // SetSkeletalMesh also re-initializes the pose buffer. Without this
    // the puppet renders blank.
    if (skeletalMeshAsset) {
        E::SetSkeletalMesh(meshComp, skeletalMeshAsset);
        UE_LOGI("puppet[MainPlayer]: copied local skin asset %p onto mesh_playerVisible",
                skeletalMeshAsset);
    } else {
        UE_LOGW("puppet[MainPlayer]: no skin asset provided -- puppet will render whatever the class default carried (often blank)");
    }
    // SetAnimClass instantiates the AnimInstance (flips AnimationMode to
    // UseAnimBlueprint). The class default likely already has it set,
    // but on an inert orphan with suppressed BP paths the BUA cache may
    // be stale; re-applying triggers a fresh BlueprintBeginPlay + caches
    // Pawn/Movement from TryGetPawnOwner (= orphan, valid Pawn) -- which
    // is exactly what v2 anim drive expects. CMC.Velocity + MovementMode
    // are then driven from RemotePlayer::ApplyToEngine per tick.
    if (animClass) {
        E::SetAnimClass(meshComp, animClass);
        UE_LOGI("puppet[MainPlayer]: applied local AnimClass %p onto mesh_playerVisible",
                animClass);
    } else {
        UE_LOGW("puppet[MainPlayer]: no AnimClass provided -- mesh may render in reference pose");
    }

    // mesh_playerVisible: force always-tick + visible. Class default may
    // have VisibilityBasedAnimTick=OnlyTickPoseWhenRendered which would
    // collapse the puppet to a stick when not on screen.
    E::SetAnimTickAlways(meshComp);
    E::SetComponentVisible(meshComp, true);

    // AnimBP setup: removeArms ON (avoid grab-pose arm flail);
    // walkSpeedMultiplier seeded to 1.0. useLegIK is NOT written here -- BUA
    // writes it natively each tick from Movement.MovementMode (the kerfur
    // AnimBP gates foot-IK alpha off when MovementMode == MOVE_Falling, same
    // path the LOCAL player uses). The puppet's MovementMode is mirrored
    // from the source's airborne state via RemotePlayer::ApplyToEngine's
    // direct write to puppet.CMC.MovementMode @+0x168 each tick.
    // lookingAtPlayer: seeded false here (one-shot). The per-tick force-false was
    // RETIRED with the old ModifyBone head drive (2026-06-11) -- the head is now
    // driven by the native lookAt/customLookAt path (DriveHeadLookAtWorld), which
    // makes lookingAtPlayer irrelevant to the head AIM. The BP graph re-toggles
    // this flag each frame; it now only selects an idle/look STATE variant, not
    // the head direction. Harmless (RE: votv-puppet-head-look-RE-2026-06-11.md).
    // Note: SetAnimClass above instantiated a fresh AnimInstance, so
    // LiveAnimInstance is the NEW one (any pointer captured before
    // SetAnimClass is stale).
    // Seed BOTH kel anim instances (2026-06-11 round 3): the puppet renders TWO
    // overlapped bodies -- mesh_playerVisible AND its AttachParent, the native
    // ACharacter::Mesh slot, each with its OWN AnimInstance of the same kerfur
    // class (the v5 comment above: "both ... as ONE body"). The LOOK drive must
    // hit both (the round-2/3 head bug); the seeds are ROLE-AWARE -- the two
    // bodies are intentionally asymmetric (see removeArms below).
    //
    // Audit round-3 items 4+5: make both instances live + of `animClass` BY
    // CONSTRUCTION, not empirically -- mirror the SetAnimClass treatment onto
    // the Mesh slot (its instance previously existed at class-default mercy;
    // a null instance at seed time would silently skip removeArms forever --
    // there is no per-tick re-seed path), and gate the seeds on the IN-HAND
    // exact class pointer (no FindClass name lookup -> no resolution-order
    // dependence; a foreign/failed instance is skipped because the kerfur
    // reflected offsets must never write foreign state).
    void* meshSlotComp = ReadPtr(actor, P::off::ACharacter_Mesh);
    if (animClass && meshSlotComp && R::IsLive(meshSlotComp)) {
        E::SetAnimClass(meshSlotComp, animClass);
    }
    // FOOTSTEP-TRACE / capsule-height root fix (sounds RE 2026-06-11 par.1):
    // the Mesh slot's RelativeLocation.Z rests at the class default (-85,
    // capsule half-height) because the puppet's suppressed BP tick never runs
    // the settle write the LOCAL player gets every frame (mainPlayer uber
    // @60155-60536: VInterpTo(Mesh.RelLoc -> (lean,0,0))). The chain measure
    // in RemotePlayer::Spawn then compensates by LIFTING THE ACTOR +85
    // (meshOffsetZ_=85), which (a) floats the collision capsule 85 cm above
    // the floor and (b) starves lib_C::step's ground trace (ActorLoc down to
    // ActorLoc-(halfH-1) bottoms ~66 cm short of the floor) -> footsteps
    // SILENT even though our stride dispatch fires. Replicate the settled
    // write ONCE here: Z := 0 (X=lean is 0 at spawn; Y untouched). The chain
    // measure then yields meshOffsetZ_=0 -> actor at true capsule height,
    // identical visuals BY CONSTRUCTION, step's trace reaches the floor.
    // K2_SetRelativeLocation = the canonical path (synchronous
    // UpdateComponentToWorld, so the chain measure right after reads the
    // settled mesh world-Z).
    if (meshSlotComp && R::IsLive(meshSlotComp)) {
        const FVector relLoc = ReadAt<FVector>(
            meshSlotComp, P::off::USceneComponent_RelativeLocation);
        static void* sSetRelLocFn = nullptr;
        if (!sSetRelLocFn) {
            if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
                sSetRelLocFn = R::FindFunction(sc, L"K2_SetRelativeLocation");
            }
        }
        if (sSetRelLocFn) {
            ue_wrap::ParamFrame f(sSetRelLocFn);
            f.Set<FVector>(L"NewLocation", FVector{relLoc.X, relLoc.Y, 0.f});
            f.Set<bool>(L"bSweep", false);
            f.Set<bool>(L"bTeleport", true);
            ue_wrap::Call(meshSlotComp, f);
            UE_LOGI("puppet[MainPlayer]: settled Mesh.RelLoc.Z %.1f -> 0 "
                    "(suppressed-tick VInterpTo replica; footstep trace + capsule at true height)",
                    relLoc.Z);
        } else {
            UE_LOGW("puppet[MainPlayer]: K2_SetRelativeLocation unresolved -- Mesh slot "
                    "unsettled (footsteps stay silent; actor will ride +%.0f)", -relLoc.Z);
        }
    }
    for (void* seedComp : {meshComp, meshSlotComp}) {
        if (!seedComp || !R::IsLive(seedComp)) continue;
        void* anim = LiveAnimInstance(seedComp);
        if (!anim || !R::IsLive(anim) || !animClass || R::ClassOf(anim) != animClass) continue;
        // removeArms is the FP SELF-VIEW recipe, not an arms-only toggle: it
        // gates TwoWayBlend_1 into a branch whose ModifyBone nodes SCALE AWAY
        // upperarm_L/R + neck + HEAD (bp_reflect: kerfuranim_cfg.txt @3265;
        // BoneToModify set incl. head/neck; hands-on round 4 "puppet looks
        // headless"). The two bodies are deliberately ASYMMETRIC: the Mesh
        // slot is the puppet's head+arms PROVIDER (must stay full-body,
        // removeArms=false); mesh_playerVisible is the de-headed underlay
        // (removeArms=true, its SP role).
        WriteAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_removeArms(),
                      seedComp == meshComp);
        WriteAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer(), false);
        WriteAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_walkSpeedMultiplier(), 1.f);
    }
    UE_LOGI("puppet[MainPlayer]: spawned actor=%p mesh_playerVisible=%p at (%.0f,%.0f,%.0f)",
            actor, meshComp, loc.X, loc.Y, loc.Z);
    DumpAnimState(L"puppet", meshComp);
    // One-shot: prove the Mesh-slot's instance class + state too (the head-look
    // drive writes BOTH; this dump is the smoke evidence for the second one).
    if (meshSlotComp && R::IsLive(meshSlotComp)) DumpAnimState(L"puppet-MeshSlot", meshSlotComp);

    // 2026-05-25 v5 diagnostic: kept for verification of THIS commit's fix.
    // Will retire once user confirms puppet visible (RULE 2 baggage). The
    // FProperty dump for SetVisibility was already captured in the v4
    // diagnostic logs (param IS 'bNewVisibility' as we used; bit-4 IS
    // bVisible; direct write IS at the right bit). Now we only need to
    // verify mesh_playerVisible.AttachParent stays VISIBLE so the
    // IsVisible() cascade returns true.
    auto dumpMeshComp = [](const wchar_t* label, void* comp) {
        if (!comp) {
            UE_LOGI("puppet-state[%ls]: <null>", label);
            return;
        }
        void* attachParent = ReadPtr(comp, P::off::USceneComponent_AttachParent);
        const uint8_t visByte = ReadAt<uint8_t>(comp, P::off::USceneComponent_VisFlagsByte);
        const uint8_t hiddenByte = ReadAt<uint8_t>(comp, P::off::USceneComponent_HiddenFlagsByte);
        const bool bVisible = (visByte & (1u << 4)) != 0;
        const bool bHiddenInGame = (hiddenByte & (1u << 2)) != 0;
        void* skinAsset = ReadPtr(comp, P::off::USkinnedMesh_SkeletalMesh);
        std::wstring parentClass = attachParent ? R::ClassNameOf(attachParent) : L"<null>";
        UE_LOGI("puppet-state[%ls]: comp=%p AttachParent=%p(%ls) visByte=0x%02x bVisible=%d bHiddenInGame=%d SkelMesh=%p",
                label, comp,
                attachParent, parentClass.c_str(),
                (unsigned)visByte, (int)bVisible, (int)bHiddenInGame, skinAsset);
    };
    dumpMeshComp(L"mesh_playerVisible", meshComp);
    dumpMeshComp(L"arms",               ReadPtr(actor, P::off::AmainPlayer_arms));
    dumpMeshComp(L"playermodel",        ReadPtr(actor, P::off::AmainPlayer_playermodel));
    dumpMeshComp(L"ACharacter::Mesh",   ReadPtr(actor, P::off::ACharacter_Mesh));

    return actor;
}

void* SpawnPuppet(const FVector& loc, void* skeletalMeshAsset, void* animClass) {
    // Audit H9 (2026-05-27): MainPlayer is the only puppet kind (RULE 2
    // retired the SkelMesh backup + the VOTVCOOP_PUPPET_KIND env var). The
    // mainPlayer_C path is hands-on-verified working per commit b100e8e.
    return SpawnPuppetMainPlayer(loc, skeletalMeshAsset, animClass);
}

void DriveHeadLookAtWorld(void* puppetActor, const FVector& worldTarget) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    // The actor slot can still pass IsLive for a tick while its child component is
    // already being torn down (UE finalizes sub-objects first). Re-check the
    // component before reading AnimScriptInstance @ +0x6B0.
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;

    // Drive the head via the kerfur NATIVE lookAt pipeline (RE 2026-06-11,
    // votv-puppet-head-look-RE-2026-06-11.md). The visible head/neck twist comes
    // from two FAnimNode_LookAt nodes (head Alpha 1.0 / neck Alpha 0.5, 45-deg
    // clamp each) that aim at the AnimBP `lookAt` FVector; a native PropertyAccess
    // FastPath copy carries `lookAt` -> LookAtLocation each tick. So writing
    // `lookAt` (+ `customLookAt=true`) via WriteLookAtOnAnim makes OUR world target
    // win: it is the nodes' native input, and customLookAt stops BUA re-aiming
    // `lookAt` at the LOCAL PlayerCameraManager (= the observer -- the old "puppet
    // head follows the host" bug).
    //
    // The retired recipe (zero the LookAt Alphas + write ModifyBone @0x2C60
    // .Rotation + RotationMode=2 + lookingAtPlayer=false + headLookAt) FOUGHT THE
    // WRONG NODE: that ModifyBone .Rotation was clobbered every tick by FastPath
    // copy [5] (headLookAt -> .Rotation) and was in the wrong mode (2 = ADDITIVE,
    // not Replace) and world space. Deleted (RULE 2). This is the SAME write
    // DriveKerfurLookAt uses for the NPC mirror (WriteLookAtOnAnim, kerfur-gated),
    // on the PUPPET's OWN instances -- so NPC head-follow is untouched.
    WriteLookAtOnAnim(anim, worldTarget);

    // The puppet renders TWO overlapped kel bodies (hands-on root cause
    // 2026-06-11 round 3): mainPlayer_C shows mesh_playerVisible @0x04F8
    // ATTACHED TO the native ACharacter::Mesh slot @0x0280 -- same skin asset,
    // EACH ticking its OWN kerfur AnimInstance (see the v5 spawn comment "both
    // ... as ONE body"; the hurt-flash already swaps materials on BOTH meshes).
    // In SP the two stay identical because both BUAs auto-aim lookAt at the
    // same local camera; driving only ONE instance breaks that invariant and
    // the OTHER (un-driven) head keeps auto-following the observer = the
    // "auto head follow still ticking" report. Drive BOTH instances with the
    // same target (class-gated per instance; the dedupe guard covers a future
    // single-mesh refactor).
    void* meshSlot = ReadPtr(puppetActor, P::off::ACharacter_Mesh);
    if (meshSlot && meshSlot != comp && R::IsLive(meshSlot)) {
        if (void* slotAnim = LiveAnimInstance(meshSlot)) {
            if (slotAnim != anim) WriteLookAtOnAnim(slotAnim, worldTarget);
        }
    }
}

bool ReadKerfurLookAt(void* npcActor, FVector& outWorldTarget) {
    return ReadLookAtOnAnim(NpcBodyAnimInstance(npcActor), outWorldTarget);
}

void DriveKerfurLookAt(void* npcActor, const FVector& worldTarget) {
    WriteLookAtOnAnim(NpcBodyAnimInstance(npcActor), worldTarget);
}

bool ReadKerfurBodyYaw(void* npcActor, float& outYaw) {
    // The kerfur actor BP aims the VISIBLE body by rotating ACharacter::Mesh's WORLD rotation
    // (decoupled from the actor root) -- per-peer toward the local player. Read the resolved mesh
    // world yaw on the host so the mirror can reproduce it. Class-gated (kerfur-family only).
    void* mesh = NpcBodyMesh(npcActor);
    if (!mesh || !IsKerfurAnimBP(LiveAnimInstance(mesh))) return false;
    outYaw = ue_wrap::engine::GetComponentWorldRotation(mesh).Yaw;
    return true;
}

void DriveKerfurBodyYaw(void* npcActor, float yaw) {
    // Drive a mirror kerfur's body facing: set ACharacter::Mesh WORLD rotation to the streamed
    // yaw. The mirror's actor tick is OFF (DisableCharacterTicks), so the BP's per-tick mesh
    // rotation never runs there -> no clobber, no gate flag needed. MUST be called AFTER
    // SetActorRotation (moving the actor root re-bases this child mesh's world transform).
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
    // UMovementComponent::Velocity @+0xC4 (FVector, Engine.hpp:15427).
    // Constant is hardcoded here rather than promoted into sdk_profile.h so
    // every "raw memory write" stays in the ue_wrap layer; coop/ callers
    // see only this typed API.
    constexpr size_t kUMovementComponent_Velocity = 0xC4;
    WriteAt<FVector>(cmc, kUMovementComponent_Velocity, worldVelocity);
    const uint8_t mm = inAir ? P::off::kMOVE_Falling : uint8_t{1};  // MOVE_Walking
    WriteAt<uint8_t>(cmc, P::off::UCharacterMovement_MovementMode, mm);
}

void DriveSprintWalkSpeed(void* puppetActor, bool sprinting) {
    if (!puppetActor || !R::IsLive(puppetActor)) return;
    void* cmc = ReadPtr(puppetActor, P::off::ACharacter_CharacterMovement);
    if (!cmc || !R::IsLive(cmc)) return;
    // MaxWalkSpeed @+0x18C (float, Engine.hpp): run-LOUDNESS parity (sounds RE
    // 2026-06-11 par.4). lib_C::step's footstep volume =
    // clamp(CMC.MaxWalkSpeed/400, 0.5, 2.0) -- it reads the SETTING, not
    // Velocity. The parked puppet CMC never runs mainPlayer's updateSpeed, so
    // without this write a sprinting remote sounds walk-quiet. Mirror the
    // native sprint knob: class-default while walking, x2 while sprinting
    // (updateSpeed @676 defSpeed*2; the <=25% agility lerp deliberately
    // skipped). PLAYER PUPPETS ONLY -- a separate function (not a
    // DriveCharacterMovement param) because npc_pose_drive shares that drive
    // and an NPC's MaxWalkSpeed must not be captured as, or overwritten with,
    // the mainPlayer class default. The default is latched from the first
    // PLAYER puppet CMC (class default -- updateSpeed never ran on a puppet).
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
    // CMC tick OFF: stop gravity + Velocity integration so the network SetActorLocation drive
    // is authoritative (we own CMC.Velocity/MovementMode -- DriveCharacterMovement writes them).
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        if (R::IsLive(cmc)) E::SetComponentTickEnabled(cmc, false);
    }
    // Actor tick OFF: suppress the BP ReceiveTick graph (for an NPC mirror that is its AI state
    // machine). The AnimBP still ticks on the mesh, so it reads our per-tick CMC.Velocity write.
    E::SetActorTickEnabled(actor, false);
}

}  // namespace ue_wrap::puppet

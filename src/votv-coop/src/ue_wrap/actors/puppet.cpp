#include "ue_wrap/actors/puppet.h"
#include "puppet_internal.h"  // shared file-privates (offset templates + g_meshComp + LiveAnimInstance)

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
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

// puppet actor -> its cached SkeletalMeshComponent (avoid a GUObjectArray walk
// per Drive() frame).
std::unordered_map<void*, void*> g_meshComp;

// The live AnimInstance running on a SkeletalMeshComponent (comp + AnimScriptInstance).
void* LiveAnimInstance(void* skeletalMeshComponent) {
    return ReadPtr(skeletalMeshComponent, P::off::USkeletalMesh_AnimScriptInstance);
}

namespace {

// Plan B1 (BUA interceptor) and Plan B2 (satellite ACharacter feeding the
// AnimBP's Pawn pointer) are both retired. v2 (2026-05-27, see
// research/findings/player-puppet/votv-local-anim-drive-RE-2026-05-27.md) writes
// Velocity + MovementMode directly on the puppet's OWN CMC each tick
// (CMC tick is parked, so we own those fields). BUA reads them naturally
// via Pawn=orphan -> CMC=orphan.CMC, exactly like the LOCAL player's
// possessed CMC -- same spd / IK gate behaviour with zero AnimInstance
// pointer-redirect plumbing.

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

// The head-look STATE-GATE defeat hook (HeadGateBUAPost + InstallHeadGateHook) moved to
// puppet_spawn.cpp 2026-07-19 (s28 modular cut; its sole install site is the spawn path).
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

// The puppet SPAWN path (GetSpawnMeshOffsetZ + the static SpawnPuppetMainPlayer orphan
// spawn/neuter/rig procedure + the public SpawnPuppet wrapper) was EXTRACTED to
// puppet_spawn.cpp 2026-07-19 (s28 modular cut; declarations stay in puppet.h). The offset
// templates it shares with this TU moved to puppet_internal.h; g_meshComp + LiveAnimInstance
// are DEFINED here (named scope above) and extern-declared there.

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

bool ReadPuppetHeadLookProbe(void* puppetActor, PuppetHeadLookProbe& out) {
    out = {};
    void* comp = GetSkeletalMeshComponent(puppetActor);
    if (!comp || !R::IsLive(comp)) return false;
    // LookAtClamp (degrees) read off the puppet's OWN kerfur AnimInstance -- the two
    // FAnimNode_LookAt nodes (head @kKerfurLookAt_1, neck @kKerfurLookAt), clamp @+0x170.
    void* anim = LiveAnimInstance(comp);
    if (anim && IsKerfurAnimBP(anim)) {
        out.headClampDeg = ReadAt<float>(anim, P::anim::kKerfurLookAt_1 + P::anim::LookAt_Clamp);
        out.neckClampDeg = ReadAt<float>(anim, P::anim::kKerfurLookAt   + P::anim::LookAt_Clamp);
        out.haveClamp = true;
        // Gate diagnostics: the LookAt node alphas (does the look get blended OUT when the
        // head freezes?) + lookingAtPlayer (the dot-product state gate) + customLookAt (is
        // our drive still pinned, or did BUA reclaim lookAt?).
        out.headAlpha = ReadAt<float>(anim, P::anim::kKerfurLookAt_1 + P::anim::SkelCtl_Alpha);
        out.neckAlpha = ReadAt<float>(anim, P::anim::kKerfurLookAt   + P::anim::SkelCtl_Alpha);
        out.lookingAtPlayer = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer());
        out.customLookAt    = ReadAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_customLookAt());
        out.haveGates = true;
    }
    // Resolved WORLD rotation of the 'head' + 'neck' bones (the actual rendered twist).
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
    DisableMovementTick(actor);
    // Actor tick OFF: suppress the BP ReceiveTick graph (for an NPC mirror that is its AI state
    // machine). The AnimBP still ticks on the mesh, so it reads our per-tick CMC.Velocity write.
    E::SetActorTickEnabled(actor, false);
}

void DisableMovementTick(void* actor) {
    if (!actor || !R::IsLive(actor)) return;
    // CMC tick OFF: stop gravity + Velocity integration so the network SetActorLocation drive
    // is authoritative (we own CMC.Velocity/MovementMode -- DriveCharacterMovement writes them).
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        if (R::IsLive(cmc)) E::SetComponentTickEnabled(cmc, false);
    }
}

}  // namespace ue_wrap::puppet

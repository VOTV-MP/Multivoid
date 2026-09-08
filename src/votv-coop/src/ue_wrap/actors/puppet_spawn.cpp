// ue_wrap/actors/puppet_spawn.cpp -- the puppet spawn path: the inert mainPlayer_C spawn, its
// neutering and rig, the public SpawnPuppet wrapper, and the head-look state-gate hook whose only
// install site is this path. The shared privates (the raw read and write templates, the
// mesh-component cache, LiveAnimInstance) come from puppet_internal.h. Game thread only.

#include "ue_wrap/actors/puppet.h"
#include "puppet_internal.h"  // ReadPtr/ReadAt/WriteAt + g_meshComp + LiveAnimInstance

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <cstdint>
#include <string>

namespace ue_wrap::puppet {

namespace P = profile;
namespace R = reflection;
namespace E = engine;

namespace {

// The head-look state gate, puppets only. The two LookAt nodes are the whole sub-graph of state
// lookAtPlayer in the AnimBP's trunk state machine, and lookingAtPlayer (recomputed every anim
// update from the angle to the observer's camera, not the look target) is the transition rule:
// a puppet turned away from the observing player exits the state, the LookAt contribution
// crossfades to zero and the head snaps to neutral. A tick write cannot hold it, since the BP
// recomputes it after any write and before the state machine reads it; the seam is a Func hook
// on the AnimBP's own BlueprintUpdateAnimation, re-asserting the flag after the recompute. The
// hook fires for every instance of the shared AnimBP and writes only when the instance's outer
// chain is a mainPlayer_C with a null Controller, the puppet discriminator; kerfur NPCs and the
// local player are untouched.
void HeadGateBUAPost(void* /*context*/, void* animInstance, void* /*result*/) {
    if (!animInstance) return;
    void* comp = R::OuterOf(animInstance);   // UAnimInstance's outer = its USkeletalMeshComponent
    if (!comp) return;
    void* actor = R::OuterOf(comp);          // component's outer = the owning actor
    if (!actor) return;
    static void* sMainPlayerClass = nullptr;
    if (!sMainPlayerClass) sMainPlayerClass = R::FindClass(P::name::MainPlayerClass);
    if (!sMainPlayerClass || R::ClassOf(actor) != sMainPlayerClass) return;  // a kerfur NPC exits here
    if (ReadPtr(actor, P::off::APawn_Controller)) return;                    // the local player exits here
    const int32_t off = ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer();
    if (off < 0) return;
    WriteAt<bool>(animInstance, off, true);
}

void InstallHeadGateHook(void* animClass) {
    static bool s_tried = false;   // one shot: the same class every spawn
    if (s_tried || !animClass) return;
    s_tried = true;
    void* fn = R::FindFunction(animClass, L"BlueprintUpdateAnimation");
    if (!fn) {
        UE_LOGW("puppet: BlueprintUpdateAnimation not found on the AnimBP class -- head-look "
                "state gate stays native (head will freeze when back-turned)");
        return;
    }
    // The BP's own override, not the native UAnimInstance declaration: patching a super's Func
    // would hook every AnimInstance in the game.
    if (R::OuterOf(fn) != animClass) {
        UE_LOGW("puppet: BlueprintUpdateAnimation resolved on a SUPER class (fn=%p) -- refusing "
                "the head-gate hook (would fire for every AnimInstance)", fn);
        return;
    }
    const bool ok = ue_wrap::ufunction_hook::InstallPostHook(fn, &HeadGateBUAPost);
    if (ok) {
        UE_LOGI("puppet: head-look state-gate hook installed -- post-BUA lookingAtPlayer=true on "
                "PUPPET instances only (the LookAt nodes live INSIDE state 'lookAtPlayer'; without "
                "this the AnimBP exits that state when the puppet faces away from the observer)");
    } else {
        // An ERROR line: a hook-table-full miss inside an INFO line once shipped a fix that worked
        // on one peer only.
        UE_LOGE("puppet: head-look state-gate hook FAILED to install -- puppet heads will snap "
                "to neutral when back-turned on THIS peer (see ufunction_hook error above)");
    }
}

}  // namespace

// The mainPlayer_C orphan spawn: the class's mesh_playerVisible carries the body, the IK bones
// and the AnimBP by class default; the per-screen systems are neutered and a few AnimBP flags
// flipped. The class default may carry no SkeletalMesh (the skin is applied at runtime by
// save-load and equipment graphs, which are suppressed here), so the caller passes the local
// player's current mesh asset and AnimClass.
static void* SpawnPuppetMainPlayer(const FVector& loc,
                                   void* skeletalMeshAsset,
                                   void* animClass) {
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) {
        UE_LOGE("puppet[MainPlayer]: mainPlayer_C class not found");
        return nullptr;
    }
    // The gamemode's mainPlayer reference is captured before the spawn: the orphan's BeginPlay
    // writes gamemode.mainPlayer = self, and the captured pointer is restored after, so the
    // gamemode's save, sleep and damage paths keep operating on the real local player.
    void* gamemode = R::FindObjectByClass(P::name::GamemodeClass);
    // Live-guarded: FindObjectByClass scans by class name without a liveness check, and a level
    // reload could surface a PendingKill gamemode.
    void* gmMainPlayerBefore = nullptr;
    if (gamemode && R::IsLive(gamemode)) {
        gmMainPlayerBefore = ReadPtr(gamemode, P::off::mainGamemode_mainPlayer);
    } else {
        UE_LOGW("puppet[MainPlayer]: no live gamemode (ptr=%p IsLive=%d) -- cannot capture mainPlayer pointer for restore",
                gamemode, gamemode ? (int)R::IsLive(gamemode) : 0);
        gamemode = nullptr;  // unify the post-spawn check
    }
    // inertPawn zeros AutoPossessPlayer, AutoPossessAI and AutoReceiveInput and sets bBlockInput in
    // the deferred-spawn window before BeginPlay, so no second PlayerController auto-possesses and
    // no local input is hijacked.
    void* actor = E::SpawnActor(cls, loc, /*inertPawn=*/true);
    if (!actor) {
        UE_LOGE("puppet[MainPlayer]: SpawnActor(mainPlayer_C) failed");
        return nullptr;
    }
    // gamemode.mainPlayer restored if the orphan's BeginPlay overwrote it: the autosave timer would
    // otherwise serialise the orphan's position as the save's player transform.
    if (gamemode) {
        void* gmMainPlayerAfter = ReadPtr(gamemode, P::off::mainGamemode_mainPlayer);
        if (gmMainPlayerAfter != gmMainPlayerBefore) {
            E::WriteObjectField(gamemode, P::off::mainGamemode_mainPlayer, gmMainPlayerBefore);
            UE_LOGI("puppet[MainPlayer]: gamemode.mainPlayer was overwritten by orphan (%p -> %p); restored to %p",
                    gmMainPlayerBefore, gmMainPlayerAfter, gmMainPlayerBefore);
        }
    }
    // The orphan's cached GameMode pointer nulled, so its ReceiveTick graphs (which null-check it
    // and return) neither re-overwrite gamemode.mainPlayer nor invoke gamemode methods.
    E::WriteObjectField(actor, P::off::AmainPlayer_GameMode, nullptr);
    UE_LOGI("puppet[MainPlayer]: nulled orphan.GameMode @0x0C80 (disconnected from gamemode)");

    // mesh_playerVisible by direct offset: mainPlayer_C has several SkeletalMeshComponents
    // (ACharacter::Mesh, mesh_playerVisible, the arms, playermodel), and a child search returns
    // whichever loads first.
    void* meshComp = ReadPtr(actor, P::off::AmainPlayer_mesh_playerVisible);
    if (!meshComp) {
        UE_LOGE("puppet[MainPlayer]: actor %p has no mesh_playerVisible @0x04F8", actor);
        return actor;
    }
    // Cached for GetSkeletalMeshComponent.
    g_meshComp[actor].Set(meshComp);  // fresh from the spawn path

    // The orphan's CharacterMovementComponent tick disabled: its gravity and walking integration
    // would fight the wire-driven SetActorLocation writes, and the anim drive owns CMC.Velocity and
    // MovementMode per tick.
    if (void* cmc = ReadPtr(actor, P::off::ACharacter_CharacterMovement)) {
        E::SetComponentTickEnabled(cmc, false);
        UE_LOGI("puppet[MainPlayer]: disabled orphan CMC tick @ %p (puppet driven by SetActorLocation, not physics)", cmc);
    }
    // The orphan's actor tick disabled: mainPlayer_C's ReceiveTick runs single-player logic each
    // frame (HUD updates, look traces, hunger and thirst, wind-sound placement), and branches
    // reading other fields still run with GameMode nulled. The mesh AnimBP keeps ticking
    // (SetAnimTickAlways below).
    E::SetActorTickEnabled(actor, false);
    UE_LOGI("puppet[MainPlayer]: disabled orphan actor tick (ReceiveTick BP graph suppressed; AnimBP still ticks on mesh)");

    // Both PostProcessComponents destroyed: they drive the local camera's colour, exposure and
    // gamma, and alive on a puppet they corrupt whichever screen renders it.
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
    // The mic UAudioCaptureComponent destroyed: the orphan would otherwise capture the default
    // input device into a sink nobody reads.
    if (void* mic = ReadPtr(actor, P::off::AmainPlayer_mic)) {
        if (E::DestroyComponent(mic, actor)) {
            UE_LOGI("puppet[MainPlayer]: destroyed mic UAudioCaptureComponent @ %p", mic);
        }
    }
    // The arms and playermodel are hidden without propagation: a propagating hide on any component
    // mesh_playerVisible is attached under cascades to the body, and the puppet rendered invisible.
    if (void* arms = ReadPtr(actor, P::off::AmainPlayer_arms)) {
        E::SetComponentVisible(arms, /*visible=*/false, /*propagate=*/false);
        UE_LOGI("puppet[MainPlayer]: hid FP arms @ %p (no propagate)", arms);
    }
    if (void* playermodel = ReadPtr(actor, P::off::AmainPlayer_playermodel)) {
        E::SetComponentVisible(playermodel, /*visible=*/false, /*propagate=*/false);
        UE_LOGI("puppet[MainPlayer]: hid playermodel @ %p (no propagate)", playermodel);
    }

    // No forced visibility writes on the flashlight components at spawn: unhiding them showed a
    // placeholder error model on the puppet (a debug sprite with no asset in a shipping build); the
    // flashlight lane drives light_R.Intensity through SetIntensity, which marks the render state
    // dirty. ACharacter::Mesh is not hidden either: mesh_playerVisible's AttachParent is that slot,
    // and a child's visibility cascades through its parent's bHiddenInGame regardless of its own
    // flags, so hiding the slot was the root of the invisible puppet. The local player has it
    // visible too, and the two bodies overlap as one with the same skin.
    if (skeletalMeshAsset) {
        const bool setOk = E::SetSkeletalMesh(meshComp, skeletalMeshAsset);
        // Read back, to tell a setter that rejected the mesh (the field kept the class default)
        // from a field that holds the mesh and renders the default anyway.
        void* after = ReadPtr(meshComp, P::off::USkinnedMesh_SkeletalMesh);
        UE_LOGI("puppet[MainPlayer]: copied skin asset %p onto mesh_playerVisible "
                "(SetSkeletalMesh ret=%d, field-after=%p, matches=%d)",
                skeletalMeshAsset, setOk ? 1 : 0, after, (after == skeletalMeshAsset) ? 1 : 0);
        // The two-body invariant: the actor renders two overlapping bodies, mesh_playerVisible and
        // its AttachParent ACharacter::Mesh. Identical skins overlap invisibly; a custom skin on
        // mesh_playerVisible alone stays masked by the slot's default, and hiding the slot kills
        // the child too. So the skin goes into both slots; for the stock skin the second write is a
        // no-op.
        if (void* nativeSlot = ReadPtr(actor, P::off::ACharacter_Mesh);
            nativeSlot && R::IsLive(nativeSlot)) {
            E::SetSkeletalMesh(nativeSlot, skeletalMeshAsset);
        }
    } else {
        UE_LOGW("puppet[MainPlayer]: no skin asset provided -- puppet will render whatever the class default carried (often blank)");
    }
    // SetAnimClass instantiates the AnimInstance: on an inert orphan with suppressed BP paths the
    // cached one may be stale, and a fresh one caches its Pawn and Movement from the orphan, which
    // the anim drive then feeds per tick.
    if (animClass) {
        E::SetAnimClass(meshComp, animClass);
        UE_LOGI("puppet[MainPlayer]: applied local AnimClass %p onto mesh_playerVisible",
                animClass);
    } else {
        UE_LOGW("puppet[MainPlayer]: no AnimClass provided -- mesh may render in reference pose");
    }

    // mesh_playerVisible always ticks and is visible: OnlyTickPoseWhenRendered would collapse the
    // puppet to a stick off screen.
    E::SetAnimTickAlways(meshComp);
    E::SetComponentVisible(meshComp, true);

    // The AnimBP seeds: removeArms on (no grab-pose arm flail), walkSpeedMultiplier 1, and
    // lookingAtPlayer true (the two LookAt nodes live inside state lookAtPlayer and this flag gates
    // its transitions; false exits the state and the head snaps to neutral). useLegIK is not
    // written: the BP derives it each tick from MovementMode, which the anim drive mirrors from the
    // source's airborne state. The seed alone cannot hold, since the BP recomputes the flag every
    // update; the post-update hook re-asserts it. Both anim instances are seeded, since the puppet
    // renders two bodies (mesh_playerVisible and the ACharacter::Mesh slot), each with its own
    // instance of the same class, and the look drive must hit both; the Mesh slot's instance is set
    // to `animClass` by construction, and a foreign or failed instance is skipped, since the
    // reflected offsets must never write foreign state.
    void* meshSlotComp = ReadPtr(actor, P::off::ACharacter_Mesh);
    if (animClass && meshSlotComp && R::IsLive(meshSlotComp)) {
        E::SetAnimClass(meshSlotComp, animClass);
    }
    // The Mesh slot's relative Z settled to 0: it rests at the class default (-85, the capsule
    // half-height) because the puppet's suppressed tick never runs the settle write the local
    // player gets every frame, and left there the whole chain hangs 85 low, misplacing the body
    // against the wire-driven actor and starving the footstep ground trace, so footsteps stayed
    // silent. With the settle the puppet's chain equals the local player's by construction, which
    // is why the actor is driven at the wire pose with no offset. K2_SetRelativeLocation is the
    // canonical path.
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
        // removeArms is the first-person self-view recipe, not an arms-only toggle: it scales away
        // the upper arms, the neck and the head. The two bodies are deliberately asymmetric: the
        // Mesh slot provides the head and arms (removeArms false), mesh_playerVisible is the
        // de-headed underlay (removeArms true, its single-player role).
        WriteAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_removeArms(),
                      seedComp == meshComp);
        WriteAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer(), true);
        WriteAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_walkSpeedMultiplier(), 1.f);
    }
    // The head-look state stays alive across every anim update (the seed is recomputed away by the
    // next one; the hook is the fix).
    InstallHeadGateHook(animClass);
    UE_LOGI("puppet[MainPlayer]: spawned actor=%p mesh_playerVisible=%p at (%.0f,%.0f,%.0f)",
            actor, meshComp, loc.X, loc.Y, loc.Z);
    DumpAnimState(L"puppet", meshComp);
    // The Mesh slot's instance class and state too; the look drive writes both.
    if (meshSlotComp && R::IsLive(meshSlotComp)) DumpAnimState(L"puppet-MeshSlot", meshSlotComp);

    // The per-component visibility dump at spawn: the AttachParent and the visibility bits of each
    // mesh component, the evidence that the parent chain stays visible.
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
    // mainPlayer_C is the only puppet kind.
    return SpawnPuppetMainPlayer(loc, skeletalMeshAsset, animClass);
}

}  // namespace ue_wrap::puppet

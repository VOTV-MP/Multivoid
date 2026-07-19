// ue_wrap/actors/puppet_spawn.cpp -- the puppet SPAWN path: the mainPlayer_C orphan spawn +
// neuter/rig procedure, its public SpawnPuppet wrapper, and the spawn-time head-look
// state-gate hook whose only install site is this path.
//
// EXTRACTED from puppet.cpp 2026-07-19 (s28 modular cut; the TU was 972 LOC, past the 800
// soft cap). Bodies verbatim: the head-gate block (HeadGateBUAPost + InstallHeadGateHook)
// moved WHOLE with its sole caller (SpawnPuppetMainPlayer's install site) and stays anon;
// SpawnPuppetMainPlayer keeps its `static` (internal linkage). Public declarations
// (SpawnPuppet / GetSpawnMeshOffsetZ) stay in puppet.h. Shared file-privates
// (ReadPtr/ReadAt/WriteAt templates, g_meshComp cache, LiveAnimInstance) come from the
// impl-private puppet_internal.h.
//
// Game-thread only (SpawnActor / component rig-up).

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

// ---- head-look STATE-GATE defeat (2026-07-02, puppet-only) -----------------
// TOPOLOGY (statically proven from the cooked BakedStateMachines + pose-link
// trace; research/findings/player-puppet/votv-puppet-head-freeze-backturned-RE-2026-06-24.md
// top update): the two FAnimNode_LookAt nodes do NOT sit on the anim-graph
// trunk -- they are the whole sub-graph of state `lookAtPlayer` in the trunk
// state machine "New State Machine_1" (states: zombieRise / lookAtPlayer /
// lookStraight; the lookAtPlayer state root IS the LookAt chain's output node).
// `lookingAtPlayer` (BUA @515: Dot(head-dir, to-LOCAL-camera) >= -0.6 -- the
// OBSERVER's position, not the look target!) is FastPath-copied into
// TransitionResult_7/_5 = the lookAtPlayer<->lookStraight transition rules. A
// puppet turned back to the OBSERVING player therefore EXITS the look state and
// the whole LookAt contribution crossfades to zero in 0.25 s -> the head snaps
// to NEUTRAL = "head freezes when back-turned". This closes the 2026-06-25
// probe riddle (TWIST 36.9 tracking at DESIRED 38 but 0.5 at DESIRED 43 with
// node Alphas still 1.0/0.5): the STATE weight died, not the nodes -- the
// 45deg-clamp theory stays refuted.
//
// A game-thread tick write cannot fix it: BUA RECOMPUTES lookingAtPlayer every
// anim update, after any write of ours and before the FastPath copies sample
// it. The only correct seam is POST-BUA: patch the AnimBP's own
// BlueprintUpdateAnimation override (UFunction::Func, ufunction_hook) and
// re-assert lookingAtPlayer=true AFTER the BP recompute, BEFORE the copies +
// state-machine update read it (both run later inside the same UpdateAnimation).
//
// PUPPET-ONLY BY IDENTITY (user constraint: kerfur NPCs keep their native
// freeze): the hook fires for EVERY instance of the shared kerfur AnimBP; it
// writes ONLY when the instance's outer chain resolves to a mainPlayer_C actor
// with a NULL Controller -- the codebase's definitive puppet discriminator
// (CLAUDE.md). Kerfur NPCs (different actor class) and the LOCAL player
// (controller != null) are byte-untouched.
void HeadGateBUAPost(void* /*context*/, void* animInstance, void* /*result*/) {
    if (!animInstance) return;
    void* comp = R::OuterOf(animInstance);   // UAnimInstance's outer = its USkeletalMeshComponent
    if (!comp) return;
    void* actor = R::OuterOf(comp);          // component's outer = the owning actor
    if (!actor) return;
    static void* sMainPlayerClass = nullptr;
    if (!sMainPlayerClass) sMainPlayerClass = R::FindClass(P::name::MainPlayerClass);
    if (!sMainPlayerClass || R::ClassOf(actor) != sMainPlayerClass) return;  // kerfur NPCs exit here
    if (ReadPtr(actor, P::off::APawn_Controller)) return;                    // the LOCAL player exits here
    const int32_t off = ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer();
    if (off < 0) return;
    WriteAt<bool>(animInstance, static_cast<size_t>(off), true);
}

void InstallHeadGateHook(void* animClass) {
    static bool s_tried = false;   // one-shot: same class every spawn; the facility is idempotent anyway
    if (s_tried || !animClass) return;
    s_tried = true;
    void* fn = R::FindFunction(animClass, L"BlueprintUpdateAnimation");
    if (!fn) {
        UE_LOGW("puppet: BlueprintUpdateAnimation not found on the AnimBP class -- head-look "
                "state gate stays native (head will freeze when back-turned)");
        return;
    }
    // Must be the BP's OWN override (Exports[4] of the AnimBP asset), not the native
    // UAnimInstance declaration -- patching a super's Func would hook EVERY AnimInstance
    // in the game. Refuse rather than over-hook.
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
        // ERROR, not a "FAILED" word inside an INFO line: this exact miss (host's Func-hook
        // table full, 2026-07-02) shipped a fix that worked on one peer only and the INFO
        // line was overlooked. The puppet head WILL freeze back-turned on THIS peer.
        UE_LOGE("puppet: head-look state-gate hook FAILED to install -- puppet heads will snap "
                "to neutral when back-turned on THIS peer (see ufunction_hook error above)");
    }
}

}  // namespace

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
// research/findings/player-puppet/body-visible-f12-and-pose-mirroring-2026-05-22.md (the dedicated mainplayer-body finding was never filed).
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
        const bool setOk = E::SetSkeletalMesh(meshComp, skeletalMeshAsset);
        // Read the field BACK to distinguish "setter rejected our mesh -> field kept the
        // class-default kel" (H1) from "field holds our mesh but it renders kel" (H2, cook).
        void* after = ReadPtr(meshComp, P::off::USkinnedMesh_SkeletalMesh);
        UE_LOGI("puppet[MainPlayer]: copied skin asset %p onto mesh_playerVisible "
                "(SetSkeletalMesh ret=%d, field-after=%p, matches=%d)",
                skeletalMeshAsset, setOk ? 1 : 0, after, (after == skeletalMeshAsset) ? 1 : 0);
        // Two-body invariant (client-model probe take 3, hands-on-verified 2026-07-02): the actor
        // renders TWO overlapping bodies -- mesh_playerVisible AND its AttachParent
        // ACharacter::Mesh (class-default kel). Identical skins overlap invisibly (the game's own
        // shape); a CUSTOM skin on mesh_playerVisible alone stays MASKED by the slot's kel (probe
        // take 1), and hiding the slot kills the child too (take 2: UE gates a child's rendering
        // by its AttachParent's visibility regardless of the propagate flag). So preserve the
        // invariant: the given skin goes into BOTH slots. For the stock kel skin this is a no-op
        // (UE early-outs on an identical mesh).
        if (void* nativeSlot = ReadPtr(actor, P::off::ACharacter_Mesh);
            nativeSlot && R::IsLive(nativeSlot)) {
            E::SetSkeletalMesh(nativeSlot, skeletalMeshAsset);
        }
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
    // lookingAtPlayer: seeded TRUE (2026-07-02; flipped from the old false seed).
    // NOT cosmetic: the two FAnimNode_LookAt nodes live INSIDE state machine
    // state `lookAtPlayer`, and this flag's FastPath copies gate that state's
    // transitions -- false means the look state EXITS and the head snaps to
    // NEUTRAL (the "head freezes when back-turned" root; the 2026-06-11 "only a
    // STATE variant, irrelevant to the head AIM" note was WRONG -- see the
    // HeadGateBUAPost topology comment). BUA recomputes the flag every anim
    // update from the OBSERVER camera angle, so the seed alone cannot hold; the
    // post-BUA hook (InstallHeadGateHook below) re-asserts it each update.
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
    // @60155-60536: VInterpTo(Mesh.RelLoc -> (lean,0,0))). Left at -85 the
    // whole chain hangs an extra 85 low, which (a) misplaces the visible body
    // vs the wire-driven actor and (b) starves lib_C::step's ground trace
    // (ActorLoc down to ActorLoc-(halfH-1) bottoms ~66 cm short of the floor)
    // -> footsteps SILENT even though our stride dispatch fires. Replicate
    // the settled write ONCE here: Z := 0 (X=lean is 0 at spawn; Y untouched).
    // With this settle the puppet's whole chain equals the local player's BY
    // CONSTRUCTION -- which is exactly why RemotePlayer drives the actor at
    // the wire pose verbatim with NO offset (anchored-zero, 2026-07-04; the
    // old post-spawn chain measure raced mesh_playerVisible's BP composition
    // on world-fresh clients and sank the puppet by halfH when it lost).
    // K2_SetRelativeLocation = the canonical path (synchronous
    // UpdateComponentToWorld).
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
        WriteAt<bool>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_lookingAtPlayer(), true);
        WriteAt<float>(anim, ue_wrap::reflected_offset::AnimBP_kerfur_walkSpeedMultiplier(), 1.f);
    }
    // Keep the puppet's head-look STATE alive across every future anim update
    // (the seed above is recomputed away by the next BUA; the hook is the fix).
    InstallHeadGateHook(animClass);
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

}  // namespace ue_wrap::puppet

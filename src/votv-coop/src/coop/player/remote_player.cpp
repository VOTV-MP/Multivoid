#include "coop/player/remote_player.h"

#include "coop/dev/puppet_head_probe.h"
#include "coop/player/client_model.h"
#include "coop/player/local_body.h"
#include "coop/player/skin_effects.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace coop {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace Pup = ue_wrap::puppet;

namespace {

// steady_clock millis (game thread). Same clock everywhere keeps the interp
// math deterministic regardless of wall-clock changes.
uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Shortest-arc delta in degrees: result is in (-180, 180]. Avoids the 359 -> 1
// "long way round" the puppet would otherwise spin (MTA's GetOffsetDegrees).
float OffsetDegrees(float fromDeg, float toDeg) {
    float d = std::fmod(toDeg - fromDeg, 360.f);
    if (d > 180.f)  d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool RemotePlayer::Spawn(const std::string& skinName) {
    // A-5 v2 (2026-05-29 post-ship audit): use players::Registry::Local()
    // first (controller-filtered: picks the genuine local in 3-peer
    // scenarios where puppets are also mainPlayer_C instances). Fall back
    // to FindObjectByClass during the OMEGA splash / pre-possession window
    // where Local() returns null because no controller is attached yet --
    // in that window no puppets exist either, so FindObjectByClass safely
    // returns the only mainPlayer_C in the GUObjectArray. Without the
    // fallback, Spawn fails every tick during boot, the net pump retries
    // once per second, and the pre-existing connect-edge BP-anim SEH
    // cascade has more time to accumulate -> 4 GB+ client RSS climb.
    void* local = players::Registry::Get().Local();
    if (!local) {
        local = R::FindObjectByClass(P::name::MainPlayerClass);
    }
    if (!local) {
        UE_LOGW("RemotePlayer::Spawn: no local mainPlayer_C (not in gameplay yet)");
        return false;
    }

    // Wire convention (post-2026-05-23-evening): source streams its visible
    // mesh WORLD Z (mesh_playerVisible.GetComponentLocation().Z), NOT actor.Z.
    // Rationale + history in [[project-remote-player-open-issues]] (b). On the
    // receiver we write puppet.actor.Z = wire.z directly. The puppet's
    // SkeletalMeshComponent is the actor's ROOT, so puppet.mesh.world.Z =
    // puppet.actor.Z by construction -- exact match to source.mesh.world.Z,
    // zero offset reconstruction.
    ue_wrap::FVector loc = E::GetActorLocation(local);

    // Place the puppet a couple metres in FRONT of the local player so it's
    // immediately in view, then face it back toward the player (so its face --
    // not its back -- is what the user sees). Independent of where P1 looks.
    ue_wrap::FVector fwd = E::GetActorForwardVector(local);
    loc.X += fwd.X * 250.f;
    loc.Y += fwd.Y * 250.f;

    // The kel BASELINE comes from local_body's pristine capture, NOT the local
    // pawn's live mesh -- the local body may itself be skin-swapped (v93), and
    // reading it live would dress every "dr_kel" puppet in OUR custom skin.
    // Fallback to the live read only while the capture hasn't happened yet
    // (local_body ticks on the same pump; the window is a tick or two, during
    // which the local pawn is still un-swapped, so the live read IS pristine).
    void* skin = coop::local_body::NativeBodyMesh();
    if (!skin) skin = Pup::GetMeshPlayerVisibleAsset(local);
    if (!skin) {
        UE_LOGE("RemotePlayer::Spawn: could not read local skin asset");
        return false;
    }
    void* animClass = Pup::GetMeshPlayerVisibleAnimClass(local);

    // v93 skins (docs/COOP_CLIENT_MODEL.md): the puppet wears the skin this peer
    // announced. Same kerfurOmegaV1_Skeleton on every converter pak, so the local
    // AnimClass (kept as-is) drives it 1:1. Graceful-degrade: unresolvable pak
    // (missing on THIS machine) -> the kel baseline.
    if (!coop::client_model::IsNativeSkin(skinName)) {
        if (void* customMesh = coop::client_model::GetSkinMesh(skinName)) {
            skin = customMesh;
            UE_LOGI("RemotePlayer::Spawn: skin '%s' -> mesh %p (anthro AnimClass %p kept)",
                    skinName.c_str(), customMesh, animClass);
        }
    }

    // Both offsets captured from the LOCAL's mesh_playerVisible -- same
    // mainPlayer_C BP on every peer.
    //
    // Yaw: data-driven from the mesh's WORLD forward vector vs the actor's
    // world yaw -- the BP-authored mesh-frame (+Y forward) vs UE-actor-frame
    // (+X forward) shim. Stable (mesh comp RelRot doesn't transient-drift the
    // way RelLoc does on this component chain).
    //
    // Z: -halfH from the inherited ACharacter::CapsuleComponent.CapsuleHalfHeight.
    // Why this and NOT the mesh_playerVisible.RelativeLocation.Z raw field
    // (which the code-architect recommended): the runtime Z-trace 2026-05-23
    // proved `mesh_playerVisible.RelativeLocation.Z = 0.00` ALL THE TIME on
    // VOTV's mainPlayer_C -- the -85 cm offset of its world position from the
    // actor centre is composed via an intermediate AttachParent in the BP
    // graph, NOT stored directly on this component. Reading +0x11C returns
    // zero. The settled `mesh.world.Z - actor.world.Z` is exactly -85.00 cm
    // = -halfH (Z-trace empirical, IDA-confirmed default ACharacter shape).
    // Using -halfH is therefore BOTH stable (the capsule's HalfHeight field at
    // +0x468 doesn't drift like the dynamic world transform does) AND correct
    // (matches the settled world delta to the centimetre). The IDA agent also
    // confirmed only stock ACharacter::Crouch modifies CapsuleHalfHeight at
    // runtime, and Crouch updates it in lockstep with Mesh.RelLoc.Z so the
    // -halfH formula stays valid through crouch transitions on the source.
    // (Receiver-side crouch handling is Phase 2 wire bump.)
    // Puppet transform = wire pose VERBATIM (no actor-level offsets). Both ends
    // are mainPlayer_C since audit H9 (RULE 2 retired the SkelMesh path):
    //   Yaw -- the +Y-forward mesh shim lives INSIDE mesh_playerVisible's
    //     BP-authored RelRot.Yaw (-90) on BOTH the source and the puppet, so
    //     puppet.actor.Yaw == source.actor.Yaw with no shim here (applying -90
    //     at the actor doubled it -- "facing sideways", hands-on 2026-05-25).
    //   Z -- the settled mesh chains are identical BY CLASS, so the offset is
    //     0 by INVARIANT. The old empirical spawn-time chain measure was a
    //     RACE against mesh_playerVisible's BP construction: on a world-fresh
    //     client the authored -85 had not composed at measure time
    //     (2026-07-04 16:43:06 client log: puppetChain=0.00 -> offset -85 ->
    //     the actor held 85 cm LOW forever -> capsule in the floor, the
    //     engine's depenetration fighting our per-tick SetActorLocation =
    //     the "twitching + 1/4 под землей" host puppet). The measure (and the
    //     meshOffsetZ_/meshOffsetYaw_ members) are retired per RULE 2; the
    //     live chain is still LOGGED below as a drift diagnostic.
    actor_ = Pup::SpawnPuppet(loc, skin, animClass);
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnPuppet failed");
        return false;
    }
    // Capture the puppet's GUObjectArray slot while it is known live, so valid()
    // can validate actor_ with IsLiveByIndex (recycling-proof) rather than plain
    // IsLive (which a GC-recycled address defeats). See internalIdx_ in the header.
    internalIdx_ = R::InternalIndexOf(actor_);

    // Complete the skin: the atlas texture (slot-0 MID on both body components).
    // After SpawnPuppet both SetSkeletalMesh writes are done, so the MID override
    // cannot be reset by a later mesh swap. ApplySkinToBody's mesh writes are
    // idempotent here (same-mesh SetSkeletalMesh = engine early-out no-op).
    appliedSkin_.clear();
    ApplySkin(skinName);

    // Eager-resolve the Inc3 hurt-flash material + UFunctions so the first damage
    // flash on this puppet does zero GUObjectArray name walks (cached forever).
    E::WarmupHurtFlashCache();

    // Chain DIAGNOSTIC (measurement is no longer USED -- see the invariant
    // note above the SpawnPuppet call). A settled puppet chain is -halfH; a
    // world-fresh spawn may legitimately log ~0.00 here (mesh_playerVisible's
    // BP -85 composes a moment later on its own). If a future game version
    // changes the authored chain, this line is the drift flag.
    if (void* puppetMesh = Pup::GetSkeletalMeshComponent(actor_)) {
        const float halfH       = E::GetActorCharacterHalfHeight(local);
        const float puppetMeshZ = E::GetComponentLocation(puppetMesh).Z;
        const float puppetActorZ= E::GetActorLocation(actor_).Z;
        UE_LOGI("RemotePlayer::Spawn: chain diag -- halfH=%.2f puppet(meshZ=%.2f actorZ=%.2f "
                "chain=%.2f); offset is ANCHORED 0 (class-identical chains)",
                halfH, puppetMeshZ, puppetActorZ, puppetMeshZ - puppetActorZ);
    }

    // Anim drive: the puppet IS a mainPlayer_C orphan ⇒ BUA's
    // TryGetPawnOwner() returns the puppet itself ⇒ BUA reads
    // Pawn.GetMovementComponent().Velocity as a raw FProperty load on the
    // puppet's OWN CMC (ACharacter::CharacterMovement @+0x288). The puppet's
    // CMC tick is parked by ue_wrap::puppet::SpawnPuppetMainPlayer so nothing
    // else writes Velocity or MovementMode; ApplyToEngine writes them
    // directly each game-thread tick from the streamed pose. BUA then
    // produces spd / useLegIK / rise on the AnimInstance natively, the same
    // way it does on the LOCAL player's mesh_playerVisible AnimInstance --
    // walking/running BlendSpace + IK airborne gate work without any
    // AnimInstance overrides. See research/findings/votv-local-anim-drive-
    // RE-2026-05-27.md. No satellite, no observer, no Controller-cache fix.

    // Face the puppet toward the local player (yaw = direction from puppet to
    // player = -forward). atan2 in degrees. This yaw is in the SOURCE actor
    // convention (matches what ReadLocalPose / mainPlayer_C produces); the
    // mesh-asset orientation shim (+meshOffsetYaw_, BP-authored -90 for VOTV's
    // mesh_playerVisible) is added inside ApplyToEngine at the actor write.
    const float yaw = std::atan2(-fwd.Y, -fwd.X) * 57.29578f;

    // Seed the interpolation state to the wire reference (loc = source.actor
    // pose verbatim -- the same value SpawnPuppet placed the actor at, so the
    // first ApplyToEngine reproduces the spawn transform with no pop).
    curPos_ = loc;
    curYaw_ = yaw;
    curPitch_ = 0.f;
    curHeadYawDelta_ = 0.f;
    curSpeed_ = 0.f;
    bodyYaw_.Reset(yaw);       // presentation yaw starts at the spawn facing
    targetPos_ = loc;
    targetYaw_ = yaw;
    targetPitch_ = 0.f;
    targetHeadYawDelta_ = 0.f;
    window_.Close();
    hasPose_ = false;  // the first network pose SNAPS away from this fake placement
    // Push the spawn placement (curPos_, curYaw_) to the engine NOW -- same
    // world transform as the SpawnActor placement, no visual pop.
    ApplyToEngine();
    dirty_ = false;     // just pushed it

    // One-shot head-graph diagnostic (the IDA agent identified 2 FAnimNode_LookAt
    // instances + 7 ModifyBone instances; we need to know which ModifyBone targets
    // 'head' and what the LookAt's BoneToModify is, before we can write the
    // proper head-tracking fix). Logs at puppet spawn (~once per session).
    if (void* puppetMeshComp = Pup::GetSkeletalMeshComponent(actor_)) {
        Pup::DumpKerfurHeadGraph(puppetMeshComp);
    }

    UE_LOGI("RemotePlayer::Spawn: puppet=%p at (%.0f,%.0f,%.0f) yaw=%.0f nick='%ls'",
            actor_, loc.X, loc.Y, loc.Z, yaw, nickname_.c_str());
    return true;
}

bool RemotePlayer::valid() const {
    // IsLiveByIndex (NOT plain IsLive): validates actor_ against the GUObjectArray
    // slot captured at Spawn, so a GC-freed-then-recycled actor address is
    // rejected by the slot-identity compare instead of passing IsLive's
    // self-read recycling hole. Closes the per-tick ApplyToEngine AV / RSS
    // balloon (root-caused 2026-05-30, 4-peer smoke). actor_!=nullptr short-
    // circuits so a stale internalIdx_ after Destroy is harmless.
    return actor_ != nullptr && R::IsLiveByIndex(actor_, internalIdx_);
}

void RemotePlayer::SetVitals(float health01, float food01, float sleep01) {
    // v20 Inc3: edge-detect a health DROP (this peer took damage) BEFORE
    // overwriting health_, and arm the hurt-flash (Tick toggles the nameplate
    // red). kHurtEpsilon (> 1 wire quantization step) keeps dequantization
    // jitter at a steady health from tripping a false flash. Latest-hit-wins:
    // a fresh drop pushes the deadline out, so rapid hits make one continuous
    // flash. No new wire -- this rides the existing v19 health stream (MTA
    // CPedSync gate-on-change shape). Game thread (SetTargetPose).
    //
    // Gate on hasPose_: it is false during the FIRST SetVitals (SetTargetPose
    // sets it AFTER calling us), so a puppet spawning for an ALREADY-damaged peer
    // (health_ defaults to 1.0, first streamed health < 1.0) seeds health_ WITHOUT
    // a spurious "just took damage" flash. Real in-session drops flash normally.
    if (hasPose_ && health01 + kHurtEpsilon < health_) {
        hurtFlashEndMs_ = NowMs() + kHurtFlashMs;
        static bool sLoggedOnce = false;
        if (!sLoggedOnce) {
            sLoggedOnce = true;
            UE_LOGI("vitals: puppet took damage (health %.2f -> %.2f) -- hurt flash armed (first hit logged)",
                    health_, health01);
        }
    }
    health_ = health01;
    food_ = food01;
    sleep_ = sleep01;
}

void RemotePlayer::SetTargetPose(const coop::net::PoseSnapshot& snap) {
    if (!valid()) { actor_ = nullptr; return; }

    // v19: vitals ride the pose packet -- snap immediately (not interpolated),
    // display-only (nameplate health bar). Done before the interp branches so it
    // applies on the first packet, on a teleport snap, and on the normal path.
    // Never applied to the engine / a saveSlot (see RemotePlayer::SetVitals).
    SetVitals(coop::net::DequantizeUnitFraction(snap.healthFrac),
              coop::net::DequantizeUnitFraction(snap.foodFrac),
              coop::net::DequantizeUnitFraction(snap.sleepFrac));

    // Ragdoll display: edge-detected off the streamed kStateBitRagdoll; the whole
    // lifecycle lives in RagdollDisplay (remote_player_ragdoll.h). A stop (get-up)
    // re-bases the presentation body yaw on the wire truth -- its Update was
    // skipped during the flop and the get-up is a visual discontinuity anyway.
    if (ragdoll_.OnWireBit((snap.stateBits & coop::net::kStateBitRagdoll) != 0,
                           actor_, internalIdx_)) {
        bodyYaw_.Reset(curYaw_);
    }

    const ue_wrap::FVector tgtPos{snap.x, snap.y, snap.z};

    // First packet: the puppet sits at the fake spawn placement (250 cm in
    // front of the local player). Snap to the real pose instead of LERPing
    // across that whole vector -- the placement is a placeholder, not a state.
    if (!hasPose_) {
        curPos_ = tgtPos;
        curYaw_ = snap.yaw;
        curPitch_ = snap.pitch;
        curHeadYawDelta_ = snap.headYawDelta;
        curSpeed_ = snap.speed;
        curStateBits_ = snap.stateBits;
        bodyYaw_.Reset(snap.yaw);  // presentation yaw snaps with the real pose
        targetPos_ = tgtPos;
        targetYaw_ = snap.yaw;
        targetPitch_ = snap.pitch;
        targetHeadYawDelta_ = snap.headYawDelta;
        window_.Close();  // freeze (no interp budget)
        hasPose_ = true;
        ApplyToEngine();
        dirty_ = false;  // just pushed it
        return;
    }

    // Snap on true teleports (door warp / respawn). The legal-motion budget is
    // a fixed base + what they could plausibly cover in half a second at the
    // pose's reported speed -- LAN jitter never reaches it, real teleports do.
    const float dist = Dist3(curPos_, tgtPos);
    const float snapLimit = kSnapBaseCm + kSnapPerSpeedSec * std::fabs(snap.speed);
    if (dist > snapLimit) {
        UE_LOGI("RemotePlayer::SetTargetPose: SNAP (dist=%.0f > %.0f cm)", dist, snapLimit);
        curPos_ = tgtPos;
        curYaw_ = snap.yaw;
        curPitch_ = snap.pitch;
        curHeadYawDelta_ = snap.headYawDelta;
        curSpeed_ = snap.speed;
        curStateBits_ = snap.stateBits;
        bodyYaw_.Reset(snap.yaw);  // a teleport re-bases the presentation yaw too
        targetPos_ = tgtPos;
        targetYaw_ = snap.yaw;
        targetPitch_ = snap.pitch;
        targetHeadYawDelta_ = snap.headYawDelta;
        window_.Close();  // no active window
        ApplyToEngine();
        dirty_ = false;  // just pushed it
        return;
    }

    // Advance-before-rebase (MTA: CClientPed::SetTargetPosition's FIRST line is
    // UpdateTargetPosition()). Bring curPos_ up to NOW using the STILL-OPEN window's
    // cached error before we overwrite the target / recompute the error below. Without
    // this, poses arriving ~every frame re-Open the window at now each packet so the
    // same-frame Tick() reads alpha ~= 0 -> dAlpha ~= 0 -> curPos_ never advances and the
    // puppet trails a moving source by seconds (the interp-starvation bug, root-caused
    // 2026-06-06). Must run BEFORE targetPos_/errorPos_ are overwritten -- it consumes the
    // OLD target/error (its alpha=1 arrival branch snaps curPos_ to the OLD targetPos_).
    AdvanceInterp();

    // Normal path: open a fresh interp window from cur -> new target. The
    // error is cached NOW (target - cur, target's yaw shortest-arc from cur);
    // each Tick applies dAlpha * cachedError so the motion is LINEAR (MTA
    // form, not geometric-decay). If the next packet arrives before alpha=1,
    // it rebases the interp from wherever cur got to.
    targetPos_ = tgtPos;
    targetYaw_ = snap.yaw;
    targetPitch_ = snap.pitch;
    targetHeadYawDelta_ = snap.headYawDelta;
    curSpeed_ = snap.speed;  // speed is not interpolated; AnimBP blends locomotion
    curStateBits_ = snap.stateBits;  // state flags snap immediately
    errorPos_.X = tgtPos.X - curPos_.X;
    errorPos_.Y = tgtPos.Y - curPos_.Y;
    errorPos_.Z = tgtPos.Z - curPos_.Z;
    errorYaw_ = OffsetDegrees(curYaw_, snap.yaw);
    // Pitch is a STRAIGHT delta (no shortest-arc wrap needed): VOTV's
    // PlayerCameraManager physically clamps view pitch to ~(-89, 89) at the
    // source, so cross-180 is impossible in practice regardless of the wire
    // validator's wider (-180, 180] FRotator-axis range.
    errorPitch_ = snap.pitch - curPitch_;
    // headYawDelta is the camera lead in (-180, 180]; a fast spin can cross
    // 180 -> shortest-arc to avoid the "head whips the long way around" pop.
    errorHeadYawDelta_ = OffsetDegrees(curHeadYawDelta_, snap.headYawDelta);
    window_.Open(NowMs(), kInterpWindowMs);
    dirty_ = true;  // a new window is open; Tick will start applying motion this frame
}

void RemotePlayer::AdvanceInterp() {
    // The shared coop::LerpWindow owns the timing (alpha = clamp((now-start)/window,
    // 0,1); dAlpha = alpha-lastAlpha); we apply dAlpha to the cached errors here (MTA
    // linear form, not geometric decay). At alpha=1 the window closes (arrived) and we
    // snap cur=target. Runs every frame from Tick() AND first thing in SetTargetPose
    // (advance-before-rebase) -- see the header for why the latter is load-bearing.
    if (!window_.IsOpen()) return;  // no window open -- frozen at target

    bool arrived = false;
    const float dAlpha = window_.Advance(NowMs(), &arrived);  // same alpha/dAlpha bookkeeping, shared

    curPos_.X         += errorPos_.X         * dAlpha;
    curPos_.Y         += errorPos_.Y         * dAlpha;
    curPos_.Z         += errorPos_.Z         * dAlpha;
    curYaw_           += errorYaw_           * dAlpha;
    curPitch_         += errorPitch_         * dAlpha;
    curHeadYawDelta_  += errorHeadYawDelta_  * dAlpha;
    dirty_ = true;  // pose moved -> needs an engine push (the caller does it)

    if (arrived) {  // window closed at alpha>=1
        curPos_ = targetPos_;  // exact arrival (kills any float drift over the window)
        curYaw_ = targetYaw_;
        curPitch_ = targetPitch_;
        curHeadYawDelta_ = targetHeadYawDelta_;
    }
}

void RemotePlayer::Tick() {
    if (!valid()) { actor_ = nullptr; return; }

    // No receiver-side Z calibration: the wire carries source.mesh.world.Z
    // directly (harness::ReadLocalPose), so the puppet's mesh world.Z is
    // pinned to the source's by construction every ApplyToEngine. Crouch
    // shows up visually because UE4's ACharacter::Crouch shrinks halfH and
    // bumps Mesh.RelLoc.Z to keep mesh.world.Z anchored at ground -- which
    // means the streamed value drops slightly (the visible body actually
    // does descend a few cm in crouch), and the puppet follows along.

    // Advance the open interp window to now (MTA: the per-frame UpdateTargetPosition()).
    // See AdvanceInterp -- the SAME helper runs as the first step of SetTargetPose so a
    // fresh packet rebases from the up-to-date pose (the interp-starvation fix).
    AdvanceInterp();

    // Advance the body-yaw presentation (turn-in-place, coop/puppet_body_yaw.h)
    // -- it keeps moving even when the wire is quiet (the catch-up turn
    // outlives the camera flick that started it), so it must set dirty_
    // itself. Skipped while ragdolled: the pelvis attachment owns the
    // transform and the ragdoll stop re-bases.
    if (!ragdoll_.Active() &&
        bodyYaw_.Update(NowMs(), curSpeed_, curYaw_, curHeadYawDelta_)) {
        dirty_ = true;
    }

    // Skip the engine write when nothing has changed since the last push (frozen
    // at target between packets). The puppet -- whether SkelMesh backup or
    // mainPlayer_C orphan -- runs no physics integration (SkelMesh has no
    // CMC; mainPlayer_C orphan's CMC tick is disabled in
    // puppet::SpawnPuppetMainPlayer + actor tick is also disabled), so a
    // frozen pose genuinely stays where it is; re-writing the same
    // SetActorLocation/Rotation dozens of times per second is wasted
    // UFunction dispatch.
    if (dirty_) {
        ApplyToEngine();
        dirty_ = false;
    }

    // v20 Inc3 damage hurt-flash: toggle the nameplate RED on the EDGE of the
    // flash window. Timestamp-based (NowMs deadline), so the ~0.5 s duration is
    // FPS-independent; fires exactly ONE nameplate repaint when the flash starts
    // and one when it ends -- no per-frame widget churn. hurtFlashEndMs_ is armed
    // by SetVitals on a detected health drop (this peer took damage).
    const bool wantFlash = (hurtFlashEndMs_ != 0 && NowMs() < hurtFlashEndMs_);
    if (wantFlash != hurtFlashActive_) {
        hurtFlashActive_ = wantFlash;
        // The ImGui nameplate reads IsHurtFlashing() each Update() -> the label flashes red.
        // Body pulse: swap the puppet mesh to solid red on the rising edge, restore on
        // the falling edge (Minecraft-style hit flash). The puppet's kel mesh stays
        // VISIBLE while ragdolled (pelvis-attached to the invisible ragdoll body), so the
        // flash composes fine with the ragdoll -- materials are render state.
        if (wantFlash) E::ApplyHurtFlashMaterial(actor_, hurtSavedMaterials_);
        else           E::RestoreHurtFlashMaterial(actor_, hurtSavedMaterials_);
        if (!wantFlash) hurtFlashEndMs_ = 0;
    }
}

void RemotePlayer::SetRagdollPose(const coop::net::RagdollPoseSnapshot& snap) {
    // v22 ragdoll physics stream -- mechanism in remote_player_ragdoll.h (velocity
    // slaving onto the VISIBLE plushie body). Applied-to-live-body marks dirty so
    // ApplyToEngine keeps running its ragdoll head (liveness self-heal) next Tick.
    if (ragdoll_.SetPose(snap)) dirty_ = true;
}

void RemotePlayer::Destroy() {
    if (!actor_) return;
    // The puppet's own CMC carries Velocity / MovementMode that BUA reads
    // each tick; once the actor is destroyed, BUA stops firing for this
    // AnimInstance (the SkeletalMeshComponent is finalized with the actor).
    // No AnimInstance field cleanup needed -- the AnimInstance dies with
    // the actor.
    // Tear down the ragdoll display body FIRST (a SEPARATE actor that would
    // otherwise outlive the puppet as an orphan) + reset its latches.
    ragdoll_.TeardownForDestroy();
    // Same shape for the skin effect rig: its kerfusFace actor is a separate
    // world actor that must not outlive the puppet.
    coop::skin_effects::OnBodyDestroyed(actor_);
    // IsLiveByIndex (consistent with valid()): if the puppet was GC-freed and
    // its address recycled, plain IsLive would pass and DestroyActor would
    // destroy the FOREIGN impostor at that address. The slot-identity compare
    // rejects it -- the real puppet is already gone, so skip DestroyActor.
    if (R::IsLiveByIndex(actor_, internalIdx_)) E::DestroyActor(actor_);
    actor_ = nullptr;
    internalIdx_ = -1;
    hasPose_ = false;
    window_.Close();
    dirty_ = false;
    bodyYaw_.Reset(0.f);    // clears the latch + dt clock; re-seeded at the next Spawn
    hurtFlashEndMs_ = 0;         // v20 Inc3: clear the hurt-flash (nameplate already unregistered)
    hurtFlashActive_ = false;
    hurtSavedMaterials_.clear(); // the mesh died with the actor -- no restore needed, drop stale ptrs
    appliedSkin_.clear();        // v93: the next Spawn re-applies from SkinForSlot
    UE_LOGI("RemotePlayer::Destroy: puppet + nameplate gone");
}

void RemotePlayer::ApplySkin(const std::string& skinName) {
    if (!valid()) return;  // per-slot skin state lives in player_handshake; next Spawn reads it
    if (appliedSkin_ == skinName) return;
    // End an active hurt flash BEFORE the swap: its saved-material set belongs
    // to the OLD mesh/rig -- restoring it after the swap would write stale slot
    // pointers (incl. a torn-down skin-effects face MID whose outer died with
    // the face actor) onto the NEW mesh.
    if (hurtFlashActive_) {
        E::RestoreHurtFlashMaterial(actor_, hurtSavedMaterials_);
        hurtFlashActive_ = false;
        hurtFlashEndMs_ = 0;
    }
    // dr_kel needs the pristine baseline; a custom skin resolves its own mesh.
    void* nativeMesh = coop::local_body::NativeBodyMesh();
    if (coop::client_model::ApplySkinToBody(actor_, skinName, nativeMesh))
        appliedSkin_ = skinName;
}

void RemotePlayer::ApplyToEngine() {
    // While the puppet is RAGDOLLED it is PELVIS-ATTACHED to the VISIBLE plushie body
    // (its own kel meshes are hidden -- the plushie is the display), so the engine
    // syncs its transform per-frame -- do NOT pose-drive it here (a SetActorLocation
    // would fight the attachment). SetTargetPose keeps updating curPos_ from the wire
    // meanwhile, so the first post-recover ApplyToEngine resumes from the owner's
    // current pose. StoppedNow = the body died under us (level-transition GC):
    // self-healed -- re-base the presentation yaw + fall through to pose-drive this tick.
    switch (ragdoll_.DriveAttached(actor_, internalIdx_)) {
    case RagdollDisplay::Drive::Attached:
        return;
    case RagdollDisplay::Drive::StoppedNow:
        bodyYaw_.Reset(curYaw_);
        break;
    case RagdollDisplay::Drive::Inactive:
        break;
    }

    // Wire convention (anchored-zero, 2026-07-04 -- see Spawn's invariant note):
    //   wire.z = source.actor.Z (stable capsule centre, MTA::CEntitySA::vPos
    //            shape -- the physics anchor, unaffected by BP init transients).
    //   puppet.actor = wire pose VERBATIM. Both ends are mainPlayer_C, so the
    //   settled mesh chains are class-identical and no actor-level Z/yaw
    //   offset exists (the old empirical spawn-time measure raced the BP
    //   construction and sank/floated the puppet by halfH when it lost).
    E::SetActorLocation(actor_, curPos_);
    // bodyYaw_ (presentation, coop/puppet_body_yaw.h) -- NOT curYaw_ (wire
    // truth): while standing the body holds so the head can lead.
    E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, bodyYaw_.Yaw(), 0.f});

    // Phase 5F (flashlight cone direction): drive the puppet's lag_fl
    // spring arm pitch so the flashlight cone points where the source
    // is looking. On a real player, lag_fl follows the camera pitch via
    // the actor's tick + spring arm update; on the puppet we explicitly
    // disabled actor tick (orphan-safety), so lag_fl freezes at spawn-
    // time orientation. Without this write the cone points at a static
    // angle (typically the BP's authored default which may face the
    // ground), making the flashlight invisible even when intensity is
    // correctly applied.
    //
    // Audit H7 (2026-05-27): call K2_SetRelativeRotation via reflection
    // instead of the direct field write. Drives UE4's transform propagation
    // (UpdateComponentToWorld) which is the canonical path. UFunction
    // resolved once per process via a function-local static.
    if (auto* mp = reinterpret_cast<uint8_t*>(actor_)) {
        if (void* lag_fl = *reinterpret_cast<void**>(mp + P::off::AmainPlayer_lag_fl)) {
            if (R::IsLive(lag_fl)) {
                static void* sSetRelRotFn = nullptr;
                if (!sSetRelRotFn) {
                    if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
                        sSetRelRotFn = R::FindFunction(sc, P::name::SetRelativeRotationFn);
                    }
                }
                // Relative YAW compensates the body-yaw presentation hold: the
                // cone must point at the CAMERA (curYaw_ + curHeadYawDelta_),
                // but the parent actor now shows bodyYaw_ (turn-in-place) --
                // shortest-arc the difference so the beam tracks the look
                // direction while the body lags. Was 0 when the actor yaw WAS
                // the camera yaw.
                const float flRelYaw = OffsetDegrees(
                    bodyYaw_.Yaw(), curYaw_ + curHeadYawDelta_);
                if (sSetRelRotFn) {
                    ue_wrap::FRotator rot{curPitch_, flRelYaw, 0.f};
                    ue_wrap::ParamFrame f(sSetRelRotFn);
                    f.Set<ue_wrap::FRotator>(L"NewRotation", rot);
                    f.Set<bool>(L"bSweep", false);
                    f.Set<bool>(L"bTeleport", true);
                    ue_wrap::Call(lag_fl, f);
                } else {
                    // Fallback: direct write (function-local; if reflection
                    // can't resolve K2_SetRelativeRotation the pipeline isn't
                    // available anyway, so the engine wouldn't propagate
                    // either way -- preserves prior behavior). FRotator is
                    // {Pitch, Yaw, Roll} floats.
                    auto* rr = reinterpret_cast<float*>(
                        reinterpret_cast<uint8_t*>(lag_fl) +
                        P::off::USceneComponent_RelativeRotation);
                    rr[0] = curPitch_;
                    rr[1] = flRelYaw;
                }
            }
        }
    }

    // Drive the puppet's OWN CMC directly so BUA reads the right Velocity
    // and MovementMode (the same fields the LOCAL player's BUA reads on its
    // possessed CMC, producing spd + useLegIK/rise gates natively). CMC tick
    // is parked by ue_wrap::puppet::SpawnPuppetMainPlayer so we OWN these
    // fields -- no integration fight.
    //
    // Velocity vector: the source streams its ACTOR yaw (body facing) +
    // speed magnitude; reconstruct planar velocity along the body forward
    // axis. UE4 yaw is degrees, atan2 in cmath uses radians.
    // MovementMode: mirror the source's CMC state via PoseSnapshot.stateBits
    // bit 0 -- MOVE_Falling (3) while airborne, MOVE_Walking (1) grounded.
    // The kerfur AnimBP's BUA reads Movement.MovementMode to drive the
    // foot-IK alpha (useLegIK / rise) -- same path the LOCAL uses.
    // Routed through ue_wrap::puppet (Principle 7): the engine-specific
    // CMC offsets stay in the wrapper; coop/ sees only the typed API.
    // (RE: research/findings/votv-local-anim-drive-RE-2026-05-27.md.)
    {
        const float yawRad = curYaw_ * 0.01745329252f;  // PI/180
        const ue_wrap::FVector vel{
            std::cos(yawRad) * curSpeed_,
            std::sin(yawRad) * curSpeed_,
            0.f,
        };
        const bool inAir = (curStateBits_ & coop::net::kStateBitInAir) != 0;
        Pup::DriveCharacterMovement(actor_, vel, inAir);
        // Run-loudness parity: lib_C::step's volume reads CMC.MaxWalkSpeed
        // (the SETTING), which the parked puppet never updates -- mirror the
        // native sprint knob from the streamed speed. Threshold = the same
        // run boundary the stride emitter uses.
        Pup::DriveSprintWalkSpeed(
            actor_, curSpeed_ > coop::puppet_footsteps::Stride::kRunSpeedCmS);
        // Footstep audio (hands-on fix 2026-06-10): the native footstep
        // accumulator lives in the puppet's SUPPRESSED mainPlayer BP tick, so
        // the coop layer strides the interp displacement and dispatches the
        // game's own lib_C::step (see coop/puppet_footsteps.h). The current
        // interp position is the actor's location this frame (cur_). ONE
        // StepDue verdict drives BOTH the native step and the skin step FX
        // (perf audit W2: two independent accumulators drift apart = doubled
        // audible steps for keljoy/mynet skins). The default step's VOLUME is
        // the skin layer's call: a REPLACE-mode variant (mynet) mutes it to 0
        // exactly like the native variant's own lib step call -- lib step
        // still runs its trace/water/friction side effects either way.
        if (footsteps_.StepDue(curPos_, curSpeed_, !inAir)) {
            ue_wrap::votv_lib::CharacterStep(
                actor_, coop::skin_effects::DefaultStepVolume(
                            actor_, coop::puppet_footsteps::Stride::kStepVolume));
            coop::skin_effects::OnStep(actor_, curPos_);
        }
    }

    // Head-look: show WHERE THE REMOTE PLAYER IS LOOKING (not auto-follow the
    // observer). Reconstruct a WORLD look-point from the streamed view -- world
    // yaw = wire yaw (curYaw_) + camera lead (curHeadYawDelta_) = the source's
    // CAMERA yaw, plus pitch -- anchored at the puppet's head, and drive it
    // through the kerfur native lookAt path. The look target is camera TRUTH;
    // the body underneath shows bodyYaw_ (UpdateBodyYaw's turn-in-place hold),
    // so the body-relative LookAt clamps let the head visibly lead until the
    // body catches up. (The original assumption that the SOURCE body lags the
    // camera -- making this emerge from the wire alone -- was falsified by
    // hands-on 2026-06-11: VOTV's first-person body follows the camera
    // immediately, so the lead is synthesized receiver-side instead.)
    // RE: research/findings/votv-puppet-head-look-RE-2026-06-11.md.
    {
        constexpr float kDeg2Rad = 0.01745329252f;
        const float yawRad   = (curYaw_ + curHeadYawDelta_) * kDeg2Rad;
        const float pitchRad = curPitch_ * kDeg2Rad;
        const float cp = std::cos(pitchRad);
        const ue_wrap::FVector head = GetHeadPosition();  // actor X/Y + head-Z
        constexpr float kLookDist = 500.f;  // any positive distance; LookAt uses the direction
        const ue_wrap::FVector worldLook{
            head.X + cp * std::cos(yawRad) * kLookDist,
            head.Y + cp * std::sin(yawRad) * kLookDist,
            // +up. UE pitch-sign caveat: if hands-on shows "looks up -> head tilts
            // DOWN", flip to -std::sin(pitchRad) (a 1-char tuning, per the RE).
            head.Z + std::sin(pitchRad) * kLookDist,
        };
        Pup::DriveHeadLookAtWorld(actor_, worldLook);
        // Positive-confirm probe (ini [votvcoop] puppet_head_probe=1, ~1 Hz, no-op
        // otherwise): measures the DESIRED head twist (look-input vs body yaw) vs the
        // ACTUAL rendered 'head' bone twist + the native LookAtClamp -- proves whether
        // the back-turned freeze is the ~67deg clamp pin before we widen it puppet-only.
        coop::puppet_head_probe::Tick(actor_, bodyYaw_.Yaw(),
                                      curYaw_ + curHeadYawDelta_, curPitch_);
    }
}

bool RemotePlayer::SetLocation(const ue_wrap::FVector& location) {
    if (!valid()) { actor_ = nullptr; return false; }  // valid() = IsLive (not just non-null)
    return E::SetActorLocation(actor_, location);
}

ue_wrap::FVector RemotePlayer::GetLocation() const {
    if (!valid()) return {};  // never read a dying actor (PendingKill on level change)
    return E::GetActorLocation(actor_);
}

ue_wrap::FVector RemotePlayer::GetSyncedAimDirection() const {
    // Mirror DriveHeadLookAtWorld's convention (ApplyToEngine above): yaw = body yaw + head-yaw-delta,
    // pitch = controller pitch. The unit forward is (cp*cos y, cp*sin y, sin p). Same pitch-sign caveat:
    // if a hands-on shows the held clump rising when the puppet looks DOWN, flip the Z sign here too.
    constexpr float kDeg2Rad = 0.01745329252f;
    const float yawRad   = (curYaw_ + curHeadYawDelta_) * kDeg2Rad;
    const float pitchRad = curPitch_ * kDeg2Rad;
    const float cp = std::cos(pitchRad);
    return { cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad) };
}

ue_wrap::FVector RemotePlayer::GetHeadPosition() const {
    if (!valid()) return {};
    // Anchor = the 'head' BONE of whatever mesh the peer is CURRENTLY rendered by
    // (user 2026-07-03: "mount the nameplate to the head bone of the model which
    // player is currently rocking; attach to head bone, just smooth out the movement"):
    //   - ragdolled -> the VISIBLE plushie body's head (the kel meshes are hidden and
    //     the actor rides the pelvis attach, so the old pivot anchor is exactly why
    //     the plate "went nuts" during a flop),
    //   - else -> the visible skin mesh's head (native kel, builtin skins and
    //     converted client models all carry a 'head' bone -- coverage-measured
    //     2026-07-03; a bone-less exotic mesh anchors at the component transform,
    //     UE's own GetSocketLocation fallback).
    // One GetSocketLocation dispatch per call (GetBoneWorldLocationByName resolves
    // the FName from the global name table -- no skeleton enumeration; the May
    // "jaggy" objection was a separate one-frame-late nameplate ACTOR plus the
    // per-call skeleton walk, both long gone). The anim/IK per-frame jitter the
    // pivot anchor used to hide is instead SMOOTHED below.
    constexpr float kPlateLiftCm = 33.f;  // float the plate above the skull (2026-06-07 tuning)
    ue_wrap::FVector raw{};
    bool haveBone = false;
    if (ragdoll_.Active()) {
        void* body = ragdoll_.Body();
        if (body && R::IsLiveByIndex(body, ragdoll_.BodyIdx())) {
            if (void* mesh = E::GetRagdollBodyMesh(body))
                haveBone = E::GetBoneWorldLocationByName(mesh, L"head", raw);
        }
    } else {
        void* mesh = Pup::GetSkeletalMeshComponent(actor_);
        if (mesh && R::IsLive(mesh))
            haveBone = E::GetBoneWorldLocationByName(mesh, L"head", raw);
    }
    if (haveBone) {
        raw.Z += kPlateLiftCm;
    } else {
        // Transient fallback (dying comp / pre-first-anim tick): the old pivot shape.
        raw = GetLocation();
        raw.Z += 30.f;
    }
    // Smoothing (user refinement 2026-07-03: "smooth ТОЛЬКО высоту; X/Y super snappy"):
    // X/Y pass through RAW -- the plate must track walking/strafing with ZERO lag (the
    // full-vector filter read as trailing). The jitter worth hiding is VERTICAL (head
    // bob, crouch blends, the flop's shakes), so only Z runs through the tau ~70 ms
    // low-pass. dt comes from real elapsed time, so the multiple same-tick callers
    // (nameplate + voice) advance the filter only once; a teleport-sized Z jump (>2 m)
    // or the first sample snaps instead of gliding.
    const uint64_t now = NowMs();
    const float dz = raw.Z - headAnchorZ_;
    constexpr float kSnapZCm = 200.f;
    // Snap test is NaN-ROUTING (audit 45bdb7ac W-1): a physics-NaN'd bone read must fall
    // into the snap branch (heals the tick raw turns finite), not the advance branch
    // (which would poison headAnchorZ_ forever -- NaN fails every > comparison).
    if (headAnchorAtMs_ == 0 || !(dz >= -kSnapZCm && dz <= kSnapZCm)) {
        headAnchorZ_ = raw.Z;
    } else if (now > headAnchorAtMs_) {
        const float dtMs = static_cast<float>(now - headAnchorAtMs_);
        headAnchorZ_ += dz * (1.f - std::exp(-dtMs / 70.f));
    }
    headAnchorAtMs_ = now;
    return {raw.X, raw.Y, headAnchorZ_};
}

void RemotePlayer::SetNickname(std::wstring name) { nickname_ = std::move(name); }

}  // namespace coop

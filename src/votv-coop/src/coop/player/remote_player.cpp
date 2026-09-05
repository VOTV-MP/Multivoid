// coop/player/remote_player.cpp -- one remote player's puppet: spawned in front of the local
// player wearing the announced skin, driven by the streamed pose through a linear interpolation
// window (MTA's CClientPed shape), with the body-yaw hold, the head look, the flashlight cone,
// the footsteps, the hurt flash and the ragdoll display.

#include "coop/player/remote_player.h"

#include "coop/dev/puppet_head_probe.h"
#include "coop/player/client_model.h"
#include "coop/player/local_body.h"
#include "coop/player/skin_effects.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/reflected_offset.h"

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

// steady_clock millis; one clock everywhere keeps the interpolation deterministic under
// wall-clock changes.
uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The shortest-arc delta in degrees, in (-180, 180] (MTA's GetOffsetDegrees), so the puppet never
// spins the long way round.
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
    // Registry::Local() first (controller-filtered: the genuine local, not a puppet, which is also
    // a mainPlayer_C); FindObjectByClass as the fallback during the splash and pre-possession
    // window, where no controller is attached and no puppet exists yet. Without it Spawn failed
    // every tick through boot and the net pump's retries let the connect-edge cascade balloon RSS.
    void* local = players::Registry::Get().Local();
    if (!local) {
        local = R::FindObjectByClass(P::name::MainPlayerClass);
        // The fallback must keep the registry's world-currency test: after a world change the dead
        // world's pawn is still in the array and slot-live for seconds, and "the pre-possession
        // window" is indistinguishable from mid-teardown from the inside.
        if (local) {
            if (void* const cur = ue_wrap::world_identity::CurrentWorld()) {
                if (ue_wrap::world_identity::WorldOf(local) != cur) local = nullptr;
            }
        }
    }
    if (!local) {
        UE_LOGW("RemotePlayer::Spawn: no local mainPlayer_C (not in gameplay yet)");
        return false;
    }

    // The wire carries the source's actor pose; the puppet is written as received.
    ue_wrap::FVector loc = E::GetActorLocation(local);

    // Placed a couple of metres in front of the local player and facing them, so it is in view at
    // once; the first real pose snaps away from this placement.
    ue_wrap::FVector fwd = E::GetActorForwardVector(local);
    loc.X += fwd.X * 250.f;
    loc.Y += fwd.Y * 250.f;

    // The kel baseline comes from local_body's pristine capture, not the local pawn's live mesh,
    // which may itself be skin-swapped (every "dr_kel" puppet would wear our custom skin). The live
    // read is the fallback only before the capture, a tick or two during which the pawn is still
    // unswapped.
    void* skin = coop::local_body::NativeBodyMesh();
    if (!skin) skin = Pup::GetMeshPlayerVisibleAsset(local);
    if (!skin) {
        UE_LOGE("RemotePlayer::Spawn: could not read local skin asset");
        return false;
    }
    void* animClass = Pup::GetMeshPlayerVisibleAnimClass(local);

    // The puppet wears the skin this peer announced (docs/players.md); every converter pak shares
    // the kerfurOmegaV1 skeleton, so the local AnimClass drives it. A pak missing on this machine
    // degrades to the kel baseline.
    if (!coop::client_model::IsNativeSkin(skinName)) {
        if (void* customMesh = coop::client_model::GetSkinMesh(skinName)) {
            skin = customMesh;
            UE_LOGI("RemotePlayer::Spawn: skin '%s' -> mesh %p (anthro AnimClass %p kept)",
                    skinName.c_str(), customMesh, animClass);
        }
    }

    // The puppet transform is the wire pose unchanged, with no actor-level offset: both ends are
    // mainPlayer_C, so the mesh chains are identical by class. The +Y-forward mesh shim lives
    // inside mesh_playerVisible's BP-authored relative yaw on both ends (applying it again at the
    // actor doubled it), and the -halfH mesh Z composes on both. A spawn-time chain measurement
    // raced the BP construction on a world-fresh client and sank the puppet by halfH into the
    // floor, where the engine's depenetration fought the per-tick SetActorLocation.
    actor_ = Pup::SpawnPuppet(loc, skin, animClass);
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnPuppet failed");
        return false;
    }
    // Capture the puppet's GUObjectArray slot while it is live, so valid() can use IsLiveByIndex;
    // plain IsLive is defeated by a recycled address.
    internalIdx_ = R::InternalIndexOf(actor_);

    // Complete the skin (the atlas texture on both body components) after both SetSkeletalMesh
    // writes, so a later mesh swap cannot reset the override; ApplySkinToBody's mesh writes are
    // idempotent here.
    appliedSkin_.clear();
    ApplySkin(skinName);

    // Eager-resolve the hurt-flash material and functions, so the first flash does no name walks.
    E::WarmupHurtFlashCache();

    // A chain diagnostic, not a measurement in use: a settled puppet chain is -halfH, and a
    // world-fresh spawn may log ~0 while the BP composes; a future game version that changes the
    // authored chain shows here.
    if (void* puppetMesh = Pup::GetSkeletalMeshComponent(actor_)) {
        const float halfH       = E::GetActorCharacterHalfHeight(local);
        const float puppetMeshZ = E::GetComponentLocation(puppetMesh).Z;
        const float puppetActorZ= E::GetActorLocation(actor_).Z;
        UE_LOGI("RemotePlayer::Spawn: chain diag -- halfH=%.2f puppet(meshZ=%.2f actorZ=%.2f "
                "chain=%.2f); offset is ANCHORED 0 (class-identical chains)",
                halfH, puppetMeshZ, puppetActorZ, puppetMeshZ - puppetActorZ);
    }

    // The anim drive: the puppet is a mainPlayer_C, so its AnimBP's update reads the puppet's own
    // CharacterMovement velocity and mode; that component's tick is parked at spawn, so nothing
    // else writes them, and ApplyToEngine writes them each tick from the streamed pose. The
    // locomotion blend, the leg IK and the airborne gate then work natively, as on the local
    // player.

    // Face the puppet toward the local player (the yaw from puppet to player, in the source actor's
    // convention).
    const float yaw = std::atan2(-fwd.Y, -fwd.X) * 57.29578f;

    // Seed the interpolation state to the placement, so the first ApplyToEngine reproduces the
    // spawn transform with no pop.
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
    // Push the placement to the engine now: the same transform SpawnActor placed, no visual pop.
    ApplyToEngine();
    dirty_ = false;     // just pushed it

    // A one-shot head-graph diagnostic at spawn (the AnimBP's LookAt and ModifyBone nodes).
    if (void* puppetMeshComp = Pup::GetSkeletalMeshComponent(actor_)) {
        Pup::DumpKerfurHeadGraph(puppetMeshComp);
    }

    UE_LOGI("RemotePlayer::Spawn: puppet=%p at (%.0f,%.0f,%.0f) yaw=%.0f nick='%ls'",
            actor_, loc.X, loc.Y, loc.Z, yaw, nickname_.c_str());
    return true;
}

bool RemotePlayer::valid() const {
    // IsLiveByIndex, not IsLive: the slot captured at Spawn rejects a GC-freed and recycled address
    // that IsLive's self-read would pass (a per-tick access violation and an RSS balloon once). The
    // null check short-circuits, so a stale index after Destroy is harmless.
    return actor_ != nullptr && R::IsLiveByIndex(actor_, internalIdx_);
}

void RemotePlayer::SetVitals(float health01, float food01, float sleep01) {
    // Edge-detect a health drop before overwriting health_ and arm the hurt flash; kHurtEpsilon is
    // over one wire quantisation step, so dequantisation jitter at a steady health never trips it,
    // and a fresh drop pushes the deadline out (rapid hits make one flash). Gated on hasPose_: the
    // first SetVitals runs before it is set, so a puppet spawning for an already damaged peer seeds
    // health_ without a flash.
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

    // The vitals ride the pose packet and snap (display only, the nameplate bar); before the
    // interpolation branches, so they apply on the first packet, a teleport snap and the normal
    // path.
    SetVitals(coop::net::DequantizeUnitFraction(snap.healthFrac),
              coop::net::DequantizeUnitFraction(snap.foodFrac),
              coop::net::DequantizeUnitFraction(snap.sleepFrac));

    // The ragdoll display, edge-detected off the streamed bit (RagdollDisplay owns the lifecycle);
    // a stop re-bases the presentation yaw on the wire truth, since its Update was skipped during
    // the flop.
    if (ragdoll_.OnWireBit((snap.stateBits & coop::net::kStateBitRagdoll) != 0,
                           actor_, internalIdx_)) {
        bodyYaw_.Reset(curYaw_);
    }

    const ue_wrap::FVector tgtPos{snap.x, snap.y, snap.z};

    // The first packet: the puppet sits at the placeholder placement, so snap rather than
    // interpolate across the whole vector.
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

    // Snap on a true teleport (a door warp, a respawn): the legal-motion budget is a base plus half
    // a second at the reported speed, which LAN jitter never reaches.
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

    // Advance before rebasing (MTA's SetTargetPosition calls UpdateTargetPosition first): bring
    // curPos_ up to now with the still-open window's cached error before the target is overwritten.
    // Poses arriving every frame otherwise re-open the window at now, the same-frame Tick reads
    // alpha ~0, and the puppet trails a moving source by seconds.
    AdvanceInterp();

    // The normal path: a fresh window from cur to the new target, the error cached now, each Tick
    // applying dAlpha times the cached error, so the motion is linear (MTA's form, not a geometric
    // decay); a packet arriving before alpha 1 rebases from wherever cur got to.
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
    // Pitch is a straight delta: the source clamps view pitch to about (-89, 89), so it never
    // crosses 180.
    errorPitch_ = snap.pitch - curPitch_;
    // headYawDelta is the camera lead in (-180, 180]; a fast spin can cross 180, so shortest-arc.
    errorHeadYawDelta_ = OffsetDegrees(curHeadYawDelta_, snap.headYawDelta);
    window_.Open(NowMs(), kInterpWindowMs);
    dirty_ = true;  // a new window is open; Tick will start applying motion this frame
}

void RemotePlayer::AdvanceInterp() {
    // LerpWindow owns the timing (alpha = clamp((now - start) / window), dAlpha per call); dAlpha
    // times the cached errors here, and at alpha 1 cur snaps to target. Runs every Tick and first
    // thing in SetTargetPose.
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

    // No receiver-side Z calibration: the wire carries the source's actor Z and the puppet's is
    // pinned to it every ApplyToEngine; a crouch descends a few centimetres at the source and the
    // puppet follows.

    // Advance the open window to now (MTA's per-frame UpdateTargetPosition); the same helper opens
    // SetTargetPose.
    AdvanceInterp();

    // Advance the body-yaw presentation (the turn in place, coop/puppet_body_yaw.h), which keeps
    // moving while the wire is quiet, so it sets dirty_ itself. Skipped while ragdolled: the pelvis
    // attachment owns the transform.
    if (!ragdoll_.Active() &&
        bodyYaw_.Update(NowMs(), curSpeed_, curYaw_, curHeadYawDelta_)) {
        dirty_ = true;
    }

    // Skip the engine write when nothing changed since the last push: the puppet runs no physics
    // integration (its movement and actor ticks are parked), so a frozen pose stays where it is,
    // and re-writing the same transform dozens of times a second is wasted dispatch.
    if (dirty_) {
        ApplyToEngine();
        dirty_ = false;
    }

    // The hurt flash toggles on the edges of its window: a deadline, so the ~0.5 s is
    // FPS-independent, and exactly one repaint at the start and one at the end.
    const bool wantFlash = (hurtFlashEndMs_ != 0 && NowMs() < hurtFlashEndMs_);
    if (wantFlash != hurtFlashActive_) {
        hurtFlashActive_ = wantFlash;
        // The nameplate reads IsHurtFlashing() and flashes red; the body swaps to the hurt material
        // on the rising edge and restores on the falling one. The kel mesh stays visible while
        // ragdolled (pelvis-attached to the invisible ragdoll body), so the flash composes with it.
        if (wantFlash) E::ApplyHurtFlashMaterial(actor_, hurtSavedMaterials_);
        else           E::RestoreHurtFlashMaterial(actor_, hurtSavedMaterials_);
        if (!wantFlash) hurtFlashEndMs_ = 0;
    }
}

void RemotePlayer::SetRagdollPose(const coop::net::RagdollPoseSnapshot& snap) {
    // The ragdoll physics stream (remote_player_ragdoll.h slaves the velocity onto the visible
    // body); applied to a live body it marks dirty, so ApplyToEngine keeps running its ragdoll
    // head.
    if (ragdoll_.SetPose(snap)) dirty_ = true;
}

void RemotePlayer::Destroy() {
    if (!actor_) return;
    // The AnimInstance dies with the actor, so no field cleanup. The ragdoll display body is a
    // separate actor that would outlive the puppet as an orphan: torn down first, with its latches.
    ragdoll_.TeardownForDestroy();
    // The skin effect rig's face actor is a separate world actor too.
    coop::skin_effects::OnBodyDestroyed(actor_);
    // IsLiveByIndex: if the puppet was GC-freed and its address recycled, plain IsLive would pass
    // and DestroyActor would destroy the foreign object at that address.
    if (R::IsLiveByIndex(actor_, internalIdx_)) E::DestroyActor(actor_);
    actor_ = nullptr;
    internalIdx_ = -1;
    hasPose_ = false;
    window_.Close();
    dirty_ = false;
    bodyYaw_.Reset(0.f);    // clears the latch + dt clock; re-seeded at the next Spawn
    hurtFlashEndMs_ = 0;         // clear the hurt flash (the nameplate is already unregistered)
    hurtFlashActive_ = false;
    hurtSavedMaterials_.clear(); // the mesh died with the actor -- no restore needed, drop stale ptrs
    appliedSkin_.clear();        // the next Spawn re-applies from SkinForSlot
    UE_LOGI("RemotePlayer::Destroy: puppet + nameplate gone");
}

void RemotePlayer::ApplySkin(const std::string& skinName) {
    if (!valid()) return;  // per-slot skin state lives in player_handshake; next Spawn reads it
    if (appliedSkin_ == skinName) return;
    // End an active hurt flash before the swap: its saved materials belong to the old rig, and
    // restoring them after the swap would write stale slot pointers (a torn-down face MID among
    // them) onto the new mesh.
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
    // While ragdolled the puppet is pelvis-attached to the visible flop body (its own meshes
    // hidden), so the engine syncs its transform and a SetActorLocation would fight the attachment;
    // curPos_ keeps tracking the wire meanwhile, so the first post-recover apply resumes from the
    // owner's pose. StoppedNow means the body died under us (a level-transition GC): re-base the
    // yaw and fall through.
    switch (ragdoll_.DriveAttached(actor_, internalIdx_)) {
    case RagdollDisplay::Drive::Attached:
        return;
    case RagdollDisplay::Drive::StoppedNow:
        bodyYaw_.Reset(curYaw_);
        break;
    case RagdollDisplay::Drive::Inactive:
        break;
    }

    // The wire carries the source's actor Z (the capsule centre, MTA's vPos shape, unaffected by BP
    // init transients), and the puppet's actor is the wire pose unchanged: both ends are
    // mainPlayer_C, so no actor-level Z or yaw offset exists.
    E::SetActorLocation(actor_, curPos_);
    // bodyYaw_ (the presentation), not curYaw_ (the wire truth): standing, the body holds so the
    // head can lead.
    E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, bodyYaw_.Yaw(), 0.f});

    // The flashlight cone: drive the puppet's lag_fl spring arm pitch so the cone points where the
    // source looks. On a real player the actor's tick updates it from the camera; the puppet's
    // actor tick is off, so it would freeze at the spawn orientation and the cone would point at
    // the ground. Through K2_SetRelativeRotation by reflection, the canonical transform
    // propagation; the function resolves once.
    if (auto* mp = reinterpret_cast<uint8_t*>(actor_)) {
        if (void* lag_fl = *reinterpret_cast<void**>(mp + P::off::AmainPlayer_lag_fl)) {
            if (R::IsLive(lag_fl)) {
                static void* sSetRelRotFn = nullptr;
                if (!sSetRelRotFn) {
                    if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
                        sSetRelRotFn = R::FindFunction(sc, P::name::SetRelativeRotationFn);
                    }
                }
                // The relative yaw compensates the body-yaw hold: the cone must point at the camera
                // (curYaw_ + curHeadYawDelta_) while the parent shows bodyYaw_, so the difference
                // is shortest-arced.
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
                    // Fallback: a direct write; if the function does not resolve the engine would
                    // not propagate either way. FRotator is {Pitch, Yaw, Roll}.
                    auto* rr = reinterpret_cast<float*>(
                        reinterpret_cast<uint8_t*>(lag_fl) +
                        P::off::USceneComponent_RelativeRotation);
                    rr[0] = curPitch_;
                    rr[1] = flRelYaw;
                }
            }
        }
    }

    // Drive the puppet's own CharacterMovement so the AnimBP's update reads the velocity and mode
    // it reads on the local player (spd and the leg-IK gates); its tick is parked, so these fields
    // are ours. The source streams its actor yaw and a speed magnitude, so the planar velocity is
    // rebuilt along the body forward; the mode mirrors the in-air bit (MOVE_Falling or
    // MOVE_Walking), which the AnimBP reads for the foot-IK alpha. Through ue_wrap::puppet, so the
    // CMC offsets stay in the wrapper.
    {
        const float yawRad = curYaw_ * 0.01745329252f;  // PI/180
        const ue_wrap::FVector vel{
            std::cos(yawRad) * curSpeed_,
            std::sin(yawRad) * curSpeed_,
            0.f,
        };
        const bool inAir = (curStateBits_ & coop::net::kStateBitInAir) != 0;
        Pup::DriveCharacterMovement(actor_, vel, inAir);
        // Run-loudness parity: lib_C::step's volume reads MaxWalkSpeed, which the parked puppet
        // never updates, so the native sprint knob is mirrored from the streamed speed at the
        // stride emitter's run boundary.
        Pup::DriveSprintWalkSpeed(
            actor_, curSpeed_ > coop::puppet_footsteps::Stride::kRunSpeedCmS);
        // Footstep audio: the native accumulator lives in the puppet's suppressed BP tick, so the
        // coop layer strides the interpolated displacement and dispatches the game's own
        // lib_C::step (coop/puppet_footsteps.h). One StepDue verdict drives both the native step
        // and the skin step effects (two accumulators drift apart into doubled steps). The default
        // step's volume is the skin layer's call: a replace-mode variant mutes it to 0, and lib
        // step still runs its trace, water and friction side effects.
        if (footsteps_.StepDue(curPos_, curSpeed_, !inAir)) {
            ue_wrap::votv_lib::CharacterStep(
                actor_, coop::skin_effects::DefaultStepVolume(
                            actor_, coop::puppet_footsteps::Stride::kStepVolume));
            coop::skin_effects::OnStep(actor_, curPos_);
        }
    }

    // The head look shows where the remote player is looking: a world look point rebuilt from the
    // streamed view (the wire yaw plus the camera lead, and the pitch), anchored at the puppet's
    // head, through the kerfur AnimBP's native lookAt path. The target is camera truth while the
    // body shows bodyYaw_'s hold, so the body-relative clamps let the head lead until the body
    // catches up. The lead is synthesised here: the source's first-person body follows the camera
    // at once, so it never emerges from the wire.
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
            // Up is positive.
            head.Z + std::sin(pitchRad) * kLookDist,
        };
        Pup::DriveHeadLookAtWorld(actor_, worldLook);
        // The head probe (ini puppet_head_probe=1, ~1 Hz, otherwise a no-op): the desired head
        // twist against the rendered head bone's and the native clamp.
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
    // The same convention as the head look: yaw = the wire yaw + the head-yaw delta, pitch = the
    // controller pitch; the unit forward is (cp cos y, cp sin y, sin p).
    constexpr float kDeg2Rad = 0.01745329252f;
    const float yawRad   = (curYaw_ + curHeadYawDelta_) * kDeg2Rad;
    const float pitchRad = curPitch_ * kDeg2Rad;
    const float cp = std::cos(pitchRad);
    return { cp * std::cos(yawRad), cp * std::sin(yawRad), std::sin(pitchRad) };
}

ue_wrap::FVector RemotePlayer::GetHeadPosition() const {
    if (!valid()) return {};
    // The anchor is the head bone of whatever mesh renders the peer right now: the visible flop
    // body while ragdolled (the kel meshes are hidden and the actor rides the pelvis attach, which
    // is why a pivot anchor went wild in a flop), else the visible skin mesh (the native kel, the
    // built-in skins and the converted client models all carry a head bone; a bone-less mesh
    // anchors at the component transform). One GetSocketLocation dispatch per call; the per-frame
    // anim jitter is smoothed below.
    constexpr float kPlateLiftCm = 33.f;  // float the plate above the skull
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
        // The transient fallback (a dying component, the pre-first-anim tick): the actor pivot.
        raw = GetLocation();
        raw.Z += 30.f;
    }
    // Smoothing: X and Y pass through raw (the plate must track walking with zero lag), and only Z,
    // where the jitter lives (head bob, crouch blends, the flop), runs through a ~70 ms low-pass.
    // dt is real elapsed time, so several same-tick callers advance the filter once; a
    // teleport-sized jump or the first sample snaps.
    const uint64_t now = NowMs();
    const float dz = raw.Z - headAnchorZ_;
    constexpr float kSnapZCm = 200.f;
    // The snap test routes NaN: a NaN bone read must fall into the snap branch (healed the tick the
    // raw turns finite), not the advance branch, which would poison the anchor forever, since NaN
    // fails every comparison.
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

#include "coop/remote_player.h"

#include "coop/nameplate.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

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

bool RemotePlayer::Spawn() {
    // The local player gives us BOTH the spawn anchor and the skin to copy.
    void* local = R::FindObjectByClass(P::name::MainPlayerClass);
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

    void* skin = Pup::GetMeshPlayerVisibleAsset(local);
    if (!skin) {
        UE_LOGE("RemotePlayer::Spawn: could not read local skin asset");
        return false;
    }
    void* animClass = Pup::GetMeshPlayerVisibleAnimClass(local);

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
    if (void* srcMeshComp = Pup::GetMeshPlayerVisibleComponent(local)) {
        const ue_wrap::FRotator localActorRot = E::GetActorRotation(local);
        const ue_wrap::FVector meshFwd = E::GetComponentForwardVector(srcMeshComp);
        const float meshWorldYaw = std::atan2(meshFwd.Y, meshFwd.X) * 57.29577951f;
        meshOffsetYaw_ = ue_wrap::NormalizeAxis(meshWorldYaw - localActorRot.Yaw);
    }
    const float halfH = E::GetActorCharacterHalfHeight(local);
    meshOffsetZ_ = -halfH;
    UE_LOGI("RemotePlayer::Spawn: meshOffsetZ_=%.2f (= -halfH, capsule HalfHeight @+0x468) "
            "meshOffsetYaw_=%.2f", meshOffsetZ_, meshOffsetYaw_);

    // Spawn-placement Z = actor.Z + meshOffsetZ_, same formula as ApplyToEngine.
    // No visual pop between SpawnActor and the first ApplyToEngine because both
    // produce the same world transform.
    ue_wrap::FVector spawnLoc = loc;
    spawnLoc.Z = loc.Z + meshOffsetZ_;
    actor_ = Pup::SpawnPuppet(spawnLoc, skin, animClass);
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnPuppet failed");
        return false;
    }

    // Bug 2 / Plan B2 (root-cause fix per RULE 1): the AnimBP's
    // BlueprintUpdateAnimation reads Pawn->GetMovementComponent()->Velocity
    // and writes a LOT of AnimInstance fields from it (Movement, spd, IK
    // targets, lookAt, headLookAt, packed bools). On a SkeletalMeshActor
    // puppet, Pawn is null and BUA short-circuits -> AnimInstance is vastly
    // under-populated (8 set fields vs ~26 on local, confirmed via the full
    // AnimBP_vars_all diff dump 2026-05-23). The state machine transition
    // reads one or more of those missing fields and stays in idle.
    //
    // Fix: spawn a hidden, inert satellite ACharacter co-located with the
    // puppet. Set its CharacterMovementComponent.Velocity from the streamed
    // pose each tick. Write the puppet AnimInstance.Pawn pointer at +0x2D70
    // to point at this satellite. BUA then runs naturally on the puppet's
    // AnimInstance -- reading Pawn=satellite, Movement=satellite.CMC,
    // Movement.Velocity=our streamed velocity -- and populates EVERY field
    // the state machine looks at. Transitions fire on the same conditions
    // the local mainPlayer's AnimInstance does, so the puppet walks/runs
    // exactly like a normal kerfur.
    //
    // The satellite is invisible + has its CMC tick disabled (CMC would
    // otherwise integrate Velocity into position and reset it each tick
    // from its own state machine; we just need it as a Velocity data
    // holder, NOT a physics simulator).
    satellite_ = ue_wrap::engine::SpawnSatelliteCharacter(loc);
    if (satellite_) {
        satelliteCmc_ = ue_wrap::engine::GetCharacterMovementComponent(satellite_);
        if (satelliteCmc_) {
            // Park the CMC -- we own the Velocity field; CMC physics would
            // overwrite it each tick otherwise.
            ue_wrap::engine::SetComponentTickEnabled(satelliteCmc_, false);
        }
        // Diagnostic kept for future re-derivation across game versions: the
        // UMovementComponent::Velocity offset (0xC4 on UE4.27) is the field we
        // write each tick; LogClassProperties dumps the full FProperty chain
        // so we can confirm the offset hasn't shifted in a new build. Fires
        // once at first puppet spawn.
        ue_wrap::engine::LogClassProperties(L"MovementComponent");
        // Wire puppet AnimInstance -> satellite Pawn + Movement. Both pointers
        // must be written: the AnimBP's BlueprintBeginPlay caches both Pawn
        // and Movement from `TryGetPawnOwner()` at AnimInstance construction.
        // For our puppet that ran with Pawn=null, so both caches stayed null
        // -- the per-tick BUA does NOT re-resolve them from Pawn. The
        // 2026-05-23 velocity-chain diagnostic proved this: setting Pawn alone
        // left Movement=null, so BUA's spd write (Movement.Velocity.Size())
        // never executed (puppet.spd stayed at the construction default 400).
        // Writing BOTH unblocks BUA's per-tick velocity-pull path entirely.
        if (void* comp = Pup::GetSkeletalMeshComponent(actor_)) {
            void* anim = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(comp) + P::off::USkeletalMesh_AnimScriptInstance);
            if (anim) {
                puppetAnim_ = anim;
                ue_wrap::engine::WriteObjectField(anim, P::off::AnimBP_kerfur_Pawn, satellite_);
                if (satelliteCmc_) {
                    ue_wrap::engine::WriteObjectField(anim, P::off::AnimBP_kerfur_Movement, satelliteCmc_);
                }
                UE_LOGI("RemotePlayer::Spawn: wired puppet AnimInstance %p .Pawn=%p .Movement=%p",
                        anim, satellite_, satelliteCmc_);
            }
        }
    } else {
        UE_LOGE("RemotePlayer::Spawn: SpawnSatelliteCharacter failed -- puppet will not animate");
    }

    // Face the puppet toward the local player (yaw = direction from puppet to
    // player = -forward). atan2 in degrees. This yaw is in the SOURCE actor
    // convention (matches what ReadLocalPose / mainPlayer_C produces); the
    // mesh-asset orientation shim (+meshOffsetYaw_, BP-authored -90 for VOTV's
    // mesh_playerVisible) is added inside ApplyToEngine at the actor write.
    const float yaw = std::atan2(-fwd.Y, -fwd.X) * 57.29578f;

    // Seed the interpolation state to the ACTOR-Z reference (loc, NOT spawnLoc):
    // ApplyToEngine adds meshOffsetZ_ each tick, so curPos_.Z must carry the
    // RAW actor.Z (== what the wire will deliver as source.actor.Z). If we
    // seeded with spawnLoc (= loc + meshOffsetZ_), ApplyToEngine would add
    // meshOffsetZ_ a SECOND time and the puppet would float by exactly
    // |meshOffsetZ_| for one frame. The SpawnActor placement (spawnLoc above)
    // and curPos_=loc + ApplyToEngine produce the SAME world Z by construction.
    curPos_ = loc;
    curYaw_ = yaw;
    curPitch_ = 0.f;
    curHeadYawDelta_ = 0.f;
    curSpeed_ = 0.f;
    targetPos_ = loc;
    targetYaw_ = yaw;
    targetPitch_ = 0.f;
    targetHeadYawDelta_ = 0.f;
    interpStartMs_ = 0;
    interpFinishMs_ = 0;
    lastAlpha_ = 0.f;
    hasPose_ = false;  // the first network pose SNAPS away from this fake placement
    // Push the spawn placement (curPos_=actor.Z, curYaw_) to the engine NOW.
    // ApplyToEngine adds meshOffsetZ_ to curPos_.Z and meshOffsetYaw_ to
    // curYaw_, producing the same world transform as the SpawnActor placement
    // (= loc.Z + meshOffsetZ_). No visual pop between SpawnActor and the first
    // ApplyToEngine.
    ApplyToEngine();
    dirty_ = false;     // just pushed it

    // Show the floating nameplate above this body (lazily hooks the HUD draw).
    nameplate::Register(this);

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

bool RemotePlayer::valid() const { return actor_ != nullptr && R::IsLive(actor_); }

void RemotePlayer::SetTargetPose(const coop::net::PoseSnapshot& snap) {
    if (!valid()) { actor_ = nullptr; return; }
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
        targetPos_ = tgtPos;
        targetYaw_ = snap.yaw;
        targetPitch_ = snap.pitch;
        targetHeadYawDelta_ = snap.headYawDelta;
        interpFinishMs_ = 0;  // freeze (no interp budget)
        lastAlpha_ = 0.f;
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
        targetPos_ = tgtPos;
        targetYaw_ = snap.yaw;
        targetPitch_ = snap.pitch;
        targetHeadYawDelta_ = snap.headYawDelta;
        interpFinishMs_ = 0;  // no active window
        lastAlpha_ = 0.f;
        ApplyToEngine();
        dirty_ = false;  // just pushed it
        return;
    }

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
    const uint64_t now = NowMs();
    interpStartMs_ = now;
    interpFinishMs_ = now + kInterpWindowMs;
    lastAlpha_ = 0.f;
    dirty_ = true;  // a new window is open; Tick will start applying motion this frame
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

    // Advance interpolation if a window is open. MTA shape: error is cached
    // ONCE at packet arrival (= target - cur at that moment). Each frame:
    //   alpha    = (now - start) / window   (clamped 0..1)
    //   dAlpha   = alpha - lastAlpha
    //   cur     += errorPos * dAlpha        (linear; not geometric decay)
    //   lastAlpha = alpha
    // At alpha=1 the puppet has applied the full error; we then freeze
    // (interpFinishMs_=0) until the next packet rebases the interp.
    if (interpFinishMs_ != 0) {
        const uint64_t now = NowMs();
        const uint64_t span = interpFinishMs_ - interpStartMs_;  // == kInterpWindowMs
        float alpha = (span == 0) ? 1.f
                                  : static_cast<float>(now - interpStartMs_) / static_cast<float>(span);
        alpha = std::clamp(alpha, 0.f, 1.f);
        const float dAlpha = alpha - lastAlpha_;
        lastAlpha_ = alpha;

        curPos_.X         += errorPos_.X         * dAlpha;
        curPos_.Y         += errorPos_.Y         * dAlpha;
        curPos_.Z         += errorPos_.Z         * dAlpha;
        curYaw_           += errorYaw_           * dAlpha;
        curPitch_         += errorPitch_         * dAlpha;
        curHeadYawDelta_  += errorHeadYawDelta_  * dAlpha;
        dirty_ = true;  // pose moved this frame -> needs an engine push

        if (alpha >= 1.f) {
            curPos_ = targetPos_;  // exact arrival (kills any float drift over the window)
            curYaw_ = targetYaw_;
            curPitch_ = targetPitch_;
            curHeadYawDelta_ = targetHeadYawDelta_;
            interpFinishMs_ = 0;
        }
    }

    // Skip the engine write when nothing has changed since the last push (frozen
    // at target between packets). The puppet's SkeletalMeshActor has no
    // physics/CharacterMovement -- it cannot move on its own, so a frozen pose
    // genuinely stays where it is; re-writing the same SetActorLocation/Rotation
    // dozens of times per second is wasted UFunction dispatch.
    if (dirty_) {
        ApplyToEngine();
        dirty_ = false;
    }
}

void RemotePlayer::Destroy() {
    if (!actor_) return;
    // Order: clear AnimInstance.Pawn pointer (so BUA can't dereference a freed
    // satellite next tick), then destroy satellite, then destroy puppet actor,
    // then nameplate.
    if (puppetAnim_ && R::IsLive(puppetAnim_)) {
        E::WriteObjectField(puppetAnim_, P::off::AnimBP_kerfur_Pawn, nullptr);
    }
    if (satellite_) {
        if (R::IsLive(satellite_)) E::DestroyActor(satellite_);
        satellite_ = nullptr;
        satelliteCmc_ = nullptr;
    }
    puppetAnim_ = nullptr;
    nameplate::Unregister(this);  // drops + destroys the floating label
    if (R::IsLive(actor_)) E::DestroyActor(actor_);
    actor_ = nullptr;
    hasPose_ = false;
    interpFinishMs_ = 0;
    lastAlpha_ = 0.f;
    dirty_ = false;
    UE_LOGI("RemotePlayer::Destroy: puppet + satellite + nameplate gone");
}

void RemotePlayer::ApplyToEngine() {
    // Option G wire convention (2026-05-23-evening, code-architect verdict +
    // MTA fidelity):
    //   wire.z = source.actor.Z (stable capsule centre, MTA::CEntitySA::vPos
    //            shape -- the physics anchor, unaffected by BP init transients)
    //   puppet.actor.Z = wire.z + meshOffsetZ_  (where meshOffsetZ_ is the
    //                    LOCAL's raw BP-authored mesh.RelLoc.Z, captured once
    //                    at Spawn -- identical on every instance of the same
    //                    mainPlayer_C BP)
    // Net effect: puppet's mesh comp (= the actor's root, so puppet.mesh.world.Z
    // = puppet.actor.Z) lands at SETTLED source.mesh.world.Z by reconstruction
    // -- the source's actor.Z is stable, the offset is stable, the result is
    // stable. The 84cm mid-init swing in mesh.world.Z on the source does NOT
    // reach the wire, so the puppet does not "jump afloat after init" anymore.
    //
    // Yaw: data-driven mesh-+Y-forward vs UE-actor-+X-forward shim, also
    // captured once at Spawn.
    ue_wrap::FVector puppetLoc = curPos_;
    puppetLoc.Z += meshOffsetZ_;
    E::SetActorLocation(actor_, puppetLoc);
    E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, curYaw_ + meshOffsetYaw_, 0.f});

    // Bug 2 Plan B2: drive the satellite Character (the AnimBP Pawn pull source)
    // so its CharacterMovementComponent.Velocity carries the streamed velocity.
    // Co-locate the satellite with the puppet so any IK targets BUA computes
    // (footZ, floorFoot, lookAt) sit at sane world coordinates. Both axes of
    // the velocity vector are derived from yaw + speed (the puppet is upright;
    // Z velocity stays zero -- the BlendSpace X-input is the planar magnitude
    // = |vx|^2+|vy|^2 = speed^2 -> sqrt = speed, exactly what BUA computes).
    if (satellite_ && R::IsLive(satellite_)) {
        E::SetActorLocation(satellite_, curPos_);
        if (satelliteCmc_) {
            // Derive the planar velocity vector from streamed yaw + speed. The
            // ACTOR yaw (not camera yaw) is what gets streamed; the source's
            // body movement is along its actor +X forward, so velocity points
            // along (cos(yaw), sin(yaw)) * speed. UE4 yaw is in degrees,
            // measured from +X around +Z (right-handed Z-up); cosf/sinf use
            // radians.
            const float yawRad = curYaw_ * 0.01745329252f;  // PI/180
            const ue_wrap::FVector vel{
                std::cos(yawRad) * curSpeed_,
                std::sin(yawRad) * curSpeed_,
                0.f,
            };
            E::SetMovementVelocity(satelliteCmc_, vel);
        }
    }

    // Head bone gets the source's full view direction: pitch (look up/down) AND
    // the camera-vs-body yaw lead (looking left/right with the camera leading
    // the body, including free-look). DriveAnimBP also overrides the AnimBP's
    // `lookingAtPlayer` to false every tick -- the kerfur AnimBP's default
    // graph re-writes that flag to TRUE each frame (track the LOCAL player),
    // which is glaringly wrong on a puppet (puppet's head twists to face the
    // observer's body, not where the SOURCE is looking). We turn off that
    // auto-track and drive headLookAt directly from the streamed view.
    Pup::DriveAnimBP(actor_, curSpeed_, curPitch_, curHeadYawDelta_);
}

bool RemotePlayer::SetLocation(const ue_wrap::FVector& location) {
    if (!valid()) { actor_ = nullptr; return false; }  // valid() = IsLive (not just non-null)
    return E::SetActorLocation(actor_, location);
}

ue_wrap::FVector RemotePlayer::GetLocation() const {
    if (!valid()) return {};  // never read a dying actor (PendingKill on level change)
    return E::GetActorLocation(actor_);
}

ue_wrap::FVector RemotePlayer::GetHeadPosition() const {
    if (!valid()) return {};
    // Anchor to the actual head BONE (the skeletal mesh renders offset from the actor
    // origin, so origin+Z sits to the side of the visible head). Falls back to the
    // actor location + a height offset if the bone can't be resolved.
    if (void* comp = Pup::GetSkeletalMeshComponent(actor_)) {
        ue_wrap::FVector head;
        if (E::GetHeadWorldLocation(comp, head)) {
            head.Z += 45.f;  // float above the crown
            return head;
        }
    }
    ue_wrap::FVector p = GetLocation();
    p.Z += 200.f;
    return p;
}

void RemotePlayer::SetNickname(std::wstring name) { nickname_ = std::move(name); }

}  // namespace coop

#include "coop/player/remote_player.h"

#include "coop/dev/puppet_head_probe.h"
#include "coop/player/client_model.h"
#include "coop/player/local_body.h"
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
    // 2026-05-25 v7 (audit fix): branch yaw by puppet kind ONLY. The Z
    // offset is now measured EMPIRICALLY post-spawn (see below), not
    // hardcoded -- the prior +halfH inference was vulnerable to BP-
    // mutation timing assumptions that we can't verify without inspecting
    // every BP graph. Measurement is RULE-1 robust against any future
    // change to VOTV's mainPlayer_C BP.
    //
    //   SkelMesh BACKUP:
    //     Yaw -- capture the mesh-frame shim (+Y-forward authoring vs
    //       +X-forward actor convention) from the local player's
    //       mesh_playerVisible.world.forward vs local.actor.Yaw. Typically
    //       -90 for VOTV's BP-authored mesh. Applied at the puppet's
    //       ACTOR transform because on SkelMeshActor the SMC IS the root.
    //
    //   MainPlayer DEFAULT:
    //     Yaw -- 0. The mainPlayer_C BP construction script applies the
    //       +Y-forward shim INSIDE mesh_playerVisible.RelRot.Yaw (= -90),
    //       so puppet.mesh.world.Yaw = puppet.actor.Yaw + (-90 from BP)
    //       = source.actor.Yaw + (-90 from BP) = source.mesh.world.Yaw.
    //       No actor-level offset needed; applying -90 here doubled the
    //       shim and faced the puppet 90 degrees off ("facing sideways"
    //       hands-on 2026-05-25).
    // Audit H9 (2026-05-27): MainPlayer is the only puppet kind now (RULE 2
    // retired the SkelMesh path). The +Y-forward shim lives INSIDE
    // mesh_playerVisible.RelRot.Yaw (-90) via mainPlayer_C's BP construction,
    // so puppet.actor.Yaw == source.actor.Yaw matches without an actor-level
    // offset.
    meshOffsetYaw_ = 0.f;
    // Z offset is measured empirically AFTER spawn (via the chain block
    // below); the preliminary value is 0.
    meshOffsetZ_ = 0.f;
    UE_LOGI("RemotePlayer::Spawn: meshOffsetYaw_=%.2f preliminary meshOffsetZ_=%.2f "
            "(MainPlayer puppet: yaw=0, Z empirical post-spawn)",
            meshOffsetYaw_, meshOffsetZ_);

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

    // 2026-05-25 v8 (hands-on fix): measure BOTH source's and puppet's
    // mesh chain composite to compute the offset. The prior v7 used only
    // puppet's chain delta and assumed source delta=0 (per an early-init
    // Z-trace) -- but the SETTLED source delta is actually -halfH (mesh
    // hangs below capsule by halfH). v7 over-compensated by halfH ->
    // puppet "afloat a lot" (user 2026-05-25). Dual measurement adapts
    // to whatever both ends actually have.
    //
    // Target: puppet.mesh.world.Z = source.mesh.world.Z (at wire-aligned
    // puppet.actor.Z = source.actor.Z + offset).
    //   puppet.mesh.world.Z = puppet.actor.Z + puppetChain
    //                       = source.actor.Z + offset + puppetChain
    //   source.mesh.world.Z = source.actor.Z + srcChain
    //   => offset = srcChain - puppetChain
    // Audit H9: MainPlayer is the only puppet kind; the conditional gate is
    // gone, the chain measurement block runs unconditionally for the
    // mainPlayer_C orphan.
    {
        // 2026-05-25 hands-on bug: the prior dual-chain measurement read
        // srcChain from local.mesh_playerVisible's world transform. That
        // transform is TRANSIENT during BP construction: on the CLIENT,
        // the local mainPlayer_C spawns moments before this Spawn() fires,
        // and mesh_playerVisible has not yet composed the -halfH offset
        // through its AttachParent chain (the 2026-05-23 Z-trace showed
        // an 84 cm swing in mesh.world.Z over the first ~2 seconds post-
        // teleport). During that unsettled window, srcMeshZ ~= srcActorZ
        // and srcChain ~= 0, instead of the settled -halfH. Result on the
        // client: meshOffsetZ_ = 0 - (-2*halfH) = +2*halfH = +170 instead
        // of +halfH = +85 -> host puppet floats 85 cm on the client's
        // screen. The host side was correct because the host had been
        // running its loaded save long enough for mesh.world.Z to settle.
        //
        // Fix: read the capsule HalfHeight directly. It's a static
        // authored float on the CapsuleComponent (CDO-set + BP-construction
        // overridable, but stable after BP CTOR completes -- which it
        // has, because SpawnActor returned). srcChain on a settled
        // mainPlayer_C is always -halfH (CMC floor-snap keeps mesh
        // world.Z at actor.Z - halfH); we don't need to MEASURE that
        // empirically when we can read it from a non-transient source.
        // puppet.cpp's GetSpawnMeshOffsetZ uses the same capsule-halfH
        // path for the SkelMesh backup; we now use it here too.
        void* puppetMesh = Pup::GetSkeletalMeshComponent(actor_);
        if (puppetMesh) {
            const float halfH       = E::GetActorCharacterHalfHeight(local);
            const float srcChain    = -halfH;  // -85 on VOTV's mainPlayer_C
            const float puppetMeshZ = E::GetComponentLocation(puppetMesh).Z;
            const float puppetActorZ= E::GetActorLocation(actor_).Z;
            const float puppetChain = puppetMeshZ - puppetActorZ;
            const float prev = meshOffsetZ_;
            meshOffsetZ_ = srcChain - puppetChain;
            UE_LOGI("RemotePlayer::Spawn: chain measure (halfH-anchored, symmetric) -- halfH=%.2f srcChain=%.2f puppet(meshZ=%.2f actorZ=%.2f chain=%.2f) -> meshOffsetZ_=%.2f (was %.2f)",
                    halfH, srcChain,
                    puppetMeshZ, puppetActorZ, puppetChain,
                    meshOffsetZ_, prev);
            // Apply corrected Z immediately so the first frame doesn't
            // show the puppet at the uncompensated position.
            ue_wrap::FVector liftedLoc = loc;
            liftedLoc.Z += meshOffsetZ_;
            E::SetActorLocation(actor_, liftedLoc);
        } else {
            UE_LOGW("RemotePlayer::Spawn: chain measure failed (puppetMesh=%p) -- meshOffsetZ_=0",
                    puppetMesh);
        }
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
    bodyYaw_.Reset(yaw);       // presentation yaw starts at the spawn facing
    targetPos_ = loc;
    targetYaw_ = yaw;
    targetPitch_ = 0.f;
    targetHeadYawDelta_ = 0.f;
    window_.Close();
    hasPose_ = false;  // the first network pose SNAPS away from this fake placement
    // Push the spawn placement (curPos_=actor.Z, curYaw_) to the engine NOW.
    // ApplyToEngine adds meshOffsetZ_ to curPos_.Z and meshOffsetYaw_ to
    // curYaw_, producing the same world transform as the SpawnActor placement
    // (= loc.Z + meshOffsetZ_). No visual pop between SpawnActor and the first
    // ApplyToEngine.
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
    // v22 ragdoll physics stream -- mechanism in remote_player_ragdoll.h. When the
    // pose applied to a live body, force ApplyToEngine to refresh the kel rotation
    // from the streamed pelvis next Tick.
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
    // dr_kel needs the pristine baseline; a custom skin resolves its own mesh.
    void* nativeMesh = coop::local_body::NativeBodyMesh();
    if (coop::client_model::ApplySkinToBody(actor_, skinName, nativeMesh))
        appliedSkin_ = skinName;
}

void RemotePlayer::ApplyToEngine() {
    // While the puppet is RAGDOLLED it is PELVIS-ATTACHED to the invisible ragdoll body,
    // so the engine syncs its transform per-frame -- do NOT pose-drive it here (a
    // SetActorLocation would fight the attachment); RagdollDisplay drives the kel's
    // rotation from the streamed pelvis instead. SetTargetPose keeps updating curPos_
    // from the wire meanwhile, so the first post-recover ApplyToEngine resumes from the
    // owner's current pose. StoppedNow = the body died under us (level-transition GC):
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

    // Wire convention + per-tick Z/yaw recipe (post-2026-05-25 puppet rework):
    //
    //   wire.z = source.actor.Z (stable capsule centre, MTA::CEntitySA::vPos
    //            shape -- the physics anchor, unaffected by BP init transients).
    //   puppet.actor.Z = wire.z + meshOffsetZ_
    //
    // meshOffsetZ_ carries DIFFERENT semantics per puppet kind (see Spawn):
    //   SkelMesh path:    meshOffsetZ_ = -halfH (the BP-authored
    //                     mesh.RelLoc.Z shim reconstructed at the actor
    //                     transform because the SMC IS the root).
    //   MainPlayer path:  meshOffsetZ_ = -(puppet.mesh.world.Z -
    //                     puppet.actor.Z) MEASURED EMPIRICALLY at Spawn.
    //                     Compensates for whatever chain composite the BP
    //                     produces on the puppet (which may differ from
    //                     the source if BP mutation timing differs).
    //
    // Net effect: puppet's mesh comp lands at the SETTLED source.mesh.world.Z
    // (target = source.actor.Z per Z-trace delta=0 on local).
    //
    // Yaw: also captured once at Spawn -- 0 on the MainPlayer path (BP
    // applies the mesh-frame shim INSIDE mesh.RelRot.Yaw); mesh-frame
    // angle (typically -90) on the SkelMesh path.
    ue_wrap::FVector puppetLoc = curPos_;
    puppetLoc.Z += meshOffsetZ_;
    E::SetActorLocation(actor_, puppetLoc);
    // bodyYaw_ (presentation, coop/puppet_body_yaw.h) -- NOT curYaw_ (wire
    // truth): while standing the body holds so the head can lead.
    E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, bodyYaw_.Yaw() + meshOffsetYaw_, 0.f});

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
                    bodyYaw_.Yaw() + meshOffsetYaw_, curYaw_ + curHeadYawDelta_);
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
        // interp position is the actor's location this frame (cur_).
        footsteps_.Tick(actor_, curPos_, curSpeed_, !inAir);
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
    // 2026-05-25 NIGHT (user retest +2): pre-fix this read the puppet's 'head'
    // BONE world location, which the AnimBP recomputes every animation tick.
    // The nameplate quad was repositioned via SetActorLocation on a separate
    // floating actor each tick; if nameplate::Update fired BEFORE the AnimBP
    // advanced the bone for the frame, the nameplate sat at the previous
    // frame's head position while the visible mesh had already moved -- the
    // user reported it as "the nameplate is trying to catch up with the
    // movement... jaggy laggy". The bone read also accumulated IK perturbation
    // (Control Rig nudges head transform during walk).
    //
    // Fix: anchor to the ACTOR pivot + a small fixed Z offset. The pivot is
    // moved by our pose drive (RemotePlayer::ApplyToEngine, same tick as the
    // pose update) so it stays in lockstep with the visible mesh. The
    // bone-anchor branch is removed per RULE 2 (no parallel old + new code
    // paths).
    //
    // Anchor Z to the RENDERED 'head' bone so the label tracks the actual head --
    // dynamic across crouch (ctrl) / jump / skin height -- not a fixed actor-pivot
    // offset. The puppet renders on the kerfur skeleton (player skin retargeted), whose
    // actor pivot sits ~30 cm ABOVE the 'head' bone, so the old `actor.Z + 30` floated
    // the label ~60 cm too high (user: "way too high"). Measured 2026-06-07:
    // actor.Z=6271, 'head' bone Z=6242, mesh root=6101.
    //
    // The 2026-05-25 version ALSO read the head bone but the nameplate was a SEPARATE
    // floating actor repositioned a frame late -> "jaggy laggy". That skew is gone: the
    // ImGui projection reads the bone + projects it in the SAME nameplate::Update tick.
    // X/Y stay on the stable actor pivot (no horizontal bone jitter).
    ue_wrap::FVector p = GetLocation();  // actor pivot X/Y (stable body centre)
    // The 'head' bone read iterates the skeleton -- too costly EVERY frame * every
    // puppet -- so refresh the head-Z OFFSET (relative to the actor pivot) only every
    // few ticks + reuse it between. Jump/move track exactly (the offset is actor-
    // relative, both move together); a crouch's lower head lands within ~80 ms.
    if (--headZRefresh_ <= 0) {
        headZRefresh_ = 5;  // ~12 Hz at the 60 Hz tick
        void* mesh = Pup::GetSkeletalMeshComponent(actor_);
        float headZ = 0.f;
        if (mesh && R::IsLive(mesh) && E::GetBoneWorldZByName(mesh, L"head", headZ)) {
            headZOffset_ = (headZ + 33.f) - p.Z;  // 'head' bone (+33 above) relative to actor.Z
        }
    }
    p.Z += headZOffset_;  // default +30 until the first bone read resolves
    return p;
}

void RemotePlayer::SetNickname(std::wstring name) { nickname_ = std::move(name); }

}  // namespace coop

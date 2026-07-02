// coop/remote_player.h -- the network-driven remote player.
//
// Gameplay/network layer (principle 7). Parallel class hierarchy (principle 3):
// RemotePlayer owns the network/coop state and a POINTER to the engine entity
// it renders through. The engine actor is a `mainPlayer_C` orphan spawned with
// inertPawn=true; the class's built-in mesh_playerVisible carries the player
// body skin + IK leg bones. Per-screen systems (PostProcess + mic + arms) are
// stripped at spawn, GameMode pointer nulled, CMC tick disabled, actor tick
// disabled. The puppet's AnimBP reads its OWN CMC.Velocity + MovementMode
// natively for locomotion + IK (v2 anim drive, 2026-05-27): RemotePlayer::
// ApplyToEngine writes those CMC fields each tick from the streamed pose,
// exactly mirroring what the LOCAL player's possessed CMC does. IK legs
// work because the real Character has floor-trace context.
//
// The engine actor owns rendering/animation; RemotePlayer owns the pose data.
// MTA shape: CClientPed::m_pPlayerPed -> CPlayerPed*.
//
// No marshaling/engine-memory access lives here; it all goes through ue_wrap.

#pragma once

#include "coop/interactables/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/player/puppet_body_yaw.h"
#include "coop/player/puppet_footsteps.h"
#include "coop/player/remote_player_ragdoll.h"
#include "ue_wrap/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop {

class RemotePlayer {
public:
    // Spawn the puppet in the live world, offset from the local player, wearing
    // the local player's skin. Requires gameplay loaded (mainPlayer_C present)
    // and the game thread. Returns true on success; sets actor().
    //
    // useClientModel: this puppet represents a remote CLIENT peer (spawn site
    // passes slot != host). When true AND the custom-client-mesh pak is present,
    // the puppet wears the custom body mesh (coop::client_model) instead of the
    // local kel skin; the AnimClass stays the local anthro AnimBP (same skeleton,
    // drives the custom mesh 1:1). False (host puppet / no pak) -> kel skin.
    bool Spawn(bool useClientModel = false);

    bool valid() const;

    // Receiver-side interpolation, MTA shape (research/findings/mta-pose-interpolation-
    // 2026-05-23.md). The network path is two calls on the game thread:
    //
    //   SetTargetPose(snap)   on each NEW pose from the wire (skip duplicates).
    //                         Computes error from the current applied pose and
    //                         opens a kInterpWindowMs window to walk toward it,
    //                         OR snaps if the error exceeds the snap threshold
    //                         (real teleport / door warp), OR snaps the FIRST
    //                         packet (the spawn placeholder is fake).
    //
    //   Tick()                every game-thread tick. Advances the interpolation
    //                         (incremental LERP per MTA: dAlpha * error each
    //                         frame, freezes at alpha=1 until the next packet)
    //                         and applies the current pose to the engine. Idle
    //                         calls when there's no interp budget are cheap (it
    //                         re-applies the current pose).
    //
    // Speed is NOT interpolated -- the AnimBP locomotion blend smooths it.
    void SetTargetPose(const coop::net::PoseSnapshot& snap);
    void Tick();

    // v22 ragdoll PHYSICS sync. Called on each fresh RagdollPose packet for this
    // peer (net_pump per-slot drive) WHILE the peer is ragdolling. Slaves the mirror
    // ragdoll body's pelvis velocity to the sender's real ragdoll (so it tumbles to
    // track, not free-simulate) + stamps the streamed pelvis rotation that the next
    // ApplyToEngine drives onto the kel. No-op until a body exists (ragdoll active).
    // Game thread only.
    void SetRagdollPose(const coop::net::RagdollPoseSnapshot& snap);

    // Tear down the puppet: DestroyActor on the engine SkeletalMeshActor and
    // unregister the floating nameplate. Called on peer disconnect (Bye /
    // exit) -- otherwise the puppet lingers in the world frozen at its last
    // pose forever. The next remote pose (if the peer reconnects) re-spawns
    // a fresh puppet via the NetPumpTick auto-spawn path.
    void Destroy();

    // Apply an absolute position only (teleport, no sweep). Kept for the harness /
    // verification; the network path is SetTargetPose() + Tick().
    bool SetLocation(const ue_wrap::FVector& location);

    // Current engine-reported location (for verification / interpolation base).
    ue_wrap::FVector GetLocation() const;

    // World point to anchor the floating nameplate: puppet actor pivot
    // (which the pose drive aligns with the RENDERED head crown via
    // meshOffsetZ_) + a small Z offset to float just above the head.
    ue_wrap::FVector GetHeadPosition() const;

    // Unit forward vector from the puppet's SYNCED aim (curYaw_+curHeadYawDelta_, curPitch_) --
    // the SAME convention DriveHeadLookAtWorld uses, so it points where the puppet looks. Used by
    // puppet_carry_drive to position a puppet-held trash clump at the hand (the puppet's own tick
    // does NOT drive the PHC -- votv-puppet-grab-feasibility-RE-2026-06-22). Game thread.
    ue_wrap::FVector GetSyncedAimDirection() const;

    // The raw engine puppet actor pointer (mainPlayer_C or ASkeletalMeshActor
    // depending on puppet kind). Used by nameplate::Update to exclude
    // puppet actors from its "find the local viewer" GUObjectArray scan --
    // with the mainPlayer_C puppet path both local and puppet share the
    // same UClass, so the scan needs a way to distinguish them. Returns
    // nullptr if no puppet is spawned yet. Game thread only.
    void* GetActor() const { return actor_; }

    // The display nickname rendered above the body (set from the network
    // handshake; defaults to "..." until the peer's Join reliable msg arrives).
    void SetNickname(std::wstring name);
    const std::wstring& GetNickname() const { return nickname_; }

    // Current RTT to this player's source, in milliseconds. -1 = unmeasured,
    // 0 = sub-millisecond LAN. The nameplate appends "(<ping>ms)" (">=1") or
    // "(<1ms)" (==0) and nothing when -1; event_feed forwards this peer's OWN
    // per-slot RTT from Session::rttMsForSlot each tick.
    void SetPing(int ms) { pingMs_ = ms; }
    int GetPing() const { return pingMs_; }

    // v19 vitals (display-only). Each is a [0,1] fraction streamed in every
    // PoseSnapshot (health = source health/maxHealth; food/sleep = source value
    // / kVitalScalarMax). SetVitals is called from SetTargetPose on each new
    // pose; the nameplate reads GetHealth() to draw a health bar. These NEVER
    // touch a saveSlot -- there is one saveSlot per machine, so a puppet write
    // would corrupt the LOCAL player's persisted health (see ue_wrap::vitals).
    // v20 Inc3: SetVitals also EDGE-DETECTS a health DECREASE (this peer took
    // damage) and arms a hurt-flash (the nameplate flashes red) -- no new wire,
    // it rides the existing health stream (MTA CPedSync gate-on-change shape).
    // Defined in the .cpp (needs NowMs + nameplate). Game thread only.
    void SetVitals(float health01, float food01, float sleep01);
    float GetHealth() const { return health_; }
    float GetFood() const { return food_; }
    float GetSleep() const { return sleep_; }
    // True while the damage hurt-flash window is active (test/diagnostic seam).
    bool IsHurtFlashing() const { return hurtFlashActive_; }

    // True while a ragdoll display body is spawned (test/diagnostic seam -- the
    // ragdoll e2e observer checks this instead of the old own-mesh flop).
    bool IsRagdollDisplayed() const { return ragdoll_.Active(); }
    // The spawned playerRagdoll_C body + its GUObjectArray index (for an
    // IsLiveByIndex-guarded deref). For test verification only.
    void* RagdollBody() const { return ragdoll_.Body(); }
    int32_t RagdollBodyIdx() const { return ragdoll_.BodyIdx(); }

    void* actor() const { return actor_; }

private:
    // Apply curPos_/curYaw_/curSpeed_ to the engine actor + the AnimBP. Called by
    // Tick() once per frame (whether or not interp is active -- avoids drift if
    // some other system moved the actor between frames; cheap).
    void ApplyToEngine();

    // Advance the OPEN interp window to NOW (curPos_/curYaw_/... += dAlpha * cached
    // error). No-op when no window is open (window_.IsOpen() == false). Does NOT push
    // to the engine -- it only updates the cur* state + sets dirty_; the caller pushes.
    //
    // MTA shape (CClientPed::UpdateTargetPosition, reference/mtasa-blue): this is
    // called BOTH every frame from Tick() AND as the FIRST step of SetTargetPose().
    // The SetTargetPose pre-advance is load-bearing: poses arrive ~every frame (60 Hz),
    // so without it each new packet re-Open()s the window at now right before Tick()
    // reads alpha = (now - start)/window ~= 0 -> dAlpha ~= 0 -> curPos_ never
    // advances and the puppet trails a MOVING source by 1-2.5 s. Pre-advancing first
    // applies the motion already due, THEN rebases the error from the up-to-date pos
    // (MTA: SetTargetPosition's first line is UpdateTargetPosition()). Root-caused
    // 2026-06-06, see [[project-puppet-lag-interp-starvation]].
    void AdvanceInterp();

    // Linear LERP window + jitter buffer. At 60 Hz send (~16.7 ms interval) this is
    // ~4.5x the interval, tolerating ~4 consecutive late/dropped poses before the
    // puppet reaches target and freezes (a brief stall, not a snap).
    //
    // Steady-state trail (corrected 2026-06-06): with advance-before-rebase in place
    // (AdvanceInterp runs first in SetTargetPose), poses arriving every frame apply a
    // fraction P/W of the cached error per packet, giving a constant trail of
    // ~= speed * window: ~9 cm at a 120 cm/s walk, ~45 cm at a 600 cm/s sprint. So the
    // WINDOW is now the primary lag knob -- shrinking it cuts the trail linearly at the
    // cost of jitter tolerance. (The earlier "~25 ms steady-state" note was wrong: it
    // assumed the window COMPLETES each interval, which only holds when poses are spaced
    // >= W apart; at 60 Hz into a 75 ms window it never completes, so the trail is
    // speed*W, not the window-completion delay. That mis-estimate is what let the
    // pre-advance bug hide -- 75 ms looked harmless on paper.) Kept at 75 ms for this
    // fix so the smoke validates advance-before-rebase in isolation; tune down (or make
    // adaptive on avg recv interval) as a measured follow-up if sprint trail is felt.
    static constexpr int kInterpWindowMs = 75;
    // Snap thresholds (cm). At sprint ~600 cm/s in VOTV, one window's legal
    // motion is ~30 cm; anything more than (base + 0.5 s * speed) is a real
    // teleport (door warp, respawn) -- snap instead of trying to LERP across.
    static constexpr float kSnapBaseCm = 1000.f;
    static constexpr float kSnapPerSpeedSec = 0.5f;

    void* actor_ = nullptr;  // the engine puppet actor (owned by the engine):
                             // a mainPlayer_C orphan spawned by ue_wrap::puppet::SpawnPuppet.
                             // Its OWN UCharacterMovementComponent (at
                             // ACharacter::CharacterMovement @0x0288) is what
                             // we drive per-tick: ApplyToEngine writes
                             // puppet.CMC.Velocity @+0xC4 and MovementMode
                             // @+0x168 directly. CMC tick is parked
                             // (SetComponentTickEnabled false in puppet::
                             // SpawnPuppetMainPlayer) so nothing else touches
                             // those fields. BUA naturally reads them via
                             // Pawn.GetMovementComponent().Velocity -> spd
                             // and ...MovementMode -> useLegIK/rise gates,
                             // reproducing the LOCAL anim drive on the puppet.
                             // No satellite, no observer, no AnimInstance
                             // field overrides (post-2026-05-27 RE per
                             // research/findings/votv-local-anim-drive-RE-2026-05-27.md).

    // GUObjectArray InternalIndex of actor_, captured at Spawn while the puppet
    // was known live (via reflection::InternalIndexOf). valid() validates actor_
    // with reflection::IsLiveByIndex(actor_, internalIdx_) -- NOT plain IsLive.
    // Why: a mass GC purge can free the puppet WITHOUT firing our Destroy path,
    // then recycle its address/slot to a smaller foreign UObject. Plain IsLive
    // reads the index FROM the object's own memory, so it returns true for that
    // recycled impostor; ApplyToEngine then raw-derefs actor_ at large
    // mainPlayer_C offsets (CMC @0x288, mesh @0x4F8, lag_fl @0x670) and AVs --
    // and builds a ParamFrame on its garbage UFunction*, ballooning RSS. Gating
    // on the CACHED index closes the recycling hole (the slot-identity compare
    // rejects the impostor). See [[feedback-islive-unsafe-on-freed-cached-pointer]]
    // (root-caused 2026-05-30 from the 4-peer smoke: per-tick AV flood + 11 GB
    // balloon on a client). -1 = no puppet (valid() short-circuits on actor_==null).
    int32_t internalIdx_ = -1;

    // Both offsets are captured ONCE at Spawn from the RECEIVER's own local
    // mainPlayer_C -- same BP class as the source, so the BP-authored RelLoc
    // values are identical on every peer (BP construction sets the same
    // constants on every instance). Read raw from the static USceneComponent
    // fields (RelativeLocation @ +0x011C, RelativeRotation @ +0x0128) NOT via
    // K2_GetComponentLocation / K2_GetComponentForwardVector -- the world-
    // transform UFunctions return the COMPUTED world transform which during
    // BP construction + save-load init is transiently non-settled (Z-trace
    // 2026-05-23 captured an 84cm swing in mesh.world.Z over the first 2 sec
    // post-teleport). The RAW field carries the settled value the BP
    // construction script writes.
    //
    // Z fix history (Option G shipped 2026-05-23 evening per code-architect
    // RULE 1 verdict + MTA fidelity): the prior chain 4407fa7 -> 9765ad2 ->
    // bfa91d2 -> c8e192b -> 195dd72 -> 'stream mesh.world.Z' all failed for
    // ONE reason: they used either (a) a guessed offset (-halfH) or (b) a
    // computed world transform delta that varies with transient init state.
    // Option G streams source.actor.Z (stable capsule centre, MTA's wire
    // shape) and applies meshOffsetZ_ from a stable raw-field read of the
    // LOCAL's BP-authored RelativeLocation.Z. Identical on every instance,
    // robust to source-side init transients (puppet doesn't see them
    // because the wire no longer carries them).
    float meshOffsetZ_ = 0.f;    // = local mesh_playerVisible.RelativeLocation.Z (raw, +0x11C)
    float meshOffsetYaw_ = 0.f;  // = atan2(mesh forward) - actor yaw (BP-authored mesh frame shim)
    // Nameplate head anchor: cached 'head' bone Z relative to the actor pivot,
    // refreshed every few ticks in GetHeadPosition (the bone read iterates the
    // skeleton -- too costly per frame * per puppet). Default +30 until first resolve.
    mutable float headZOffset_ = 30.f;
    mutable int   headZRefresh_ = 0;
    // Placeholder until the peer's Join reliable message lands (typically within
    // a few RTT of connect). nameplate::Update repaints when SetNickname changes
    // this. The old "Player 2" default was misleading -- both ends saw "Player 2"
    // forever in early tests because no refresh path existed.
    std::wstring nickname_ = L"...";
    int pingMs_ = -1;  // per-peer RTT ms (event_feed sets it from Session::rttMsForSlot); -1 = not measured
    // v19 streamed vitals fractions (display-only; see SetVitals). Default full
    // so a freshly-spawned puppet shows a full bar until the first pose lands.
    float health_ = 1.f;
    float food_ = 1.f;
    float sleep_ = 1.f;
    // v20 Inc3 damage hurt-flash (nameplate red). Timestamp-based (NowMs deadline)
    // not frame-count, so the duration is FPS-independent. hurtFlashEndMs_ is armed
    // on a detected health DROP in SetVitals (latest-hit-wins); Tick toggles the
    // nameplate color on the edge of (NowMs < hurtFlashEndMs_) vs hurtFlashActive_
    // (one repaint per transition, no per-frame widget churn). Both reset in Destroy.
    uint64_t hurtFlashEndMs_ = 0;
    bool     hurtFlashActive_ = false;
    static constexpr uint64_t kHurtFlashMs = 500;     // red for ~0.5 s per hit
    static constexpr float    kHurtEpsilon = 0.006f;  // > 1 wire quantization step (1/255) -- ignore dequant jitter
    // Inc3 body pulse: the puppet's original materials (both visible meshes),
    // cached on the flash rising edge (engine swaps them to the gore-red skin) +
    // restored on the falling edge. Each entry is (component, slot, original).
    std::vector<ue_wrap::SavedMaterial> hurtSavedMaterials_;

    // Receiver-side interpolation state (game thread only -- the engine path is
    // single-threaded; no mutex). curPos_/curYaw_/curSpeed_ is what was last
    // applied to the engine; targetPos_/targetYaw_ is what we're walking toward.
    ue_wrap::FVector curPos_{};
    float            curYaw_ = 0.f;       // source's ACTOR yaw (body facing).
    float            curPitch_ = 0.f;     // source controller pitch (drives head bone)
    float            curHeadYawDelta_ = 0.f;  // source controller.yaw - actor.yaw; drives the
                                              // head bone yaw lead over the body so the
                                              // puppet's head visually follows where the
                                              // SOURCE is looking (free-look / camera lead).
    float            curSpeed_ = 0.f;
    // Presentation body yaw (turn-in-place synthesis) -- what SetActorRotation
    // actually shows; curYaw_ stays the WIRE truth. Reset on spawn / first
    // packet / teleport snap / ragdoll recover; advanced each Tick. Full
    // design doc + constants live in coop/puppet_body_yaw.h.
    coop::puppet_body_yaw::State bodyYaw_{};
    // Footstep stride emitter (2026-06-10): walks the interp displacement and
    // dispatches the game's own lib_C::step every native stride -- the
    // puppet's BP tick (where the native accumulator lives) is suppressed.
    coop::puppet_footsteps::Stride footsteps_{};
    // v8 state flags (kStateBitInAir + reserved bits). NOT interpolated --
    // snapped on every SetTargetPose. Read by ApplyToEngine to drive the
    // puppet's CMC.MovementMode each tick: bit 0 set -> MOVE_Falling=3,
    // clear -> MOVE_Walking=1. The kerfur AnimBP reads MovementMode
    // natively to gate the foot-IK alpha (useLegIK/rise).
    uint8_t          curStateBits_ = 0;
    // Ragdoll display (2026-06-01 xray-actor rework, [[project-ragdoll-sync]]):
    // the whole lifecycle (wire-bit edge -> spawn invisible playerRagdoll_C +
    // pelvis-attach; v22 pelvis stream; attached-transform drive; teardown) lives
    // in coop/player/remote_player_ragdoll.h. Glue side effects stay here, keyed
    // off its return values: OnWireBit/DriveAttached stop -> bodyYaw_.Reset;
    // SetPose applied -> dirty_. Torn down in Destroy() (body only).
    RagdollDisplay   ragdoll_;
    ue_wrap::FVector targetPos_{};
    float            targetYaw_ = 0.f;
    float            targetPitch_ = 0.f;
    float            targetHeadYawDelta_ = 0.f;
    // Cached at SetTargetPose time = target - cur. The interp incrementally
    // applies dAlpha * errorPos_ each frame (MTA's linear form -- recomputing
    // (target - cur) each frame would decay geometrically, not linearly).
    ue_wrap::FVector errorPos_{};
    float            errorYaw_ = 0.f;
    float            errorPitch_ = 0.f;
    float            errorHeadYawDelta_ = 0.f;
    // The interp TIMING (start/finish/lastAlpha + the alpha/dAlpha bookkeeping)
    // lives in the shared coop::LerpWindow (RULE 2 / WP13 -- element::Npc owns
    // one too). AdvanceInterp applies the window's dAlpha to errorPos_/errorYaw_/
    // errorPitch_/errorHeadYawDelta_ above; SetTargetPose Open()s it / Close()s on
    // a snap.
    coop::LerpWindow window_;
    bool             hasPose_ = false;  // first packet snaps; spawn placeholder is fake
    bool             dirty_ = true;     // true while there's an unapplied change to push to the engine
};

}  // namespace coop

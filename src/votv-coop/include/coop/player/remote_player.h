// coop/remote_player.h -- the network-driven remote player. RemotePlayer owns the network state
// (the streamed pose, the interpolation, the vitals) and a pointer to the engine actor it
// renders through: a mainPlayer_C orphan spawned inert, its per-screen systems stripped, its
// GameMode pointer nulled and its actor and movement ticks disabled. The puppet's AnimBP reads
// its own movement component's Velocity and MovementMode for locomotion and leg IK, and
// ApplyToEngine writes those fields each tick from the streamed pose, as the local player's
// possessed component does. Engine memory is reached only through ue_wrap.

#pragma once

#include "coop/element/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/player/puppet_body_yaw.h"
#include "coop/player/puppet_footsteps.h"
#include "coop/player/remote_player_ragdoll.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop {

class RemotePlayer {
public:
    // Spawn the puppet in the live world, placed by the local player. Needs gameplay loaded and the
    // game thread; sets actor() on success. `skinName` is the body skin this peer announced: empty,
    // "dr_kel" or an unresolvable pak gives the pristine kel baseline (local_body::NativeBodyMesh,
    // not the local pawn's live mesh, which may itself be skin-swapped). A custom skin rides the
    // same skeleton, so the local AnimClass drives it. A skin that lands after spawn is applied
    // live by ApplySkin.
    bool Spawn(const std::string& skinName = std::string());

    // Re-skin a live puppet (a mid-session SkinChange, or a Join that raced the first pose). A
    // no-op when the puppet is not spawned (the skin is per-slot state in player_handshake, read by
    // the next Spawn) or the name is already applied. Game thread only.
    void ApplySkin(const std::string& skinName);

    bool valid() const;

    // Receiver-side interpolation, two calls on the game thread. SetTargetPose on each new pose
    // from the wire: computes the error from the current applied pose and opens a kInterpWindowMs
    // window to walk toward it, or snaps when the error exceeds the snap threshold (a teleport) or
    // on the first packet (the spawn placement is a placeholder). Tick every game-thread tick:
    // advances the interpolation (dAlpha times the cached error per frame, frozen at alpha 1 until
    // the next packet) and applies the pose to the engine. Speed is not interpolated; the AnimBP's
    // locomotion blend smooths it.
    void SetTargetPose(const coop::net::PoseSnapshot& snap);
    void Tick();

    // Ragdoll physics sync, on each fresh RagdollPose packet while the peer ragdolls: slaves the
    // mirror body's pelvis velocity to the sender's ragdoll and stamps the streamed pelvis rotation
    // the next ApplyToEngine drives. A no-op until a ragdoll body exists. Game thread only.
    void SetRagdollPose(const coop::net::RagdollPoseSnapshot& snap);

    // Tear down the puppet (DestroyActor on the engine actor) and its nameplate. Called on peer
    // disconnect; the next remote pose re-spawns a fresh puppet through the pump's auto-spawn path.
    void Destroy();

    // An absolute position only (a teleport, no sweep), for the harness; the network path is
    // SetTargetPose and Tick.
    bool SetLocation(const ue_wrap::FVector& location);

    // The engine-reported location.
    ue_wrap::FVector GetLocation() const;

    // The world point the nameplate and the voice speaker anchor to: the head bone of the mesh the
    // peer is rendered by (the ragdoll body while ragdolled, the skin mesh otherwise), lifted above
    // the skull, its height low-pass filtered (about 70 ms; teleports snap).
    ue_wrap::FVector GetHeadPosition() const;

    // The unit forward vector of the puppet's synced aim (curYaw_ plus curHeadYawDelta_,
    // curPitch_), the convention DriveHeadLookAtWorld uses, so it points where the puppet looks.
    // Positions a puppet-held item at the hand, since the puppet's own tick does not drive it. Game
    // thread.
    ue_wrap::FVector GetSyncedAimDirection() const;

    // The raw engine puppet actor, nullptr before Spawn. Game thread only.
    void* GetActor() const { return actor_; }

    // The nickname rendered above the body, from the handshake; "..." until the peer's Join lands.
    void SetNickname(std::wstring name);
    const std::wstring& GetNickname() const { return nickname_; }

    // Display-only vitals, each a [0,1] fraction streamed in every PoseSnapshot (health over
    // maxHealth; food and sleep over kVitalScalarMax). Set from SetTargetPose; the nameplate reads
    // GetHealth for its bar. They never touch a saveSlot, since a puppet write would corrupt the
    // local player's persisted health. SetVitals also edge-detects a health decrease and arms the
    // hurt flash. Game thread only.
    void SetVitals(float health01, float food01, float sleep01);
    float GetHealth() const { return health_; }
    float GetFood() const { return food_; }
    float GetSleep() const { return sleep_; }
    // True while the hurt-flash window is active (a test seam).
    bool IsHurtFlashing() const { return hurtFlashActive_; }

    // True while a ragdoll display body is spawned (a test seam).
    bool IsRagdollDisplayed() const { return ragdoll_.Active(); }
    // The spawned playerRagdoll_C body and its GUObjectArray index, for a liveness-guarded deref in
    // tests.
    void* RagdollBody() const { return ragdoll_.Body(); }
    int32_t RagdollBodyIdx() const { return ragdoll_.BodyIdx(); }

    void* actor() const { return actor_; }

private:
    // Apply curPos_, curYaw_ and curSpeed_ to the engine actor and the AnimBP. Called by Tick once
    // per frame whether or not interpolation is active, so nothing that moved the actor between
    // frames leaves drift.
    void ApplyToEngine();

    // Advance the open interpolation window to now (the cur* state gains dAlpha times the cached
    // error); a no-op with no window open. Sets dirty_ and does not push to the engine. Called
    // every frame from Tick and as the first step of SetTargetPose: poses arrive about every frame,
    // and without the pre-advance each packet would re-open the window right before Tick read an
    // alpha near zero, so the puppet would trail a moving source by seconds. The motion already due
    // is applied first, then the error is rebased from the up-to-date position.
    void AdvanceInterp();

    // The interpolation window, about 4.5 send intervals at 60 Hz, so four consecutive late or
    // dropped poses stall the puppet briefly rather than snap it. With poses arriving every frame
    // the window never completes, and the steady-state trail is speed times window: about 9 cm at
    // a walk, 45 cm at a sprint. The window is the lag knob; shrinking it cuts the trail linearly
    // at the cost of jitter tolerance.
    static constexpr int kInterpWindowMs = 75;
    // Snap thresholds (cm): one window's legal motion at a sprint is about 30 cm, so anything past
    // base plus half a second of speed is a real teleport and is snapped rather than walked.
    static constexpr float kSnapBaseCm = 1000.f;
    static constexpr float kSnapPerSpeedSec = 0.5f;

    void* actor_ = nullptr;  // the engine puppet actor, a mainPlayer_C orphan; owned by the engine

    // The GUObjectArray InternalIndex of actor_, captured at Spawn while the puppet was known live;
    // valid() checks IsLiveByIndex, never plain IsLive. A mass GC purge can free the puppet without
    // our Destroy path running and recycle its slot to a smaller object; plain IsLive reads the
    // index from the object's own memory and passes on that impostor, and ApplyToEngine then
    // dereferences it at mainPlayer_C offsets. -1 means no puppet.
    int32_t internalIdx_ = -1;

    // The puppet actor sits at the wire pose with no offset: both ends are mainPlayer_C, so their
    // settled component chains are identical by class. Spawn logs the live chain as a drift
    // diagnostic only.
    // The head anchor for the nameplate and voice: the head bone of the rendered mesh. X and Y are
    // raw; only the height runs through the low-pass (head bob, crouch blends, the flop), and
    // teleports snap. Mutable, since the filter advances inside a const getter, keyed to real
    // elapsed time so multiple callers per tick are idempotent.
    mutable float    headAnchorZ_ = 0.f;
    mutable uint64_t headAnchorAtMs_ = 0;
    // The placeholder until the peer's Join lands; the nameplate repaints when SetNickname changes
    // it.
    std::wstring nickname_ = L"...";
    // The skin applied to actor_ (ApplySkin no-ops on a repeat; reset with the actor in Destroy).
    // Empty is the native kel.
    std::string appliedSkin_;
    // The streamed vitals fractions, full by default so a fresh puppet shows a full bar until the
    // first pose.
    float health_ = 1.f;
    float food_ = 1.f;
    float sleep_ = 1.f;
    // The hurt flash: a deadline in ms rather than a frame count, so its length is FPS-independent.
    // Armed on a detected health drop in SetVitals (the latest hit wins); Tick toggles the
    // nameplate colour and the body material on the edge of the deadline, one repaint per
    // transition. Both reset in Destroy.
    uint64_t hurtFlashEndMs_ = 0;
    bool     hurtFlashActive_ = false;
    static constexpr uint64_t kHurtFlashMs = 500;     // red for about half a second per hit
    static constexpr float    kHurtEpsilon = 0.006f;  // over one wire quantisation step (1/255), so dequantisation jitter is ignored
    // The puppet's original materials on both visible meshes, saved on the flash's rising edge
    // (the engine's hurt material replaces them) and restored on the falling edge.
    std::vector<ue_wrap::SavedMaterial> hurtSavedMaterials_;

    // The interpolation state, game thread only. cur* is what was last applied to the engine,
    // target* what it walks toward.
    ue_wrap::FVector curPos_{};
    float            curYaw_ = 0.f;       // the source's actor yaw (body facing)
    float            curPitch_ = 0.f;     // the source's controller pitch (drives the head bone)
    float            curHeadYawDelta_ = 0.f;  // controller yaw minus actor yaw: the head's lead over the body
    float            curSpeed_ = 0.f;
    // The presentation body yaw (turn-in-place synthesis), what SetActorRotation shows; curYaw_
    // stays the wire truth. Reset on spawn, the first packet, a snap and a ragdoll recover;
    // advanced each Tick. See coop/player/puppet_body_yaw.h.
    coop::puppet_body_yaw::State bodyYaw_{};
    // The footstep stride emitter: walks the interpolated displacement and dispatches the game's
    // own step every native stride, since the puppet's BP tick (where the game's accumulator lives)
    // is suppressed.
    coop::puppet_footsteps::Stride footsteps_{};
    // The state bits (kStateBitInAir and reserved bits), snapped on every SetTargetPose, never
    // interpolated. ApplyToEngine drives the movement mode from bit 0: set is falling, clear is
    // walking, which the AnimBP reads to gate the foot IK.
    uint8_t          curStateBits_ = 0;
    // The ragdoll display. Its whole lifecycle (the wire-bit edge spawning an invisible
    // playerRagdoll_C attached at the pelvis, the pelvis stream, the attached-transform drive, the
    // teardown) lives in coop/player/remote_player_ragdoll.h; the glue here keys off its returns
    // (a stop resets bodyYaw_, an applied pose sets dirty_). Torn down in Destroy.
    RagdollDisplay   ragdoll_;
    ue_wrap::FVector targetPos_{};
    float            targetYaw_ = 0.f;
    float            targetPitch_ = 0.f;
    float            targetHeadYawDelta_ = 0.f;
    // Cached at SetTargetPose as target minus cur. The interpolation applies dAlpha times this each
    // frame; recomputing target minus cur per frame would decay geometrically, not linearly.
    ue_wrap::FVector errorPos_{};
    float            errorYaw_ = 0.f;
    float            errorPitch_ = 0.f;
    float            errorHeadYawDelta_ = 0.f;
    // The window timing lives in the shared coop::LerpWindow (element::Npc owns one too).
    // AdvanceInterp applies its dAlpha to the errors above; SetTargetPose opens it and closes it on
    // a snap.
    coop::LerpWindow window_;
    bool             hasPose_ = false;  // first packet snaps; spawn placeholder is fake
    bool             dirty_ = true;     // true while there's an unapplied change to push to the engine
};

}  // namespace coop

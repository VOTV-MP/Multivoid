// coop/remote_player.h -- the network-driven remote player.
//
// Gameplay/network layer (principle 7). Parallel class hierarchy (principle 3):
// RemotePlayer owns the network/coop state and a POINTER to the engine entity it
// renders through -- a "skin-puppet": a bare ASkeletalMeshActor wearing the local
// player's exact skin (mesh_playerVisible) + the body AnimBP, which WE drive
// entirely (no pawn, no controller, no CharacterMovement). The engine actor owns
// rendering/animation; RemotePlayer owns the pose data. (MTA shape:
// CClientPed::m_pPlayerPed -> CPlayerPed*.)
//
// Why a puppet, not a 2nd mainPlayer_C: the protagonist class dragged its whole
// single-player tick surface along (the hijack/gamma cascade) and its AnimBP
// posed as a "stick" without a possessing controller. The puppet poses purely
// from AnimBP variables we set (root-cause finding, two converging agents) and
// has no hijack surface (RULE 1 root-cause; RULE 3 parallel hierarchy).
//
// No marshaling/engine-memory access lives here; it all goes through ue_wrap.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/types.h"

#include <cstdint>
#include <string>

namespace coop {

class RemotePlayer {
public:
    // Spawn the puppet in the live world, offset from the local player, wearing
    // the local player's skin. Requires gameplay loaded (mainPlayer_C present)
    // and the game thread. Returns true on success; sets actor().
    bool Spawn();

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

    // Clear the "we have a target" state so the NEXT SetTargetPose snaps (no
    // LERPing across the disconnect interval). Call when the peer goes from
    // Connected -> not-Connected (Bye / timeout) -- the puppet stays where
    // it is, but the first pose after reconnect teleports rather than sliding.
    void ResetPoseState();

    // Apply an absolute position only (teleport, no sweep). Kept for the harness /
    // verification; the network path is SetTargetPose() + Tick().
    bool SetLocation(const ue_wrap::FVector& location);

    // Set horizontal facing independently of where the local player looks.
    bool SetFacing(float yaw);

    // Current engine-reported location (for verification / interpolation base).
    ue_wrap::FVector GetLocation() const;

    // World point to anchor the floating nameplate: actor location (feet) + a Z
    // offset to float just above the head.
    ue_wrap::FVector GetHeadPosition() const;

    // The display nickname rendered above the body (set from the network
    // handshake; defaults to "Player 2" so the harness shows something).
    void SetNickname(std::wstring name);
    const std::wstring& GetNickname() const { return nickname_; }

    void* actor() const { return actor_; }

private:
    // Apply curPos_/curYaw_/curSpeed_ to the engine actor + the AnimBP. Called by
    // Tick() once per frame (whether or not interp is active -- avoids drift if
    // some other system moved the actor between frames; cheap).
    void ApplyToEngine();

    // Linear LERP window for new poses. ~1.5x the 30 Hz send interval (33 ms);
    // so the puppet is still ~30% short of the previous target when the next
    // packet arrives, smoothing jitter without an explicit render-delay buffer.
    static constexpr int kInterpWindowMs = 50;
    // Snap thresholds (cm). At sprint ~600 cm/s in VOTV, one window's legal
    // motion is ~30 cm; anything more than (base + 0.5 s * speed) is a real
    // teleport (door warp, respawn) -- snap instead of trying to LERP across.
    static constexpr float kSnapBaseCm = 1000.f;
    static constexpr float kSnapPerSpeedSec = 0.5f;

    void* actor_ = nullptr;  // the engine ASkeletalMeshActor puppet (owned by the engine)
    std::wstring nickname_ = L"Player 2";

    // Receiver-side interpolation state (game thread only -- the engine path is
    // single-threaded; no mutex). curPos_/curYaw_/curSpeed_ is what was last
    // applied to the engine; targetPos_/targetYaw_ is what we're walking toward.
    ue_wrap::FVector curPos_{};
    float            curYaw_ = 0.f;
    float            curSpeed_ = 0.f;
    ue_wrap::FVector targetPos_{};
    float            targetYaw_ = 0.f;
    // Cached at SetTargetPose time = target - cur. The interp incrementally
    // applies dAlpha * errorPos_ each frame (MTA's linear form -- recomputing
    // (target - cur) each frame would decay geometrically, not linearly).
    ue_wrap::FVector errorPos_{};
    float            errorYaw_ = 0.f;
    uint64_t         interpStartMs_ = 0;
    uint64_t         interpFinishMs_ = 0;  // 0 == no active interp window (frozen)
    float            lastAlpha_ = 0.f;
    bool             hasPose_ = false;  // first packet snaps; spawn placeholder is fake
    bool             dirty_ = true;     // true while there's an unapplied change to push to the engine
};

}  // namespace coop

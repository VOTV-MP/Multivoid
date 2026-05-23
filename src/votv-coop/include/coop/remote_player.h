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

    // World point to anchor the floating nameplate: actor location (feet) + a Z
    // offset to float just above the head.
    ue_wrap::FVector GetHeadPosition() const;

    // The display nickname rendered above the body (set from the network
    // handshake; defaults to "..." until the peer's Join reliable msg arrives).
    void SetNickname(std::wstring name);
    const std::wstring& GetNickname() const { return nickname_; }

    // Current RTT to this player's source, in milliseconds. 0 = unmeasured.
    // The nameplate appends "(<ping>ms)" when this is non-zero; event_feed
    // pulls Session::lastRttMs() each tick and forwards it here.
    void SetPing(int ms) { pingMs_ = ms; }
    int GetPing() const { return pingMs_; }

    void* actor() const { return actor_; }

private:
    // Apply curPos_/curYaw_/curSpeed_ to the engine actor + the AnimBP. Called by
    // Tick() once per frame (whether or not interp is active -- avoids drift if
    // some other system moved the actor between frames; cheap).
    void ApplyToEngine();

    // Linear LERP window. At 60 Hz send (~16.7 ms interval) this is ~4.5x the
    // interval -- a generous jitter buffer that handles LAN noise instantly
    // AND tolerates WAN jitter (50-150 ms RTT, irregular arrivals). The
    // visual lag added vs a tight 30 ms window is ~25 ms steady-state, well
    // below the threshold of "feels laggy" for a peer-view puppet (the local
    // player's own input is unaffected). Could be made adaptive (track avg
    // recv interval) later if needed.
    static constexpr int kInterpWindowMs = 75;
    // Snap thresholds (cm). At sprint ~600 cm/s in VOTV, one window's legal
    // motion is ~30 cm; anything more than (base + 0.5 s * speed) is a real
    // teleport (door warp, respawn) -- snap instead of trying to LERP across.
    static constexpr float kSnapBaseCm = 1000.f;
    static constexpr float kSnapPerSpeedSec = 0.5f;

    void* actor_ = nullptr;  // the engine ASkeletalMeshActor puppet (owned by the engine)
    // Placeholder until the peer's Join reliable message lands (typically within
    // a few RTT of connect). nameplate::Update repaints when SetNickname changes
    // this. The old "Player 2" default was misleading -- both ends saw "Player 2"
    // forever in early tests because no refresh path existed.
    std::wstring nickname_ = L"...";
    int pingMs_ = 0;  // last RTT measurement (set by event_feed from Session::lastRttMs)

    // Receiver-side interpolation state (game thread only -- the engine path is
    // single-threaded; no mutex). curPos_/curYaw_/curSpeed_ is what was last
    // applied to the engine; targetPos_/targetYaw_ is what we're walking toward.
    ue_wrap::FVector curPos_{};
    float            curYaw_ = 0.f;
    float            curPitch_ = 0.f;   // view pitch for the head bone
    float            curSpeed_ = 0.f;
    ue_wrap::FVector targetPos_{};
    float            targetYaw_ = 0.f;
    float            targetPitch_ = 0.f;
    // Cached at SetTargetPose time = target - cur. The interp incrementally
    // applies dAlpha * errorPos_ each frame (MTA's linear form -- recomputing
    // (target - cur) each frame would decay geometrically, not linearly).
    ue_wrap::FVector errorPos_{};
    float            errorYaw_ = 0.f;
    float            errorPitch_ = 0.f;
    uint64_t         interpStartMs_ = 0;
    uint64_t         interpFinishMs_ = 0;  // 0 == no active interp window (frozen)
    float            lastAlpha_ = 0.f;
    bool             hasPose_ = false;  // first packet snaps; spawn placeholder is fake
    bool             dirty_ = true;     // true while there's an unapplied change to push to the engine
};

}  // namespace coop

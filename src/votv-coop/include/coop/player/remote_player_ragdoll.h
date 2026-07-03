// coop/player/remote_player_ragdoll.h -- the remote player's ragdoll DISPLAY.
//
// Extracted from RemotePlayer 2026-07-02 (audit: remote_player.cpp past the
// 800-LOC soft cap; the ragdoll display is a self-contained lifecycle with its
// own state). Gameplay layer (principle 7): engine access via ue_wrap only.
//
// Mechanism (2026-07-03 VISIBLE-PLUSHY rework): when a remote player ragdolls
// (wire kStateBitRagdoll), spawn VOTV's own playerRagdoll_C body VISIBLE -- the
// game's plushie ragdoll, whose mesh is natively rigged to the full 6-bone
// physics chain (lowlegs-thighs-pelvis-chest-head-head_end); it IS what SP
// shows in mirrors -- and HIDE the puppet's two kel body meshes for the flop
// (SetVisibility only: no mesh-asset clearing, so a pak-loaded custom-skin
// asset keeps its component reference and cannot be GC'd mid-flop). The puppet
// actor stays pelvis-attached to the body as the position anchor (nameplate +
// recover hand-off). Spawn is DEATH-FREE (ragdollMode is globally scoped and
// kills the host -- never called). v22 physics sync: the body's pelvis
// velocity is slaved to the sender's real ragdoll so the visible flop TRACKS
// it (the streamed pelvis ROTATION is unused since the rework -- it existed to
// tumble the kel-attached actor; it stays on the wire untouched).
//
// History: v22 hid the body and tumbled the pelvis-attached kel instead (rigid
// one-piece look); a master-pose probe (2026-07-03 AM) slaved the kel meshes
// to the body but only 4/6 bones mapped (the visible skin's skeleton has no
// thighs/lowlegs -- name-based master-pose cannot couple the legs) and the
// user refuted it by eye. Both retired per RULE 2.
//
// Owned by RemotePlayer (composition). The owner keeps its glue side effects
// (presentation-yaw re-base, pose-dirty) keyed off the return values so this
// class never reaches back into RemotePlayer state. Game thread only.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop {

class RagdollDisplay {
public:
    // Wire-bit edge detector (called on every pose packet with the streamed
    // kStateBitRagdoll). Spawns the body on the rising edge / recovers on the
    // falling edge -- exactly once per transition (the bit is continuous, so a
    // dropped edge self-heals on the next pose; TeardownForDestroy resets the
    // latch so a recycled puppet re-converges). Returns true when a STOP ran
    // this call: the owner re-bases its presentation yaw on the wire truth
    // (the get-up is a visual discontinuity anyway).
    bool OnWireBit(bool ragdollBit, void* puppetActor, int32_t puppetIdx);

    // v22 ragdoll PHYSICS sync (RemotePlayer::SetRagdollPose): slave the live
    // body's pelvis velocity to the sender's streamed state so the plushie
    // tumbles to TRACK the sender's real ragdoll. Returns true when applied to
    // a live body; a packet racing ahead of the spawn edge is dropped harmless
    // -- the next one applies.
    bool SetPose(const coop::net::RagdollPoseSnapshot& snap);

    // ApplyToEngine head. While ragdolled the VISIBLE plushie body is the whole
    // display and the attachment owns the puppet transform -- pose-driving
    // would fight it; there is nothing to drive per frame.
    enum class Drive {
        Inactive,   // not ragdolling -- the owner pose-drives as normal
        Attached,   // attachment owns the transform this tick -- owner returns
        StoppedNow  // body died under us (e.g. level-transition GC): self-
                    // healed (kel un-hidden, detach, clear) -- owner re-bases
                    // its yaw and falls through to normal pose-drive THIS tick
    };
    Drive DriveAttached(void* puppetActor, int32_t puppetIdx);

    // RemotePlayer::Destroy teardown: destroy the BODY only (the puppet actor
    // dies right after -- it would otherwise orphan the body; its hidden kel
    // meshes die with it, so no un-hide) + reset every latch so a recycled
    // puppet starts un-ragdolled and re-converges.
    void TeardownForDestroy();

    // Test/diagnostic seam (forwarded by RemotePlayer accessors).
    bool Active() const { return active_; }
    void* Body() const { return body_; }
    int32_t BodyIdx() const { return bodyIdx_; }

private:
    void Start(void* puppetActor, int32_t puppetIdx);
    void Stop(void* puppetActor, int32_t puppetIdx);

    // Last WIRE-bit value acted on (edge detector: the spawn/recover dispatch
    // fires once per transition, no retry).
    bool wireState_ = false;
    // The puppet is currently pelvis-attached to a body -> the attachment
    // drives the transform instead of the pose drive.
    bool active_ = false;
    // The spawned (visible) playerRagdoll_C body + its GUObjectArray index
    // (IsLiveByIndex safety across ticks). null/-1 when not ragdolled.
    void*   body_ = nullptr;
    int32_t bodyIdx_ = -1;
};

}  // namespace coop

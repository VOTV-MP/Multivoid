// coop/player/remote_player_ragdoll.h -- the remote player's ragdoll DISPLAY.
//
// Extracted from RemotePlayer 2026-07-02 (audit: remote_player.cpp past the
// 800-LOC soft cap; the ragdoll display is a self-contained lifecycle with its
// own state). Gameplay layer (principle 7): engine access via ue_wrap only.
//
// Mechanism (2026-06-01 xray-actor rework, [[project-ragdoll-sync]]): when a
// remote player ragdolls (wire kStateBitRagdoll), spawn an INVISIBLE
// playerRagdoll_C physics body (VOTV's own ragdoll, its "plushy" mesh hidden
// inside SpawnPlayerRagdollBody) and PELVIS-ATTACH the visible Dr. Kel puppet
// to it, so the kel tumbles WITH the physics -- DEATH-FREE (ragdollMode is
// globally scoped + kills the host) and needs no shared skeleton. v22 adds the
// ragdoll PHYSICS stream: the mirror body's pelvis velocity is slaved to the
// sender's real ragdoll and the streamed pelvis rotation is driven onto the
// kel each tick, so the flop TRACKS the sender instead of free-simulating.
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

    // v22 ragdoll PHYSICS sync (RemotePlayer::SetRagdollPose): stash the
    // streamed pelvis state + slave the live body's pelvis velocity to it so
    // the mirror tumbles to TRACK the sender's real ragdoll. Returns true when
    // applied to a live body (the owner marks its pose dirty so the next
    // ApplyToEngine refreshes the kel rotation); a packet racing ahead of the
    // spawn edge is stashed-only and harmless -- the next one applies.
    bool SetPose(const coop::net::RagdollPoseSnapshot& snap);

    // ApplyToEngine head. While the puppet is pelvis-attached the engine syncs
    // its transform per-frame -- pose-driving would fight the attachment; this
    // drives only the kel's WORLD rotation from the streamed pelvis (or, before
    // the first v22 packet, bootstraps from the mirror body's own pelvis).
    enum class Drive {
        Inactive,   // not ragdolling -- the owner pose-drives as normal
        Attached,   // attachment owns the transform this tick -- owner returns
        StoppedNow  // body died under us (e.g. level-transition GC): self-
                    // healed (detach + clear) -- owner re-bases its yaw and
                    // falls through to normal pose-drive THIS tick
    };
    Drive DriveAttached(void* puppetActor, int32_t puppetIdx);

    // RemotePlayer::Destroy teardown: destroy the BODY only (the puppet actor
    // dies right after -- it would otherwise orphan the body) + reset every
    // latch so a recycled puppet starts un-ragdolled and re-converges.
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
    // The spawned (invisible) playerRagdoll_C body + its GUObjectArray index
    // (IsLiveByIndex safety across ticks). null/-1 when not ragdolled.
    void*   body_ = nullptr;
    int32_t bodyIdx_ = -1;
    // v22: the latest streamed pelvis state. hasPose_ gates the rotation
    // drive: until the first packet arrives, DriveAttached bootstraps from the
    // mirror body's own pelvis; once streaming, the EXACT streamed rotation is
    // used (no reliance on the local sim matching the sender). Reset on stop.
    coop::net::RagdollPoseSnapshot lastPose_{};
    bool hasPose_ = false;
};

}  // namespace coop

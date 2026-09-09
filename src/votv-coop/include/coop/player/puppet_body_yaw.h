// coop/player/puppet_body_yaw.h -- receiver-side body-yaw presentation (turn-in-place) for the
// remote-player puppet.
//
// The wire streams the SOURCE's actor yaw, but VOTV's first-person pawn turns its body with the
// camera almost immediately, so on foot the streamed yaw IS the camera yaw -- headYawDelta near
// zero. Replaying it as-is snaps the whole puppet with every camera move, and the head's lookAt
// target then sits dead ahead of the body, where it can never visibly lead. The "head first, then
// body" behaviour the source never produces is synthesized HERE, by Update below.
//
// MTA precedent: reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:3450-3474, where
// a remote ped's body rotation is its own interpolated presentation state, never slammed from the
// wire.

#pragma once

#include "ue_wrap/core/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coop::puppet_body_yaw {

class State {
public:
    static constexpr float kTurnStartDeg     = 60.f;
    static constexpr float kTurnStopDeg      = 5.f;
    static constexpr float kTurnRateDegSec   = 360.f;
    static constexpr float kFollowRateDegSec = 1080.f;
    static constexpr float kMoveSpeedCmS     = 30.f;

    // What SetActorRotation shows (the presentation yaw).
    float Yaw() const { return yaw_; }

    // Re-base on wire truth: spawn placement, first packet, teleport snap,
    // ragdoll recover. Clears the hysteresis latch + the dt clock.
    void Reset(float yawDeg) {
        yaw_ = yawDeg;
        turning_ = false;
        lastMs_ = 0;
    }

    // Advance one tick. Returns true when yaw_ changed, and the caller pushes the new value to the
    // engine. nowMs is the caller's steady clock, passed in so this struct stays clock-free, the
    // single clock living in remote_player. NormalizeAxis(to - from) is the shortest-arc delta in
    // (-180, 180], MTA's GetOffsetDegrees shape.
    //
    // STANDING (speed <= kMoveSpeedCmS): yaw HOLDS and the head leads via the kerfur lookAt clamp,
    // driven elsewhere from the camera yaw. Once the camera leads by kTurnStartDeg -- just inside
    // the head's 45 plus the neck's ~22 degrees of LookAt reach -- the body turns toward it at
    // kTurnRateDegSec until within kTurnStopDeg, a hysteresis latch.
    //
    // MOVING: rate-follow the STREAMED actor yaw, the true body facing, at kFollowRateDegSec --
    // fast enough for the hardest real flick, slow enough to smooth the hold-to-move pop.
    // Controller yaw as body facing produces a sideways strafe.
    bool Update(uint64_t nowMs, float speedCmS, float wireYawDeg, float headYawDeltaDeg) {
        const float dtRaw = (lastMs_ != 0) ? static_cast<float>(nowMs - lastMs_) * 0.001f : 0.f;
        lastMs_ = nowMs;
        // Clamp hitches (alt-tab, load stall) so a long frame never warp-spins
        // the body; dt==0 (first call / same-ms tick) advances nothing.
        // (std::min) parenthesized: this header reaches TUs where windows.h
        // defines a min() macro.
        const float dt = (std::min)(dtRaw, 0.1f);
        if (dt <= 0.f) return false;

        const bool moving = speedCmS > kMoveSpeedCmS;
        float desired;
        float rate;
        if (moving) {
            desired = wireYawDeg;
            rate = kFollowRateDegSec;
            turning_ = false;
        } else {
            const float camYaw = wireYawDeg + headYawDeltaDeg;
            if (!turning_ &&
                std::fabs(ue_wrap::NormalizeAxis(camYaw - yaw_)) < kTurnStartDeg) {
                return false;  // head leads; body holds
            }
            turning_ = true;
            desired = camYaw;
            rate = kTurnRateDegSec;
        }

        const float remaining = ue_wrap::NormalizeAxis(desired - yaw_);
        const float absRemaining = std::fabs(remaining);
        if (!moving && absRemaining <= kTurnStopDeg) {
            turning_ = false;  // settled -- hold again until the next big lead
            return false;
        }
        if (absRemaining < 0.01f) return false;  // aligned (moving steady-state)

        const float step = rate * dt;
        yaw_ = (absRemaining <= step) ? desired
                                      : yaw_ + (remaining > 0.f ? step : -step);
        yaw_ = ue_wrap::NormalizeAxis(yaw_);
        return true;
    }

private:
    float    yaw_ = 0.f;
    bool     turning_ = false;  // hysteresis latch: mid turn-in-place
    uint64_t lastMs_ = 0;       // dt clock (0 = unseeded)
};

}  // namespace coop::puppet_body_yaw

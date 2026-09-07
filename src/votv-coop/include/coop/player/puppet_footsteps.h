// coop/player/puppet_footsteps.h -- footstep audio for the remote-player puppet.
//
// VOTV's footsteps are a tick-driven distance accumulator in mainPlayer's ubergraph: while
// |velocity| > 10 cm/s it accumulates how far the foot location moved and calls lib_C::step every
// 150 cm. The puppet cannot inherit that, because its actor tick is off from spawn
// (puppet_spawn.cpp) to keep mainPlayer_C's ReceiveTick -- HUD, look traces, hunger and thirst --
// from running a second single-player brain.
//
// So this reproduces the CALLER, the stride accumulator, and dispatches the same callee the local
// player's own tick does: lib_C::step, which sphere-traces the ground, raises the stepped-on event
// and then picks the per-material cue and its spatialization. Nothing is re-implemented.
//
// Header-only: one small struct per RemotePlayer, ticked from ApplyToEngine right after the CMC
// drive. Float math per frame plus ONE lib_C::step dispatch per ~150 cm walked (2-4 Hz per moving
// puppet). Game thread only.

#pragma once

#include "ue_wrap/core/types.h"
#include "ue_wrap/world/votv_lib.h"

#include <cmath>

namespace coop::puppet_footsteps {

struct Stride {
    // Native constants, read off mainPlayer's ubergraph: a 150 cm stride, a |v| > 10 cm/s move
    // gate, and 0.75 applied to the accumulated distance while running. The run TEST is ours -- the
    // graph reads its run input, which the wire pose does not carry, so speed stands in for it at
    // the same 400 cm/s lib_C::step normalizes volume against. The graph's crouch x2.0 and water
    // x0.5 are omitted: stateBits carry only InAir and Ragdoll today.
    static constexpr float kStrideCm     = 150.f;
    static constexpr float kMinSpeedCmS  = 10.f;
    static constexpr float kRunSpeedCmS  = 400.f;
    static constexpr float kRunFactor    = 0.75f;
    // Teleport/connect-snap guard: a per-frame displacement this large is a
    // warp, not locomotion -- drop the sample (re-prime from the new spot).
    static constexpr float kMaxSampleCm  = 200.f;
    // Remote-step loudness. The native call passes 1.0 for the local player's own feet, and a
    // puppet at full volume reads too loud. lib_C::step multiplies this by its own
    // clamp(MaxWalkSpeed/400, 0.5, 2.0), so sprinting still scales louder.
    static constexpr float kStepVolume   = 0.6f;

    // Advance the stride accumulator one frame; true exactly when a step lands (every ~150 walked
    // cm). The caller dispatches the consequences from the ONE verdict -- the puppet fires
    // lib_C::step (CharacterStep) AND the skin step FX (skin_effects::OnStep) together, so they
    // cannot drift apart; the local body's skin_effects::TickStride runs its own instance over the
    // wire-pose samples. One stride emitter either way.
    bool StepDue(const ue_wrap::FVector& pos, float speedCmS, bool grounded) {
        if (!grounded || speedCmS <= kMinSpeedCmS) {
            // Idle or airborne: drop the accumulator and unprime, so the next moving sample
            // re-primes from the new spot. The graph zeroes its own lastStep at this same gate.
            // lib_C::step's ground trace is the second airborne safety -- it is silent when the
            // trace hits nothing.
            accum_ = 0.f;
            primed_ = false;
            return false;
        }
        if (!primed_) {
            last_ = pos;
            primed_ = true;
            return false;
        }
        const float dx = pos.X - last_.X, dy = pos.Y - last_.Y, dz = pos.Z - last_.Z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        last_ = pos;
        if (d > kMaxSampleCm) return false;  // warp -- not a stride
        accum_ += d * (speedCmS > kRunSpeedCmS ? kRunFactor : 1.0f);
        if (accum_ >= kStrideCm) {
            accum_ = 0.f;
            return true;
        }
        return false;
    }

    void Reset() { accum_ = 0.f; primed_ = false; }

private:
    ue_wrap::FVector last_{};
    float accum_ = 0.f;
    bool  primed_ = false;
};

}  // namespace coop::puppet_footsteps

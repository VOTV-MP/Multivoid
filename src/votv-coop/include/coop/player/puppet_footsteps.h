// coop/puppet_footsteps.h -- footstep audio for the remote-player puppet.
//
// WHY (hands-on 2026-06-10, "remote player has no footstep sounds"): VOTV's
// footsteps are a BP TICK distance accumulator in mainPlayer's ubergraph
// (block @70172: accumulate |delta findFootLocation| while |v|>10, fire
// lib_C::step every 150 cm) -- and the puppet's mainPlayer BP tick is
// DELIBERATELY suppressed (puppet.cpp SetActorTickEnabled(false): it would
// run the whole SP brain + clobber the global camera MPC). Kerfur stays
// audible because its rig uses anim-notifies -> int_anim_events.step --
// an interface mainPlayer_C does not implement (and we cannot add one:
// asset edit, A6). So the coop layer reproduces THE CALLER (the stride
// accumulator, native constants cited per @-offset) and dispatches the
// SAME callee the local player and Kerfur both funnel into --
// lib_C::step -- which natively does the ground trace, the per-material
// cue choice (foot_def/grass/metal/snow/water/...), spatialization and
// the steppedOn world reactions. Nothing re-implemented (RULE 1 natural
// dispatch; the kerfur/lib RE pass 2026-06-10).
//
// Header-only: one small struct owned per RemotePlayer, ticked from
// ApplyToEngine right after the CMC drive. Cost: float math per frame +
// ONE lib_C::step ProcessEvent per ~150 cm walked (~2-4 Hz per moving
// puppet). Game thread only.

#pragma once

#include "ue_wrap/types.h"
#include "ue_wrap/votv_lib.h"

#include <cmath>

namespace coop::puppet_footsteps {

struct Stride {
    // Native constants (mainPlayer ubergraph):
    //   stride 150 cm           (@70685)
    //   move gate |v| > 10 cm/s (@70241)
    //   run factor 0.75 on accumulated distance while sprinting (@70458;
    //     the run threshold mirrors the speedVolume=400 normalization).
    // Crouch x2.0 / water x0.5 are omitted: no crouch/water wire state today
    // (PoseSnapshot stateBits carries only InAir/Ragdoll); revisit with them.
    static constexpr float kStrideCm     = 150.f;
    static constexpr float kMinSpeedCmS  = 10.f;
    static constexpr float kRunSpeedCmS  = 400.f;
    static constexpr float kRunFactor    = 0.75f;
    // Teleport/connect-snap guard: a per-frame displacement this large is a
    // warp, not locomotion -- drop the sample (re-prime from the new spot).
    static constexpr float kMaxSampleCm  = 200.f;
    // Remote-step loudness: the native call passes 1.0 for the LOCAL player's
    // own feet; a remote puppet's steps at full volume read too loud
    // (user-tuned down 2026-06-11). Multiplies the step BP's internal
    // clamp(MaxWalkSpeed/400, 0.5, 2.0) -- sprint still scales louder.
    static constexpr float kStepVolume   = 0.6f;

    // Advance the stride accumulator one frame; true exactly when a step lands
    // (every ~150 walked cm). The caller dispatches the consequences from the
    // ONE verdict -- the puppet fires lib_C::step (CharacterStep) AND the skin
    // step FX (skin_effects::OnStep) together, so they can never drift apart;
    // the local body's skin_effects::TickStride runs its own instance over the
    // wire-pose samples (RULE 2: one stride emitter).
    bool StepDue(const ue_wrap::FVector& pos, float speedCmS, bool grounded) {
        if (!grounded || speedCmS <= kMinSpeedCmS) {
            // Idle/airborne: reset like the native chain re-primes lastStep
            // when stopped (@71043). lib_C::step's own ground trace is the
            // second airborne safety (silent on no hit).
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

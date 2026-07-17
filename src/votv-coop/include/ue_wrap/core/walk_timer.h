// ue_wrap/walk_timer.h -- [WALK-TIME] profiling for periodic heavy passes (the L5 FPS-stutter hunt).
//
// WHY: a periodic full-GUObjectArray walk (FindObjectByClass / a NumObjects() loop / SnapshotActorsByType)
// runs on the GAME THREAD, so a pass that takes several ms IS a frame-time spike. The ~3-4s FPS stutter
// survived two one-at-a-time walk gates because we GUESSED which walk caused it instead of measuring.
// Wrap each periodic walk in a ScopedWalkTimer and the smoke log shows EXACTLY which pass ate the frame,
// with its duration -- profile the drop, don't assume. Only passes over the threshold log (a cheap/gated/
// skipped walk stays silent), so [WALK-TIME] lines ARE the frame-relevant passes, rankable by duration.
//
// The harness (tools/pile-test-assert.ps1) asserts the max [WALK-TIME] duration stays under a frame budget
// -- so an FPS regression (a new or un-gated heavy walk) FAILS the script instead of only being felt.
// Game-thread diagnostic; negligible cost (one steady_clock read per wrapped pass).
#pragma once

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstdint>

namespace ue_wrap {

struct ScopedWalkTimer {
    const char*                           name_;
    std::chrono::steady_clock::time_point t0_;
    explicit ScopedWalkTimer(const char* name)
        : name_(name), t0_(std::chrono::steady_clock::now()) {}
    ~ScopedWalkTimer() {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0_)
                            .count();
        // >=1ms -- frame-time-relevant (16.7ms = a fully dropped 60fps frame; even 1-5ms is a felt hitch).
        // A gated/skipped walk costs ~0 and never logs, so the absence of a [WALK-TIME] line is the proof
        // a gate worked. The throttling lives in the caller's gate, not here.
        if (us >= 1000)
            UE_LOGI("[WALK-TIME] %s = %lld us", name_, static_cast<long long>(us));
    }
};

}  // namespace ue_wrap

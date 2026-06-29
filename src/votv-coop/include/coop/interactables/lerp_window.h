// coop/lerp_window.h -- the shared MTA-form interpolation TIMING window.
//
// One concept, one implementation (RULE 2 / WP13): the per-frame alpha/dAlpha
// bookkeeping that drives a fixed-duration linear interpolation toward a cached
// error. Extracted 2026-06-07 from the two byte-identical copies that had grown
// up independently -- RemotePlayer (coop/remote_player.cpp) and element::Npc
// (coop/npc_pose_drive.cpp). Both now own a LerpWindow and apply the returned
// dAlpha to their OWN cached errors (RemotePlayer interpolates pos+yaw+pitch+
// headYawDelta; Npc only pos+yaw) -- the window owns ONLY the timing, never any
// pose field, so the differing field sets stay in each class.
//
// MTA shape (CClientPed::Interpolate): the error is cached ONCE at packet
// arrival (target - cur at that instant); each frame applies dAlpha * error so
// the motion is LINEAR, not geometric-decay. At alpha==1 the full error has been
// applied and the window closes (the caller snaps cur=target exactly to kill
// float drift) until the next packet re-Opens it.
//
// Pure value type -- no engine/network deref, no allocation, header-only. Each
// owner is single-threaded on the game thread (the receiver interp path), so no
// synchronization. NowMs() stays in each caller (steady_clock ms) and is passed
// into Advance -- the window is clock-agnostic.

#pragma once

#include <algorithm>
#include <cstdint>

namespace coop {

class LerpWindow {
public:
    // True while a window is open (motion still to apply). False == frozen at
    // target between packets (mirrors the old `interpFinishMs_ == 0` sentinel).
    bool IsOpen() const { return finishMs_ != 0; }

    // Open a fresh window [now, now+windowMs]. Resets the applied-alpha cursor so
    // the first Advance() in this window yields dAlpha measured from 0.
    void Open(uint64_t now, int windowMs) {
        startMs_   = now;
        finishMs_  = now + static_cast<uint64_t>(windowMs);
        lastAlpha_ = 0.f;
    }

    // Freeze at target (snap / teleport / reset): no open window, no motion.
    void Close() {
        startMs_   = 0;
        finishMs_  = 0;
        lastAlpha_ = 0.f;
    }

    // Advance the open window to `now` and return the alpha DELTA to apply to the
    // caller's cached errors this step (cur += error * dAlpha). Returns 0 when no
    // window is open. On the final step (alpha reaches 1) the window CLOSES and
    // *arrived is set true, so the caller snaps cur = target exactly. This is the
    // exact bookkeeping the old RemotePlayer/Npc AdvanceInterp ran inline:
    //   alpha    = clamp((now - start) / span, 0, 1)
    //   dAlpha   = alpha - lastAlpha;  lastAlpha = alpha
    //   if (alpha >= 1) close + arrived
    float Advance(uint64_t now, bool* arrived = nullptr) {
        if (arrived) *arrived = false;
        if (finishMs_ == 0) return 0.f;  // no window open -- frozen at target
        const uint64_t span = finishMs_ - startMs_;  // == the windowMs handed to Open
        float alpha = (span == 0) ? 1.f
                                  : static_cast<float>(now - startMs_) / static_cast<float>(span);
        alpha = std::clamp(alpha, 0.f, 1.f);
        const float dAlpha = alpha - lastAlpha_;
        lastAlpha_ = alpha;
        if (alpha >= 1.f) {
            finishMs_ = 0;  // exact arrival -- caller snaps cur = target
            if (arrived) *arrived = true;
        }
        return dAlpha;
    }

private:
    uint64_t startMs_  = 0;
    uint64_t finishMs_ = 0;  // 0 == no active window (frozen at target)
    float    lastAlpha_ = 0.f;
};

}  // namespace coop

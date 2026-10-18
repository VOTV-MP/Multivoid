// coop/dev/director/clear_hand.h -- the director's clearing of the local player's hand: the key a human presses to
// drop first (InpActEvt_drop), the hand then MEASURED, and the effect-seam release only when that key is measured
// inert. The walk set's ClearHand process runs this same code; a drill whose step needs an empty hand ticks it
// itself. DEV-ONLY, like the rest of the director; game thread. In proc_walkgrab.cpp.

#pragma once

#include "coop/dev/director/director.h"   // PlayerContext

namespace coop::director {

enum class ClearState { Working, Cleared, Failed };

// One clearing, ticked once per game-thread tick until it says Cleared or Failed; it bounds itself (a few dozen
// ticks), so a caller adds only a wall-clock backstop for a stalled game thread.
class ClearHand {
public:
    // Tick on the context the caller refreshed this tick.
    ClearState Tick(const PlayerContext& ctx);
    // Tick on the local player's own state, read here; Failed when it cannot be read (not possessed).
    ClearState Tick();
    const char* Why() const { return why_; }          // the failure, once Failed
    bool UsedEffectFallback() const { return usedEffectFallback_; }   // the drop key was measured inert
    int  Ticks() const { return ticks_; }
    void LogCleared() const;                          // the "hand cleared" line, for a caller that learns it late
private:
    int  ticks_ = 0;
    bool usedEffectFallback_ = false;
    bool failed_ = false;
    const char* why_ = "";
};

}  // namespace coop::director

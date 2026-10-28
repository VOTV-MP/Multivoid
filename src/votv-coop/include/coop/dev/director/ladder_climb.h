// coop/dev/director/ladder_climb.h -- the director climbs a ladder the way a player does: it takes hold through the
// ladder's own use verb (ladder_C::actionOptionIndex, which refuses a player more than 125 uu from the ladder or a
// capsule that does not fit there), then holds the game's "forward" key at the human-input seam (mainPlayer's AnyKey
// event, which forwards the key to the ladder it controls) until the ladder lets the player go at the top. No
// location is written. DEV-ONLY, like the rest of the director; game thread.
//
// The walk TO the foot is the caller's (routes.h's standpoints and a background walk); this is the ladder's half,
// driven from the caller's tick.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::director::ladder {

// The ladder `owner` carries: the child actor of its `ladder` component, of class ladder_C or a subclass (the
// towers' is ladder_old_C). Null when there is none, or it is not live.
void* Find(void* owner);

// The world locations of the ladder's foot (its `a` point) and top (its `b_off` point), read from the live
// components. False when one does not read.
bool Foot(void* ladder, ue_wrap::FVector& out);
bool Top(void* ladder, ue_wrap::FVector& out);

// How one climb ended. `why` is null for a climb the ladder ended itself by letting the player go; otherwise the
// reason it did not run or ran out (a static string). `end` is where the player stood when it ended, `atTop` whether
// that is within 200 uu of the ladder's top point.
struct Outcome {
    bool        mounted = false;
    bool        atTop = false;
    uint64_t    climbMs = 0;
    ue_wrap::FVector end{};
    const char* why = nullptr;
};

// One climb: mount, hold forward, release. Tick it once per game-thread tick with the same player and ladder; it
// returns true from the tick that finished it, and Result() is then final. A refused mount is final (no retry), and a
// climb is bounded at 60 s: a climb that runs out lets go with the game's own jump key, which the ladder answers
// with its drop, so the player is not left hanging.
class Climb {
public:
    bool Tick(void* player, void* ladder);
    const Outcome& Result() const { return out_; }

    // The caller is going away while the climb runs (the arm ended, the session ended): the forward key goes up as
    // a person's does and the climb is over. A no-op in any other phase.
    void Release(void* player, void* ladder);

private:
    enum class Phase : uint8_t { Mount, Climb, Done };

    void Finish(void* player, void* ladder, const char* why, bool drop = false);

    Phase phase_ = Phase::Mount;
    Outcome out_;
    uint64_t startMs_ = 0;
    uint64_t nextLogMs_ = 0;
    ue_wrap::reflection::FName forward_{};   // the key the game binds to "forward", read at the mount
};

}  // namespace coop::director::ladder

// ue_wrap/core/class_reask.h -- when a lookup that missed asks again.
//
// ClassByName answers null while a class loads, and a class finishing its load moves no number
// (object_index.h:55-58), so the class set's move can come before the moment the lookup would answer.
// A Latch asks again on a move and then on every call for kLoadWindowMs after it; once that window
// has run out it asks nothing until the next move, so a class that never loads costs no work per tick
// for the rest of the session. Game thread (it reads the class set's version).
#pragma once

#include <cstdint>

namespace ue_wrap::class_reask {

// How long after a class-set move a missed lookup is asked on every call.
constexpr uint64_t kLoadWindowMs = 10000;

class Latch {
public:
    // True when the lookup should be asked now: the class set moved since the last window opened, or
    // the open window has not run out. A holder calls it only while its lookup is unanswered.
    bool Due();

    // A window was opened and has run out without the holder dropping the latch: the lookup is
    // not asked again until the class set moves.
    bool Expired() const { return opened_ && !open_; }

    // The next Due() opens a window, whatever the class set did (the holder lost what it had held).
    void Reset();

private:
    bool     opened_ = false;
    bool     open_ = false;
    uint64_t setAsked_ = 0;
    uint64_t windowEndMs_ = 0;
};

}  // namespace ue_wrap::class_reask

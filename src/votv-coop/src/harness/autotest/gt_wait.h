// harness/autotest/gt_wait.h -- a drill's readiness wait on a state only the game thread may read. Private to the
// drills that include it (grep `gt_wait.h` under harness/autotest/ for the list).

#pragma once

#include "disc_drills.h"
#include "harness/autotest/peer_signal.h"

namespace harness::autotest {

// Poll `pred` on the game thread every 100 ms for at most `seconds`. A bound passing is an INVALID verdict (named) unless
// the caller judges it (`abort` false). The wait ends on the state, never on a clock.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return peer_signal::WaitFor(what, seconds, [&] {
        bool ok = false;
        disc_drill::OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

}  // namespace harness::autotest

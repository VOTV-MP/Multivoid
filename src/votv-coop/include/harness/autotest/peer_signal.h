// harness/autotest/peer_signal.h -- TEST-ONLY: the folder handshake two-peer drills use to order their steps.
//
// The two peers share a folder (VOTVCOOP_CAN_DIR). A peer leaves a signal by writing a file there and the other
// waits for it. A signal counts only when it was written after THIS peer's drill began: an earlier run's files stay
// in the folder, and reading one as the other peer's answer let a drill pass or move on against a run that never
// happened (measured, 6866cee81). State is per process, since one drill runs per process.

#pragma once

#include <string>

namespace harness::autotest::peer_signal {

// Reads VOTVCOOP_CAN_DIR, records the run start and remembers `logTag` for the ABORT lines. Logs
// `<tag>: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset` and returns false when the folder is not set.
bool Begin(const char* logTag);

// Leaves the signal `name` for the other peer.
void Mark(const char* name);

// Leaves the signal `name` carrying `text` (one line) for the other peer to read with AwaitText.
void Mark(const char* name, const std::string& text);

// Waits for the other peer's signal `name`, written since Begin. A timeout logs
// `<tag>: VERDICT ABORT -- no '<name>' from the other peer` and returns false.
bool Await(const char* name, int seconds = 240);

// Await, and `out` receives the signal's first line. Same fresh-only rule and the same ABORT line on a timeout.
bool AwaitText(const char* name, std::string& out, int seconds = 240);

namespace detail {
void SleepStep();                                    // one 100 ms poll step
void ReportTimeout(const char* what, int boundSeconds);  // the ABORT line of WaitFor
}  // namespace detail

// Polls a state every 100 ms for at most boundSeconds. A timeout is an ABORT line unless the caller judges it.
template <class Pred>
bool WaitFor(const char* what, int boundSeconds, Pred pred, bool abortOnTimeout = true) {
    for (int i = 0; i < boundSeconds * 10; ++i) {
        if (pred()) return true;
        detail::SleepStep();
    }
    if (abortOnTimeout) detail::ReportTimeout(what, boundSeconds);
    return false;
}

}  // namespace harness::autotest::peer_signal

// harness/autotest/peer_signal.h -- TEST-ONLY: the folder handshake two-peer drills use to order their steps.
//
// The two peers share a folder (VOTVCOOP_CAN_DIR). A peer leaves a signal by writing a file there and the other
// waits for it. A signal counts only when it was written after THIS peer's drill began: an earlier run's files stay
// in the folder, and reading one as the other peer's answer let a drill pass or move on against a run that never
// happened (measured, 6866cee81). State is per process, since one drill runs per process.
//
// A mark is never written over a file nor renamed into place (Windows refuses both while an outside process holds the
// target or the source open): each Mark creates a NEW file `<name>.<stamp>` in one write ending with '\n', the stamp
// being the system time in 100 ns units as 16 hex digits. A reader takes the newest COMPLETE `<name>.<stamp>` (one that
// ends with '\n') written since Begin. The writer keeps its two newest files of a name and deletes its older ones, so
// a mark a reader listed one poll earlier is still there when it opens it; a delete an outside holder refuses leaves
// a file that is harmless because readers take the newest.

#pragma once

#include <string>

namespace harness::autotest::peer_signal {

// Reads VOTVCOOP_CAN_DIR, records the run start and remembers `logTag` as the drill's verdict tag. Says
// `rig: VERDICT <tag> INVALID -- VOTVCOOP_CAN_DIR is unset` (coop/session/rig_verdict.h) and returns false when the
// folder is not set.
bool Begin(const char* logTag);

// The folder Begin read from VOTVCOOP_CAN_DIR; empty before Begin or when it is unset. For a drill's own log line.
const std::string& Dir();

// Leaves the signal `name` for the other peer.
void Mark(const char* name);

// Leaves the signal `name` carrying `text` (one line) for the other peer to read with AwaitText.
void Mark(const char* name, const std::string& text);

// Waits for the other peer's signal `name`, written since Begin. A timeout says
// `rig: VERDICT <tag> INVALID -- no '<name>' from the other peer` and returns false.
bool Await(const char* name, int seconds = 240);

// Await, and `out` receives the signal's first line. Same fresh-only rule and the same INVALID verdict on a timeout.
bool AwaitText(const char* name, std::string& out, int seconds = 240);

// The signal `name`'s first line if it was written since Begin, without waiting and without a verdict on a miss: for a drill
// that polls a signal alongside another state.
bool Peek(const char* name, std::string& out);

namespace detail {
void SleepStep();                                    // one 100 ms poll step
void ReportTimeout(const char* what, int boundSeconds);  // the INVALID verdict of WaitFor
}  // namespace detail

// Polls a state every 100 ms for at most boundSeconds. A timeout is an INVALID verdict unless the caller judges it.
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

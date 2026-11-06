// coop/dev/cremator_drill_g2.h -- what the cremator drill's G2 arms (cremator_drill_g2.cpp) take from the drill's own
// file: the waits' bounds, the reading of a cremator copy and the verdict line. Private to coop/dev. Game thread unless a
// declaration says otherwise.

#pragma once

#include "ue_wrap/core/game_thread.h"

#include <atomic>
#include <string>

namespace coop::dev::cremator_drill {

namespace detail {

// Bounds on waits for a game state: a timeout is the answer FAIL, never a clock to reach a state.
inline constexpr int kJoinSec    = 480;
inline constexpr int kSignalSec  = 240;
inline constexpr int kHostRunSec = 90;
inline constexpr int kCopySec    = 40;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);

// One reading of a cremator's copy, as signalled: -1 for a flag that did not read.
struct Reading { int active = -1, closed = -1, anim = -1; };
Reading ReadCopy(const std::wstring& key);
Reading Parse(const std::string& text);
std::string Text(const Reading& r);
int FieldOf(const std::string& text, const char* name);

// The cremator this peer's lane indexes under `key`.
void* Resolve(const std::wstring& key);
// A copy at rest in the wanted state: the flag, and the handle not moving.
bool CopyAt(const std::wstring& key, int active);

// One `cremator-drill: VERDICT PASS|FAIL <arm> -- <what>` line.
void Verdict(bool ok, const char* arm, const std::string& what);
// The failed verdicts so far, for the DONE line.
int Fails();
// A drill that could not measure: one `VERDICT ABORT -- <why>` line and the rig's `VERDICT cremator-drill INVALID` line
// with the same text (printf-style), so the rig ends the run instead of the other peer waiting out its bound.
void Abort(const char* fmt, ...);

// The world is up and the host's puppet exists here; false after the readiness verdict said why.
bool HostPuppetUp();
// Waits for this peer's lane to resolve the cremator `key` names.
bool CopyResolves(const std::wstring& key);
// The host's lane key for the cremator nearest the host's player; empty after the ABORT line said why.
std::wstring HostFindKey();
// Waits for the copy at `active` and signals what it reads under `signal`; a copy that never gets there is reported as
// it is, and the host judges it.
void ReportCopy(const std::wstring& key, int active, const char* signal);

}  // namespace detail

// The G2 arms (g2, g2-double, g2-far), one per role: a client presses the cremator's handle through the real seam and
// the host judges what its own copy and the lever's call count show.
void RunHostG2(const std::string& arm);
void RunClientG2(const std::string& arm);

}  // namespace coop::dev::cremator_drill

// coop/session/boot_selftests.h -- the selftests of game modules that the session once ran inside
// Session::Start: the join beacon, the nickname arbiter, the nickname's trims and the novelty cap.
// They test those modules, not the session, so the DLL's session bring-up runs them.

#pragma once

namespace coop::boot_selftests {

// Run each once per process (a latch per test, so a Stop/Start repeats nothing); results are logged,
// not acted on, like the harness's other selftests. Any thread that brings a session up.
void RunOnce();

}  // namespace coop::boot_selftests

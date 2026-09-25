// coop/dev/run_and_wait_selftest.h -- [dev] the one game-thread wait (ue_wrap/core/game_thread.h)
// driven through every way a body can end: an answer, no answer, a fault, a C++ throw, and a call made
// on the game thread itself. Each must come back as the header documents, so the fault path is
// measured rather than argued. Armed per run by run_and_wait_selftest=1; one pass per process, from a
// worker thread the first session tick starts. Its faults are provoked on purpose: the pump logs each
// one as an ERROR, and the verdict line names those as its own.
#pragma once

namespace coop::dev::run_and_wait_selftest {

// The session tick's entry: one latched flag read when off; starts the pass once. Game thread.
void Tick();

}  // namespace coop::dev::run_and_wait_selftest

// coop/dev/pi1_selftest.h -- [dev] proves the four substrate primitives of ue_wrap/core on the live game:
// a script-gate scope mark, a native post hook's bytecode call site, a const reference admitted into a
// native pre hook (on both of its routes), and a caller's local edited in place.
//
// Each peer, once its world is up (the host with its own Player Element, a client once it announced
// world-ready, both with the Blueprint library class loaded and a local player), installs its watches and
// hooks, waits for the name watches to settle, then runs every check in ONE tick so each result line is
// printed before a rig can end the run. Each check (scope-mark, call-sites, const-ref-pre,
// const-ref-bytecode, caller-locals) logs a PASS line tagged PI-1 or the FAIL lines of what it found, and a
// DONE line naming the role ends the run. It calls two read-only library functions, turns the local
// player's controller by a reflected call and by makeLookAt (once refused, once not), and puts it back each
// time. Armed per run by pi1_selftest=1.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pi1_selftest {

// The pump tick's entry: one latched flag read when off. Runs once per process, on the first ticks on which
// this peer's world is ready. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::pi1_selftest

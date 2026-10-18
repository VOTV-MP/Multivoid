// coop/dev/pi2_selftest.h -- [dev] proves the three pieces the puppet-identity module rests on, on the
// live game: the locals a bytecode call writes (script_calls::CallHasLocal, with two hand-built sites for
// its truncated and unreadable-local answers; the second logs one [Error] line), the registry's puppet-count
// listener, the caller-locals boot check (which now covers RemoveActors' remaining), and the registry's
// puppet-body query.
//
// Each peer runs every check in ONE tick, the first on which it is ready: its world is up (the host with
// its own Player Element, a client once it announced world-ready), the Blueprint library class is loaded,
// a local player exists and the other peer's puppet is valid. The classes birch_C, npc_angryErieFlesh_C
// and prop_shower_C are taken in that tick (found by name, else loaded by their /Game path, once), not
// waited on. Each check (call-locals, listener, caller-locals, body) logs a PASS line tagged PI-2 or the
// FAIL lines of what it found, then one DONE line names the role; DONE does not end the run, the rig's
// markers do. The listener check registers and clears a test puppet on the highest peer slot and leaves
// the registry, and the previous listener, as it found them; a session with that slot in use logs
// ABANDONED and skips the check. Armed per run by pi2_selftest=1.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pi2_selftest {

// The pump tick's entry: one latched flag read when off. Runs once per process, on the first tick on which
// this peer is ready. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::pi2_selftest

// coop/dev/physmods_drill.h -- [dev] the physical-modules drill (poll arc 2.2, F-43): a plug is sent at its
// verb, and nothing else sends one.
//   HOST   -- once the joiner's world is ready, plugs one module into a free desk slot through the desk's own
//             plugInModule (a module spawned at the desk, as a player carries one there), then waits until
//             its array also holds the client's module; its DONE line carries the verdict.
//   CLIENT -- once its world is ready and the host's module has reached its array, plugs another the same
//             way. A joiner whose desk already holds modules at its world-ready is a rejoin into the
//             drill's world (the drill starts on an empty desk): its own load wrote them, it plugs nothing
//             and says so, and the host's log shows whether any op came of that load (F-43).
// The host plugs once per process. A desk that would explode a hot plug (a console active with coldswap
// on) has coldswap lifted for the drill's own call and put back in the same tick. Lines are tagged
// [PHYSMODS-DRILL]; run with physmods_drill=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::physmods_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client says its line again.
void OnDisconnect();

}  // namespace coop::dev::physmods_drill

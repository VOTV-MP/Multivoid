// coop/dev/physmods_drill.h -- [dev] the physical-modules drill (poll arc 2.2, F-43): every edit of the
// desk's module array is sent at its verb, slot by slot, and a joiner's load sends none.
//   HOST   -- once the joiner's world is ready, and only on an empty desk (the rejoin leg reads a module
//             at the joiner's world-ready as the joiner's own load): plugs a module into a slot (leg 1).
//             When its array holds a second module of that type, the client's, it unplugs its own
//             through the desk's E press (leg 3). When both are gone and the client's other module has
//             arrived, its DONE line carries the verdict: the array holds that one module and no other.
//   CLIENT -- once its world is ready and the host's module has reached its array, plugs a second
//             module of that type into another slot (leg 2). When the host's has left, it unplugs its
//             own through the E press and plugs a module of another type (leg 4), then waits for the
//             host's canonical array to come back once for each of the two, its ACKED line. A joiner
//             whose desk holds modules at its world-ready is a rejoin into the drill's world: its own
//             load wrote them, it plugs nothing and says so, and the host's log shows whether any op
//             came of that load (F-43).
// A plug is a module spawned at the desk and handed to plugInModule, as a carried one arrives there; an
// unplug is the desk's lookAt and actionOptionIndex on a hit at the slot, what a player's E runs. A desk
// that would explode a hot plug or unplug (a console active with coldswap on) has coldswap lifted for
// the drill's own call and put back. The host's legs run once per process, which it keeps across a
// client's rejoin; the client's run per session. Lines are tagged [PHYSMODS-DRILL]; run with
// physmods_drill=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::physmods_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: the client arms again, so a rejoin says its line.
void OnDisconnect();

}  // namespace coop::dev::physmods_drill

// coop/dev/physmods_drill.h -- [dev] the physical-modules drill: every edit of the desk's module array is
// sent at its verb, slot by slot, and a joiner's load sends none.
//   HOST   -- once per process, when the joiner's world is ready and the desk is empty: plugs a module
//             (leg 1); when the client's second of that type is in, unplugs its own through the desk's E
//             press (leg 3); when both are gone and the client's other module is in, its DONE line says
//             the array holds that one module and no other.
//   CLIENT -- once the host's module is in its array, plugs a second of that type into another slot
//             (leg 2); when the host's has left, unplugs its own through the E press and plugs another
//             type (leg 4), then waits for the host's canonical array to come back once for each, still
//             the array its ops left: its ACKED line. A joiner whose desk holds modules plugs nothing.
// A plug is a module spawned at the desk and handed to plugInModule; an unplug is the desk's lookAt and
// actionOptionIndex on a hit at the slot, a player's E. A console active with coldswap on would explode
// either, so coldswap is lifted around the drill's own call and put back. "[PHYSMODS-DRILL] ABANDONED"
// is a leg that cannot go on (--dead-marker), "[PHYSMODS-DRILL] FAIL" a lane that measured wrong
// (--fail-marker). Run with physmods_drill=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::physmods_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: the client arms again, so a rejoin says its line.
void OnDisconnect();

}  // namespace coop::dev::physmods_drill

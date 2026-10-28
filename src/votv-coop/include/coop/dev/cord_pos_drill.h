// coop/dev/cord_pos_drill.h -- [dev] a corded prop that binds on a joiner after its world-ready keeps the floor it
// rests on while it is hidden. Set cord_pos_drill=1 on BOTH peers; run it on a fresh host world; no player is moved.
// On the first tick the host sees slot 1's peer-world-ready it spawns three coffee makers at ledge spots and plugs
// each into a free powered wall socket (the first at the measured coffee spot, the others at sockets whose spot has a
// ledge under it, or beside the first). Once a host copy rests it says `cordpos rest <key> <z> <x> <y>` in the drill
// chat. The client reads each maker from its bind, every tick, and while it is hidden keeps its lowest z and notes a
// tick with a root collision of none. When the host's line is in and the maker is shown, a maker that had a
// collision of none while hidden, or whose lowest hidden z lay more than 5 cm under the host's rest, is a line
// starting `[CORDPOS-DRILL] FAIL:`; one with no hidden read is a line starting `[CORDPOS-DRILL] INCONCLUSIVE:`.
// Each primitive's collision and the maker's place are then compared with the bind's collision and the host's place at the
// reveal +2 s and after 10 s at rest (within 5 cm, else FAIL). Each maker also logs the order of its drive end and
// its reveal. The client ends with a line starting `[CORDPOS-DRILL] client DONE --` and PASS, FAIL, INCONCLUSIVE or
// INVALID; the host prints that it staged its makers once its three rest lines are said. A host that cannot stage
// the drill prints a line starting `[CORDPOS-DRILL] INVALID (host):` and tells the client, which ends on INVALID.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::cord_pos_drill {

// Store the session; idempotent, called at install. A latched read when off.
void Install(coop::net::Session* s);

// Advances this peer's steps; a latched read when off. Game thread.
void Tick();

// The session ended: both roles start over.
void OnDisconnect();

// A client's mirror of a prop bound under `eid` (prop_fresh_spawn, before mirror_defer hides it). A coffee maker is
// tracked from here. A latched read when off.
void OnMirrorBound(uint32_t eid, void* actor);
// The same mirror right after its hide: one hidden read, so a mirror the lift reveals at once is still judged.
void OnMirrorHidden(uint32_t eid, void* actor);

}  // namespace coop::dev::cord_pos_drill

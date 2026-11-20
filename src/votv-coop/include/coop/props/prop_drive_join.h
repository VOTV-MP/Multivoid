// coop/props/prop_drive_join.h -- HOST: the props a physics constraint ties to a driven prop get a drive row of their own.
//
// A nailed, corded or wired partner of a prop that is carried, thrown, pushed or let go is dragged by the constraint, and no
// cause of its own moves it, so no row streamed it and its copies stood still. When a prop gets a row (any cause but this
// one's), or a hand takes it (the host's own, or a client's), the prop is owed a join: at the next drive tick a walk over
// coop/props/constraint_ties reaches every keyed prop tied to it, transitively, and each one that has no row and is in no
// hand gets a coasting row (CoastCause::Joined), which the drive's own rest rule ends when the body sleeps and its leader is
// in no hand. The walk passes through any actor whose root body simulates physics (props, cord and wire segments, a hook)
// and stops at one whose root does not (a frozen prop, a wall, a device, the world); only a keyed prop with an element id
// gets a row. A member already driven is kept and walked through; one past the join cap is owed, and opened when a slot
// frees, when its body sleeps or when its leader's row Ends (its row then ends on the next rest probe, so its pose
// travels the row End). Game thread.

#pragma once

#include <cstdint>

namespace coop::prop_drive_join {

// AUTHORITY: one author per element, as MTA syncs each unoccupied element by its own syncer
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:476-509). The host streams every joined
// member while a carrying client streams the leader by its hand. That holds because the constraint lives on the host alone:
// P2.3b, which gives a client a copy of the nail, must decide it again, since the client-hand Owe would then put two
// simulators on a member. Divergences from MTA's trailer link: it is learned from an event (the engine's constraint call),
// not re-read each sync pulse (reference/mtasa-blue/Client/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:289-291,
// 359-363); and no link travels on the wire (reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:308-312
// sends a joiner each link; a joiner here is sent the rows).

// HOST: `leader` is driven or held; its tied props join its drive. Queued only, so no walk and no row opens inside the
// caller. A no-op off the host, for a null actor and for one already queued. `why` names the cause for the log.
void Owe(void* leader, const char* why);

// The owed joins, walked, and the owed members served. Called at the top of prop_drive_host's tick, before its empty-table
// early return, since a join is what fills the table.
void Tick();

// HOST: a row's End has just gone to the clients (prop_drive_host's NoteEndSent, before the dev End listener). Each member
// owed to that element as its leader is read for sleep now and, asleep, given its row at once (past the cap, ended by the
// rest rule once it rests); an awake one stays owed. No-op off the host and with nothing owed. Game thread.
void OnRowEnded(uint32_t eid);

// Session end: the owed lists and counters.
void Clear();

}  // namespace coop::prop_drive_join

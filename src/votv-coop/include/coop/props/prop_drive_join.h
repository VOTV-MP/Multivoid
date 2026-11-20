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
// frees or when its body sleeps (its row then ends on the next rest probe, so its pose still travels the row End).
//
// MTA tows a trailer as a link between two synced elements, each with its own sync row, and the tractor's driver is the
// trailer's syncer (reference/mtasa-blue/Server/mods/deathmatch/logic/CVehicle.cpp:597-615); here the link is a constraint
// and the host is the syncer of both ends. Game thread.

#pragma once

namespace coop::prop_drive_join {

// HOST: `leader` is driven or held; its tied props join its drive. Queued only, so no walk and no row opens inside the
// caller. A no-op off the host, for a null actor and for one already queued. `why` names the cause for the log.
void Owe(void* leader, const char* why);

// The owed joins, walked, and the owed members served. Called at the top of prop_drive_host's tick, before its empty-table
// early return, since a join is what fills the table.
void Tick();

// Session end: the owed lists and counters.
void Clear();

}  // namespace coop::prop_drive_join

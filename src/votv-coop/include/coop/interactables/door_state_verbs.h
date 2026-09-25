// coop/interactables/door_state_verbs.h -- a base door's open state moves through its two state
// verbs, and the lane stands on both.
//
// Every live writer of a door's open state reaches it through doorOpen or doorClose -- a press, a
// hit's pry, the autoclose, a keypad's open, a creature, the blackout, a jam, a trigger; a save load
// writes it raw, and the load's connect snapshot carries it. The gate watches the two by name, and
// the swing's end, move__FinishedFunc, where isOpened settles:
//   - HOST, after the body: the channel reads the state the verb left and sends it when it changed
//     (Channel::OnLocalEdge), so a door crosses the moment it starts to swing, whoever moved it, and
//     again where the swing settles otherwise than it began (or the swing's intent fields did not
//     resolve and the state reads settled).
//   - CLIENT, before the body: a client's copy of a door the lane indexes is the host's to move, so
//     every call is refused but the door lane's own apply to that door -- its autoclose, a jammed
//     door's late unjam, a creature or a trigger on this machine would otherwise move this copy
//     alone. That covers a call made inside one of our own reflected calls too: a creature mirror we
//     move overlaps a door and opens it from inside our move, and a device replay's door call is
//     refused the same way; the host's own copy of either arrives as DoorState.
// A door the lane does not index keeps its native verbs: the lane has no name for it.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::door_state_verbs {

// Registers the three name watches. The per-tick retry pump (subsystems::Install). Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when all are live. Game thread, once per pump tick.
void Tick();

// The session ended: the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::door_state_verbs

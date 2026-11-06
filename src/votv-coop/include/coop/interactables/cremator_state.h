// coop/interactables/cremator_state.h -- the cremator's lever run crosses: the host's `isActive` reaches every client,
// whose copy drives its own handle to match.
//
// The cremator is the host's machine. Its `isActive` flips at the end of the handle's travel
// (handleLimeline__FinishedFunc): on the host that finish is the edge the keyed channel sends (CrematorState); on a
// client it is the moment the copy's handle is at rest, where the lane looks again at the want it holds. A client's
// copy never moves its handle on its own account: its `useLever` is refused unless the lane's own drive made the
// call, and the press reaches the host as a CrematorIntent (coop/interactables/cremator_intent).
//
// The channel applies a state once and keeps no want, and the handle takes seconds, so the adapter below keeps the
// want per cremator: the apply writes it and returns, and the drive (useLever) runs only when the copy's handle is at
// rest, its flag differs from the want, and, for a start, its door reads shut. The drive never closes the door: the
// host shuts its lid before it starts, and the lid lane brings that to the copy. It is looked at at exactly three
// events, the want changing, the copy's handle finishing, and the close of that copy's door, never on a poll. A
// joiner takes the same path from the channel's connect snapshot.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cremator_state {

// The keyed channel's adapter: the state a cremator reads (the want this client holds for it, else `isActive`) and
// the apply (write the want, then drive). Game thread.
bool ReadState(void* cremator, bool& on);
bool ApplyState(void* cremator, bool on);

// The host's lever run changed: `cremator`'s `isActive` is now `active`, read at the end of the handle's travel (the
// same finish that sends the edge on the wire). Raised on the host only. A listener only records.
using HostRunFn = void (*)(void* cremator, bool active);

// Adds a listener once (the same function again adds nothing). False when the fixed list is full. Game thread.
bool AddHostRunListener(HostRunFn fn);

// The cremator whose door `door` is, when this client holds a want for it, and what that want is. False for any other
// lid, one lookup (coop/interactables/cremator_client_guard asks it at every lid's open). Game thread.
bool HeldWantOfDoor(void* door, void*& cremator, bool& wantOn);

// Registers the lane's three name watches, once per session start (coop/interactables/verb_lanes). A watch the gate
// refused is final for the process and said once. Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when all are live. Game thread, once per pump tick.
void Tick();

// The session ended: the wants and counters, with one summary line.
void OnDisconnect();

}  // namespace coop::cremator_state

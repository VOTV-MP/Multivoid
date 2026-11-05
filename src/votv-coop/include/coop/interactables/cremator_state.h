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
// want per cremator: the apply writes it and returns, and the drive (the door shut first when the run starts, then
// useLever) runs only when the copy's handle is at rest and its flag differs from the want. It is looked at at exactly
// two events, the want changing and the copy's handle finishing, never on a poll. A joiner takes the same path from
// the channel's connect snapshot.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cremator_state {

// The keyed channel's adapter: the state a cremator reads (the want this client holds for it, else `isActive`) and
// the apply (write the want, then drive). Game thread.
bool ReadState(void* cremator, bool& on);
bool ApplyState(void* cremator, bool on);

// Registers the lane's two name watches. The per-tick retry pump (coop/interactables/verb_lanes), so a watch the
// gate refused is registered again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when both are live. Game thread, once per pump tick.
void Tick();

// The session ended: the wants and counters, with one summary line.
void OnDisconnect();

}  // namespace coop::cremator_state

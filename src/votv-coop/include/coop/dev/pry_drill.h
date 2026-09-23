// coop/dev/pry_drill.h -- [dev] a crowbar's pry of a stuck pryable, watched on both peers.
//
// The acting peer calls a stuck pryable's own crowbarOpen, the verb a crowbar's hit runs: the
// component's unstick with the tool, then a kick along the crowbar's axis, which with no crowbar
// is zero. Nobody holds the prop, so only the stick lane's unstick carries it to the other peers.
// Arms (`pry_drill`): host -- the host pries once a client's join is over; client -- the client
// pries once its own join is over; join -- the host pries while a joiner is still loading, which
// the joiner's snapshot or window correction must answer. Before the pry, the watching peer of the
// host and client arms runs its copy's hand unstick (the component's unstick without the tool),
// which a pryable refuses with its "Tool required" hint: the replay the receiver ran before, kept
// as the contrast. Both peers print every pryable they have, stuck or not, and every change of
// one; the acting peer prints ACTOR DONE once its copy has come to rest.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::pry_drill {

bool IsEnabled();

// The drill's pass: the acting peer's steps, both peers' watch. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step, the watch emptied. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::pry_drill

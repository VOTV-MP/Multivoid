// coop/dev/hookdrag_selftest.h -- a prop dragged by a hook, driven (`VOTVCOOP_HOOKDRAG_SELFTEST=1`
// for one run, on BOTH peers).
//
// An idle two-peer run never ties a hook to a prop, so the channel that carries a prop nobody is
// holding is never exercised. The planting peer -- the host, the client, or both, per the
// `hookdrag_role` row -- picks the nearest keyed physics prop to its own player, plants a hook into
// it with the game's own attach verb, hands it to the player the way the item does, walks the
// player out and back with the bot director so the constraint drags the prop, then destroys the
// hook so the prop coasts and rests. A client's plant exercises the whole client direction: the
// host ties the client's hook on its mirror and the channel carries the prop back; both planting is
// two host constraints on one body when the two players stand where one prop is nearest to both.
// The peer that does not plant picks nothing: it takes the prop the planter's hook bites, named by
// the element id that hook's state carries. Both peers log that prop's position on one clock, so the
// verdict is the distance between two series, and a run whose peers named different props says so.
// It MUTATES the world (the prop is moved), so it is armed per run from the environment only.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::hookdrag_selftest {

// Cache the session pointer. Call once at boot. No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Advances this peer through its phases, each ended by its event and bounded (a
// bound that passes ends the run with a FAIL line naming the phase), and samples the target on both. No-op
// with the flag off or before both peers are in the world.
void Tick();

// Print the target and how far it moved here. Called when the run ends and at teardown, since
// the rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Has this peer's drag ended: its verdict was printed, by the run's last phase or by a FAIL. "Ended"
// and not "passed", for the sleep drill's `beside` arm, which holds the host asleep until it is true
// and whose origin waits for that sleep on both peers. Game thread. False with the flag off.
bool HostEnded();

// Is the drill armed on this peer (the flag row). The sleep drill's `beside` arm needs it on the host.
bool IsEnabled();

// The longest this peer's own path through the drill can take, the sum of its phase bounds: the
// planter's pick, two legs and rest, or the other peer's wait for the pick, the tie, the release and
// the rest. For a drill that holds a peer for as long as this one runs (the sleep drill's beside
// arm). Any thread; `isHost` is the role of the peer asking.
uint64_t WorstCaseMs(bool isHost);

// Clear the phase and the target so a reconnect re-runs the drag.
void OnDisconnect();

}  // namespace coop::dev::hookdrag_selftest

// coop/dev/hostile_drill_world.h -- the world_latest drill arm, proving a client's alarm, bed and laptop power cost
// the host at most their budgets however fast the client changes them, that the newest value still lands, and that
// the laptop's replay settles on it without echoing its own edge. A joined client sends, in one tick, 120 AlarmState,
// SleepState and LaptopState power messages each alternating its own value and the other, then the other; the HOST
// judges by its own counts: once it has drained all of them, how many of each it applied, then whether each last
// value still owed was applied by a later payment (a kind the burst already left holding it owes nothing and is not
// judged), then whether its laptop settled on the last value with no edge broadcast as its own. The bed flood also
// carries forged ClientRest beds in rotation (a character outside an identifier, a nonzero byte past the length, an
// Awake report with a bed), each of which the host must refuse and count. Defensive testing
// of OUR OWN mod, per coop/dev/hostile_drill.h; nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::world {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, WorldLatest };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the flood once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::world

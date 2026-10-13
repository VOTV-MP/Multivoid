// coop/dev/hostile_drill_latest.h -- the keep_latest drill arm, proving a client's display state costs the host
// at most its budget however fast the client changes it, and that the newest value still lands. A joined client
// sends, in one tick, 120 SkinChange, NameplateChange, NickColorChange and HandItem messages each alternating two
// values, then a last value of each; the HOST judges by its own counts: once it has drained all of them, how many
// of each it applied, and then whether each kind's last value was applied by a later payment (a kind the burst
// already left holding its last value owes no payment and is not judged). Defensive testing of OUR OWN
// mod, per coop/dev/hostile_drill.h; nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::latest {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, KeepLatest };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the flood once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::latest

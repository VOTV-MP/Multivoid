// coop/dev/hostile_drill_kerfur.h -- the kerfur_flood drill arm, proving a client's kerfur menu verbs and form
// conversions cost the host at most their lanes' budgets however fast the client sends them. A joined client
// sends, in one tick, 121 KerfurCommand and 121 KerfurConvertRequest, each naming a distinct host-range id that
// nothing on the client holds; the HOST judges by its own per-sender counters of how many entered each lane's cost
// path and how many the lane refused: once all 121 of a kind are accounted for, whether the runs reached the lane's
// cap, followed the lane's pace from the first run, and never held more than the cap unserved and unrefused (the
// messages the drain handed over, less the runs and the refusals). The ids name
// no live kerfur, so a run is judged at the cost path, never at a verb's effect. Defensive testing of OUR OWN mod,
// per coop/dev/hostile_drill.h; nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::kerfur {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, KerfurFlood };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the flood once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: arm on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::kerfur

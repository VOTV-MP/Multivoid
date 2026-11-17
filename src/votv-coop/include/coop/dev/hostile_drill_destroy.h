// coop/dev/hostile_drill_destroy.h -- the bad_destroy_eid drill arm, proving a client's PropDestroy that names an
// element which is not a prop cannot destroy that element's actor. The HOST stages a victim: a bare actor it spawns
// and binds as a WORLD-ACTOR element at a fixed id, the stand-in for any non-prop element (an NPC, a kerfur, a
// building piece). A joined client sends one PropDestroy naming that id. The host judges by the victim: it counts the
// message when it reaches the destroy receiver, then reads the victim at once. Alive is PASS; dead is FAIL.
// The red `eid_type_off` (the same hostile-drill row, read by remote_prop::ResolveLiveActorByEid) resolves an eid
// whatever its element's type, so the victim dies and the arm FAILs. Defensive testing of OUR OWN mod, per
// coop/dev/hostile_drill.h; nothing here ships enabled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill::destroy {

// The sub-arm; None when the active drill arm is not ours.
enum class Arm { None, BadDestroyEid };

// The arm's name, for the verdict markers.
const char* ArmName(Arm a);

// Client (sender), game thread: once this peer's own join is over, send the PropDestroy once.
void TickClient(coop::net::Session* s, Arm a);

// Host (judge), game thread: stage the victim on the first world-ready slot, then judge and print the PASS / FAIL /
// INCONCLUSIVE marker.
void TickHostJudge(coop::net::Session* s, Arm a);

// Back to the un-armed state (disconnect / re-arm).
void OnDisconnect();

}  // namespace coop::dev::hostile_drill::destroy

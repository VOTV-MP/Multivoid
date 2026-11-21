// coop/dev/basedmove_drill.h -- drill: a pose names the mover its player stands on, and the receiver rides the puppet on it (ini
// basedmove_drill=off|ride|hostride|bad_base_xy|bad_base_vol|baseend / env VOTVCOOP_BASEDMOVE_DRILL on BOTH peers; the red is
// basedmove_base_off on the rider's peer). The mover is the base's cargo lift: a rider on its platform presses the platform
// into a puppet that lags it, and the lift's own block test reverses it.
//
// The rider is walked to the platform's open side by the director and steps onto its walkable surface through the input seam,
// never teleported; when no route reaches the floor the lift rests at, the host first brings it to its other stop with its own
// press. Each arm ends on one `rig: VERDICT basedmove-<arm> PASS|FAIL|INVALID` line (coop/session/rig_verdict.h); every verdict is
// decided by an event and a time bound says INVALID only.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::basedmove_drill {

// The arms. ride: the client rides, the host presses; every host frame logs the gap the lift reads, the puppet's distance d from
// where its pose puts it, its feet against the slab and its attachment; PASS when the lift reaches its stop, FAIL at the first
// reversal (red basedmove_base_off, the pose naming no base). hostride: the host rides and presses, the client reads the host's
// puppet against its own copy of the platform; PASS at the stop when its feet stayed within 20 cm of the slab on every frame, FAIL
// at the first frame past it (measured: 2.2 cm green, 96.5 cm with the base off, the same red). bad_base_xy and bad_base_vol: the
// client forges its claim (an offset 200 cm off its absolute pose; an absolute height past the lift's travel); PASS when the host
// sets the claim aside, counted, and its puppet follows the absolute pose, FAIL when the puppet rides the forged claim. baseend: the
// client rides; mid-travel the host destroys its copy of the platform and the puppet must still exist, hang from no parent and
// stand within 30 cm of where it stood: green only, it proves the engine's own detach.
// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::basedmove_drill

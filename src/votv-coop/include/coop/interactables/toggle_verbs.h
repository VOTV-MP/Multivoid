// coop/interactables/toggle_verbs.h -- a symmetric keyed device's state goes out at the verb that
// writes it, on the peer that ran it (such a device is its presser's: no auto-revert, so no fight).
// One watch a row, by name, and after the body, on a device of the row's class, the device's lane
// reads the state the call left and sends it when it changed (Channel::OnLocalEdge); the lane's own
// apply is its echo and sends nothing, and a client sends nothing before its world is ready (its
// load is the host's save). The rows:
//   - a light switch's use(): the one writer of its `a` -- a runTrigger 0 on its group, its click,
//     `a` negated, its mesh.
//   - a garage's runTrigger(owner, index): the one writer of its Open past its load -- Open negated
//     and its swing, unless the garage is still moving.
//   - an appliance's actionOptionIndex: a faucet's, sink's, shower's, oven's (fixed, at its switch)
//     and tape unit's toggle of its bool, then its repaint.
//   - a server box's visual(bool): the one writer of its `active`, called by a kerfur Omega.
// The group a switch triggers is the light group lane's (coop/interactables/lightgroup_verbs).

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::toggle_verbs {

// Registers each row's name watch. The per-tick retry pump (coop/interactables/verb_lanes). Game
// thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when they are live. Game thread, once per pump tick.
void Tick();

// The session ended: each row's edge count, with one summary line.
void OnDisconnect();

}  // namespace coop::toggle_verbs

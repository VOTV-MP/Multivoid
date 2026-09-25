// coop/interactables/toggle_verbs.h -- a symmetric keyed device's state goes out at the verb that
// writes it, on the peer that ran it (such a device is its presser's: no auto-revert, so no fight).
// One watch a row, by name; after the body, on a device of the row's class, its lane reads the state
// the call left and sends it when it changed (Channel::OnLocalEdge). The lane's own apply is its echo
// and sends nothing; a client sends nothing before its world is ready. The rows, each its class's
// writer past the load: a light switch's use() (`a`; its group is coop/interactables/lightgroup_verbs);
// a garage's runTrigger (Open, unless mid-swing); an appliance's actionOptionIndex (a faucet's, sink's,
// shower's, oven's and tape unit's bool) and a server box's visual(bool) (a kerfur Omega's); a
// locker's open(bool) (its toggle, an Arir's npcOpen) and the drone console's actionOptionIndex
// (its `opened`); a lid's open(bool) and close() (a prop_swinger's `opened`: its grab, damage, padlock
// and the tick's rest-close all call one of the two).

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

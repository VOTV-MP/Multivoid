// coop/interactables/lightgroup_verbs.h -- a light group's state moves through its runTrigger, and
// the lane stands on it.
//
// Every live writer of a group's isActive reaches it through trigger_lightRoot_C::runTrigger -- 0
// toggles it, gated on the group's breaker `active`; 1 sets it; 2 clears it -- whichever Blueprint
// drives the lights (a switch's use, powerControl, the gamemode, an eventer, a keyhole, a
// generator). A save load writes it raw through loadAft, and the load's connect snapshot carries it.
// The gate watches runTrigger by name:
//   - HOST, after the body: the group lane reads the state the call left and sends it when it
//     changed (Channel::OnLocalEdge).
//   - CLIENT, before the body: a client's copy of a group the lane indexes is the host's to move, so
//     every call is refused but the lane's own apply to that group. A switch this client pressed,
//     an eventer's flicker or this client's own breaker would otherwise move this copy alone; the
//     switch still flips here, and its bit reaches the host on the switch lane.
// A group the lane does not index keeps its native verb: the lane has no name for it.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::lightgroup_verbs {

// Registers the name watch. The per-tick retry pump (subsystems::Install). Game thread.
void Install(coop::net::Session* session);

// Settles the watch and says once when it is live. Game thread, once per pump tick.
void Tick();

// The session ended: the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::lightgroup_verbs

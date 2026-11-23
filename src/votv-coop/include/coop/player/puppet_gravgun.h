// coop/player/puppet_gravgun.h -- one gravity gun shot per click, however many players there are.
//
// Every mainPlayer_C carries a comp_gravitygun_C (weaponcomp_gravitygun), the puppets of the other players included.
// Its tick reads lib_C::getMainPlayer -- this machine's own player -- and, while that player holds the gravity gun
// (prop_gravgun_C) and grabs with it and the component's own `recharge` is spent, it shoots and sets its recharge. So
// each puppet's component shot for this machine's player too, on a recharge of its own: with one other player a click
// shot twice, with three players three times. A puppet's gun is its owner's, which its owner's own machine runs.
// EVERY MACHINE: the tick of a comp_gravitygun_C whose owner is a puppet is refused. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::puppet_gravgun {

void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once a tick.
void Tick();

}  // namespace coop::puppet_gravgun

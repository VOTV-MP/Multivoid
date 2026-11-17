// coop/player/hunger_share.h -- the host's rule that a larger session's players each get hungry slower.
//
// One base has one income and one food supply, and every player eats. With `net.hunger_shared` on (the
// host's rule, replicated to every peer) each player's hunger runs at players^-0.44 of the game's rate:
// 74% for two, 54% for four, so the group still eats more as it grows (2.2 times one player's food
// for four, not four times). The game multiplies its hunger tick by mainPlayer_C's
// foodDraining; each peer writes the scaled value on its own player while the value is the game's
// default or this module's last write, and leaves it alone while an effect is driving it. Stamina and
// sleep are untouched.

// Nothing here runs on a clock. The rate is written where its inputs are announced: the roster's
// occupancy edges (roster_ledger::SubscribeSlotReplaced), the rule's own change (config::Subscribe), a new
// player body (a script-gate watch on mainPlayer_C::ReceiveBeginPlay, a body born with the game's 1) and
// the end of an effect (a watch on effect_C::removeRanout). The one writer of foodDraining in the game is
// the ariral vaccine's effect, which sets it on every tick of its life (effect_ariralVaccine.cpp:79) and
// ends through removeRanout (effect.cpp:146), leaving the value at about 1.

#pragma once

namespace coop::hunger_share {

// Idempotent, called by the session's install pass on every pump tick: subscribes to the roster and the rule
// once, and registers each of the two watches until the gate has taken it. Game thread.
void Install();

// Each game tick: finish arming the watches and retry a write the player was not up for. A bool check
// when there is nothing to do. Game thread.
void Tick();

// The share of the game's hunger rate one player of `players` gets.
float RateFor(int players);

}  // namespace coop::hunger_share

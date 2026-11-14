// coop/player/hunger_share.h -- the host's rule that a larger session's players each get hungry slower.
//
// One base has one income and one food supply, and every player eats. With `net.hunger_shared` on (the
// host's rule, replicated to every peer) each player's hunger runs at players^-0.44 of the game's rate:
// 74% for two, 54% for four, so the group still eats more as it grows (2.2 times one player's food
// for four, not four times). The game multiplies its hunger tick by mainPlayer_C's
// foodDraining, which only the ariral vaccine's effect writes; each peer writes the scaled value on its
// own player while the value is the game's default or this module's last write, and leaves it alone
// while an effect is driving it. Stamina and sleep are untouched.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::hunger_share {

// Each peer, once a second. Game thread.
void Tick(coop::net::Session* session);

// The share of the game's hunger rate one player of `players` gets.
float RateFor(int players);

// The session ended: this player's rate is the game's again.
void OnDisconnect();

}  // namespace coop::hunger_share

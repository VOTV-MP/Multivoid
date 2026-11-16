// coop/creatures/hunt_gray.h -- the Grays' punch lands on the player the gray reached, not on the host.
//
// A gray (graytest_C) that a mainPlayer_C overlaps, or that arrives, calls its grayEventController_C's `despawn`, which punches
// `lib.getMainPlayer()` -- the host's player, whoever touched the gray -- and destroys the grays. A client a gray touches would
// ragdoll the host and never be punched. A client's copy does nothing on overlap (coop/world/spawn_authority refuses the
// controller's `begin` and its overlap there).
// The host's pre watch on `despawn` (the script-body gate, by name) picks the player nearest the calling gray
// (coop/creatures/hunt_target NearestTo: the toucher stands inside its sphere) and, when that is a client, has the controller's
// getMainPlayer reads answer that client's puppet for the span of the call (coop/creatures/served_player kMainPlayerAny). The
// punch lands on the puppet, where coop/player/player_damage cancels it and delivers it to the client as a PlayerHit
// (creature_hit::IsDelivered names the controller). The record is withdrawn on the next tick: the body reads the player before
// its Delay and not after. Host only; game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::hunt_gray {

// Register the pre watch on the controller's `despawn` (once; the gate resolves the name on the game thread). Host only. Idempotent.
void Install(coop::net::Session& session);

// Withdraw the records of the calls the last tick ran. Host only. Game thread.
void Tick(coop::net::Session& session);

// Whether `actor` is a grayEventController_C: its punch is a hit a creature makes, so the relay delivers it.
bool IsController(void* actor);

void OnDisconnect();

}  // namespace coop::hunt_gray

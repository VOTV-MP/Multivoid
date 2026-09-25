// coop/props/container_view_close.h -- a player's view into a container ends where the player's reach
// does. The game opens a container's view through an actor: the drone, its sack, the ATV, or a placed
// container itself (every openPropInv caller: ATV.cpp:1399, drone.cpp:1335, prop_dronesack.cpp:109,
// prop_container.cpp:307). In single-player the one player who could move that actor away -- send the
// drone, drive the ATV, carry the sack off -- cannot be looking into it at the same time, so the game
// never closes such a view. In a session another player can, and a client that stayed inside the
// drone's sack after the host sent the drone kept taking from it (F-108).
//
// Each gameplay tick on every peer, while the local player's inventory screen shows a container other
// than its own inventory, the view closes through the game's own exit() once neither the container nor
// any actor that opens it (ue_wrap::container_openers) is within the arm's reach of the player's camera,
// measured to the actor's reach sphere (engine::ActorReachSphere). The host accepts a container slice up
// to the same reach plus its pose-staleness pad (container_write_policy through intent_authority), so a
// view closes before a write of its would be refused. Coop layer; game thread.
#pragma once

namespace coop::props::container_view_close {

// A pointer chain when no container is viewed; while one is, a reach test per actor that opens it.
// Game thread.
void Tick();

}  // namespace coop::props::container_view_close

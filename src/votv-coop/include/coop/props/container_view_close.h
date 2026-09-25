// coop/props/container_view_close.h -- a client's view into a container ends where the player's reach does.
// The game opens a container's view through the drone, its sack or the ATV (each hands its container field
// to mainGamemode_C::openPropInv from its actionOptionIndex), or a container opens itself (prop_container_C
// and its subclasses, a backpack, a roomba). In single-player the one player who could move that actor away
// cannot be looking into it at once, so the game never closes such a view; in a session a client stayed
// inside the drone's sack after the host sent the drone and kept taking from it (F-108), each take the
// host refused turning into a copy. Each gameplay tick on a client, while the inventory screen shows a
// container other than the player's own inventory, the view closes through the screen's own exit() once
// neither the container nor an actor that opens it (ue_wrap::container_openers) is within the player's
// live arm length -- 200, more with a mop, a lamp or a breather in hand -- of the camera, measured to the
// actor's reach sphere (engine::ActorReachSphere). The host allows the default arm plus a 600 uu pad from
// the author's body, 70 uu below the camera, so a view closes before a write of it is refused while the
// held arm and the lags of both copies stay inside the pad. The host's own writes are never refused, and
// its view is left as single-player leaves it. Coop layer; game thread.
#pragma once

namespace coop::props::container_view_close {

// A pointer chain when no container is viewed, nothing on the host; while a client views one, a reach
// test per actor that opens it. Game thread.
void Tick();

}  // namespace coop::props::container_view_close

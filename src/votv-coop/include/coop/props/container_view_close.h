// coop/props/container_view_close.h -- a client's view into a container ends where the player's reach does.
// A container opens its view itself (prop_container_C and its subclasses, a backpack, a roomba: opencontainer hands its
// inventory component to the inventory screen's begin). In single-player the one player who could move that actor away
// cannot be looking into it at once, so the game never closes such a view; in a session a client could stay
// inside a container the host moved away and keep taking from it, each take the host refused. Each gameplay tick on a
// client, while the inventory screen shows a container, the view closes through the screen's own end() once
// neither the container nor an actor that opens it (ue_wrap::container_openers) is within the player's
// live arm length -- 200, more with a mop, a lamp or a breather in hand -- of the camera, measured to the
// actor's reach sphere (engine::ActorReachSphere). The host allows the default arm plus a 600 uu pad from
// the author's body, 70 uu below the camera, so a view closes before a write of it is refused while the
// held arm and the lags of both copies stay inside the pad. The host's own writes are never refused, and
// its view is left as single-player leaves it. The view into an ATV's trunk (the screen's begin on the ATV's own
// inventory component, coop/interactables/atv_trunk_sync) is closed the same way, the ATV standing for the container.
// Coop layer; game thread.
#pragma once

namespace coop::props::container_view_close {

// A pointer chain when no container is viewed, nothing on the host; while a client views one, a reach
// test per actor that opens it. Game thread.
void Tick();

// The rule Tick closes on, asked of any view: the container, or an actor that opens it, has a reach sphere
// within the player's live arm length of the camera. True too when the camera or the arm does not read, as
// Tick closes nothing then. The dev drills read this, so a stand is judged by the rule that closes the
// screen. Game thread.
bool ViewInReach(void* player, void* container);

// The room that rule has left, in uu: the arm plus the sphere's radius minus the camera's distance to its
// centre, the best over the container and its openers. Negative: Tick closes the view. A very negative value
// when the camera, the arm or every sphere does not read. Game thread.
float ViewReachMargin(void* player, void* container);

}  // namespace coop::props::container_view_close

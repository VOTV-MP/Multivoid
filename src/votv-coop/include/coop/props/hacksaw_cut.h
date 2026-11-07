// coop/props/hacksaw_cut.h -- a container a client's hacksaw cuts breaks on the host.
//
// The hacksaw (prop_hacksaw_C) runs only where it is held. Its tick's completion spawns the scrap of the prop it saws,
// calls the prop's `broken` and destroys it, all inside one call. The scrap crosses as the player's births
// (coop/props/prop_spawn_authoring), and a plain prop's destroy crosses by the destroy seam. A container's `broken`
// spills what it holds, and that spill would be this client's alone while its destroy took the host's copy, contents
// and all. So on a client, the saw's first birth for a container asks the host to break its own copy
// (coop/props/container_break), whose spill and destroy reach every peer; until the container's destroy, every other
// birth is the local spill and is dissolved, and the destroy itself is not sent. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::hacksaw_cut {

// Store the session and install the birth seam (BeginDeferredActorSpawnFromClass, post). Idempotent and retried; called
// every load tick.
void Install(coop::net::Session* session);

// CLIENT, from the destroy seam: whether `actor` is the container a cut handed to the host, whose destroy this peer
// does not send. Closes the cut.
bool TakesDestroy(void* actor);

// Closes a cut whose destroy never came (the completion is one call, so a cut never spans two ticks). Game thread.
void Tick();

// The session ended: the counts are said and the cut is forgotten.
void OnDisconnect();

}  // namespace coop::props::hacksaw_cut

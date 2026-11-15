// coop/world/obelisk_copy.h -- a client's copy of the falling obelisk mints nothing.
//
// obelisk_C is spawned by the level's trigger when a player walks into the armed box. Its graph is the whole event:
// the enter sound and shake, the fall on a timeline, the landing shakes, a radial impulse, seventy "rock" props
// thrown from the crater, a punch of the local player, and the static prop_obelisk_C a player later pulls out. The
// host's obelisk reaches a client as a world actor (coop/world/world_actor_mirror) and the copy runs that graph, so
// each player hears, sees and is punched by the fall on their own machine. The rocks and the pulled-out prop are the
// host's props and cross on the prop lane, so the copy's own mints of them are refused: a pre hook on the deferred
// spawn refuses a call whose source is the copy. The hook is armed from the copy's birth to its seventy-first mint
// (the obelisk prop and the rocks), or until the copy has fallen or ended.

#pragma once

namespace coop::obelisk_copy {

// CLIENT, from the world-actor mirror's spawn: `actor` is the obelisk copy just made, or the save's own obelisk
// adopted as it. A copy that has already fallen arms nothing. Game thread.
void OnCopy(void* actor);

// The copy ended, or the session did: the hook is disarmed.
void OnCopyGone(void* actor);
void OnDisconnect();

}  // namespace coop::obelisk_copy

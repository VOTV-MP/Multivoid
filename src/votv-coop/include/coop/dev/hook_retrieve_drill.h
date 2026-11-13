// coop/dev/hook_retrieve_drill.h -- [dev] an anchored hook taken back through the host, once.
// hook_retrieve_drill=run: the host ties a hook between two cups before the client's puppet and hands it to the
// lane's own commit; the client walks to its copy, aims until its look takes the hook (the pop-up) and presses E.
// join: the hook is tied before the host player before the client joins, so the client's copy is the one its save
// loaded. host / joinhost: the same two copies, and the host player takes the hook. window: the host takes it between
// the client's save being cut and its world being ready; the client's join ends with its copy gone (red nojoingone, on
// the host: the join's word is not sent, the copy stands).
// The host names its hook by key in the lobby chat and says when its puppet of the client has a pose; the client
// judges only that hook, and presses only after the puppet line.
// Reds, on the client: hook_retrieve_red=nocollision keeps the copy's collision off, as before the lane (the aim
// fan holds every pose without the look taking the hook); clientruns runs the game's pickup on the copy alone (in the
// press's own tick nothing was asked of the host, and the copy and an item are already here).
// THE HOST says the one verdict, `rig: VERDICT hook-retrieve`: PASS when its own half held (its hook gone by one grant,
// or its own pickup with its item and no grant) and the client reported `done ok` in the lobby chat, FAIL on its own
// contradiction or the client's `done fail`, INVALID on `done invalid` or any bound. Each is decided by an event.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hook_retrieve_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::hook_retrieve_drill

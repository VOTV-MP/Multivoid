// coop/dev/hook_retrieve_drill.h -- [dev] an anchored hook taken back through the host, once.
// hook_retrieve_drill=run: the host ties a hook between two cups before the client's puppet (attach_a, both ends
// anchored) and hands it to the lane's own commit; the client walks to the copy, aims until its look takes the hook
// (the pop-up), presses E. Host DONE: its hook gone by one granted pickup; client DONE: copy gone, one more hook
// item carried. join: the hook is tied before the host player before the client joins, so the client's copy is the
// one its save loaded, and the client is put beside it. host / joinhost: the same two copies, and the host player
// takes the hook: the host DONE with its item and no grant, the client DONE with its copy gone and no item.
// Reds, on the client: hook_retrieve_red=nocollision keeps the copy's collision off, as before the lane, and the
// client FAILs with no pose of the fan on the hook; clientruns runs the game's pickup on the copy alone, and the
// host FAILs with its hook still standing. "[HOOK-RETRIEVE-DRILL] FAIL" is the lane failing, "ABANDONED" the drill
// unable to do its part; the host's "[HOOK-RETRIEVE-DRILL] host DONE" ends a run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hook_retrieve_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::hook_retrieve_drill

// coop/dev/hook_retrieve_drill.h -- [dev] an anchored hook taken back through the host, once.
// hook_retrieve_drill=run: the host ties a hook between two cups before the client's puppet (attach_a, both ends
// anchored) and hands it to the lane's own commit; the client walks to the copy, aims until its look takes the hook
// (the pop-up), presses E. Host DONE: its hook gone by one granted pickup; the client's `rig: VERDICT hook-retrieve
// PASS` when the grant ran there: copy gone, one more hook item carried. join: the hook is tied before the host
// player before the client joins, so the client's copy is the one its save loaded, and the client is put beside it.
// host / joinhost: the same two copies, and the host player takes the hook: the host DONE with its item and no grant,
// the client's PASS with its copy gone and no item.
// Reds, on the client: hook_retrieve_red=nocollision keeps the copy's collision off, as before the lane, and the
// client FAILs when the aim fan has held every pose without its look taking the hook; clientruns runs the game's
// pickup on the copy alone, and the client FAILs in the press's own tick: the lane asked the host nothing, and the
// copy and an item are already here. Every verdict is `rig: VERDICT hook-retrieve PASS|FAIL|INVALID`, decided by an
// event; a bound says INVALID only, and so does a walk or a setup that cannot happen. "[HOOK-RETRIEVE-DRILL] FAIL" is
// the lane failing, "ABANDONED" the drill unable to measure.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hook_retrieve_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::hook_retrieve_drill

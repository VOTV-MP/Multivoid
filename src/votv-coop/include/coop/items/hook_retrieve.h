// coop/items/hook_retrieve.h -- an anchored hook taken back off the world with the use key, on any peer.
//
// An anchored hook is the host's (coop/items/hook_anchor); every client holds a copy, a mirror or the one its save
// loaded. The game's pickup, hook_C::actionOptionIndex, drops a prop_hook_C at the player, pockets it and destroys
// the hook; run on a client's copy it would do that there alone. So on a client it is refused and asked of the host
// by the hook's save key, the name every copy shares. The host takes its hook away without a drop when an end is
// within the sender's reach and tells every client the hook is gone, naming the taker, whose copy then runs the
// game's pickup. The host player's own pickup runs the game's and is told the same way. A refused request has no
// answer: the copy stays. A player's own hook before its second end anchors is the owner lane's (hook_sync).
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HookRetrievePayload;
}  // namespace coop::net

namespace coop::hook_retrieve {

void Install(coop::net::Session* session);

// Registers the watch on the hook's action and settles it. Called from hook_sync's tick.
void Tick();

// A HookRetrieve arrived: a client's request on the host, the host's word that a hook is gone on a client.
void OnMsg(const coop::net::HookRetrievePayload& p, int senderPeerSlot);

// CLIENT: an anchored mirror was just built from the host's record. Its collision goes back on -- loadData leaves both
// spheres query-only, as the game's own anchored hook has them -- so the player's look takes it and offers the pickup,
// and nothing is pushed by it.
void PrepareAnchoredCopy(void* mirror);

// HOST: the pickups granted to a client since the process started (the drill reads it).
uint64_t GrantedCount();

void OnDisconnect();

}  // namespace coop::hook_retrieve

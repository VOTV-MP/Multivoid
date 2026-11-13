// coop/items/hook_retrieve.h -- an anchored hook taken back off the world with the use key, on any peer.
//
// An anchored hook is the host's (coop/items/hook_anchor); every client holds a copy, a mirror or the one its save
// loaded. The game's pickup, hook_C::actionOptionIndex, drops a prop_hook_C at the player, pockets it and destroys
// the hook; run on a client's copy it would do that there alone. So on a client it is refused and asked of the host
// by the hook's save key, the name every copy shares. The host takes its hook away without a drop when an end is
// within the sender's reach and tells every client the hook is gone, naming the taker, whose copy then runs the
// game's pickup. The host player's own pickup runs the game's and is told the same way. A refused request is answered
// to its sender alone and the copy stays. A client holds one request per hook in flight: a press on that hook asks
// nothing more until the host's Gone or Refused for it, or the session's end. A player's own hook before its second
// end anchors is the owner lane's (hook_sync). Game thread.

#pragma once

#include <cstdint>
#include <string>

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

// HOST: a joiner's save was just cut. Keeps the keys of the anchored hooks the world holds, which that save holds too.
// A joiner's save is a photograph, and a Gone is not sent to a slot before its world-ready.
void CaptureJoinHooks(int slot);

// HOST: the slot's connect replay (inside the snapshot scope, ahead of the snapshot). Each hook kept at its cut that the
// host no longer holds is named gone to that slot alone, so a hook taken while the joiner loaded leaves no copy there.
// A no-op without a cut (the stale-fallback join), and for a hook still standing.
void SendJoinGone(int slot);

// A peer left: its kept keys go.
void OnPeerLeftSlot(int slot);

// HOST: the pickups granted to a client since the process started (the drill reads it).
uint64_t GrantedCount();

// CLIENT, this session: the copies the host's Gone took away with no pickup of this client's (the drill reads it).
uint64_t GoneCopyCount();

// The live hook on this machine whose save key is `key` (an adopted hook, an anchored mirror, or one a save loaded), or
// null. For the drill, which judges the one hook the host names.
void* FindHook(const std::wstring& key);

// CLIENT, this session (the drill reads them): the requests asked of the host, the pickups its grant ran here, and the
// refusals it answered.
uint64_t AskedCount();
uint64_t TakenCount();
uint64_t RefusedAnswerCount();

void OnDisconnect();

}  // namespace coop::hook_retrieve

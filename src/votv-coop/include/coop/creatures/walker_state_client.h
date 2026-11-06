// coop/creatures/walker_state_client.h -- a client's walking mannequin copy: the pose and the hunt the host sent for it.
//
// The host's walker crosses as a prop and a client's copy runs no brain (coop/creatures/walker_brain), so what the walker's own
// Blueprint decides arrives as a WalkerState {key, looking, target} (coop/creatures/walker_state_host). This module keeps one
// row per walker key, a fixed table of eight: the copy's `anim.stand` is written from `looking` (the game's own pose variable,
// the one the walker's tick writes from `looking` on the host), and `target` is kept for the copy: the gaze lane
// (coop/creatures/walker_gaze) hears it through SubscribeHunt.
//
// A state for a key whose copy is not here yet is held, and applied at the copy's birth: the prop lane binds every wire-born copy
// and every adopted twin through RegisterPropMirror, which calls OnBound, so no poll looks for it. A row ends with its copy (the
// death seam, by class) and with the session. When the table is full, the oldest row with no copy is replaced and the loss said
// once; a walker's key is up to five at a time, so a full table is a defect. Coop layer; game thread.

#pragma once

#include <string>

#include <cstdint>

namespace coop::net {
struct WalkerStatePayload;
struct WireKey;
}  // namespace coop::net

namespace coop::walker_state_client {

// The host's word for one walker: applied to its copy when there is one, held when there is not. Game thread.
void OnState(const coop::net::WalkerStatePayload& p);

// remote_prop::RegisterPropMirror bound `actor` under wire key `key`: a held state for that key is applied to it. With nothing
// held this is one comparison. Game thread.
void OnBound(void* actor, const std::wstring& key);

// Told whenever a copy's row is applied (a state arrived for a copy that is here, or the copy was bound under a held state) with
// the slot the walker hunts (0xFF none), and once more with `ended` when the copy ends, whatever its target was. `copy` is the
// copy's actor (live except at `ended`, when it is never to be read through). At most two subscribers; false when full or when
// `fn` is already subscribed. Game thread.
using HuntFn = void (*)(const coop::net::WireKey& key, void* copy, uint8_t target, bool ended);
bool SubscribeHunt(HuntFn fn);

// [dev], for the walker drill: the slot the host last said the walker of this copy hunts, 0xFF for none, for a copy with no
// row and for a copy that ended. Game thread.
uint8_t TargetOfCopy(void* copy);

// [dev], for the walker drill: the row held for the walker `key` (applied to its copy, or held without one), as the host last said
// it: the slot it hunts (0xFF none) and its `looking`. False when no state for the key ever arrived (or the row ended). Game thread.
bool RowOf(const std::wstring& key, uint8_t& target, bool& looking);

// Per gameplay tick on a client: ask again for the copy's end of play while the class has not resolved (only when the class set
// moved), then one comparison. Game thread.
void Tick();

// The session ended: every row is dropped. Game thread.
void OnDisconnect();

}  // namespace coop::walker_state_client

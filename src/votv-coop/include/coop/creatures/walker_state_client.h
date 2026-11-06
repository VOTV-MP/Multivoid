// coop/creatures/walker_state_client.h -- a client's walking mannequin copy: the pose and the hunt the host sent for it.
//
// The host's walker crosses as a prop and a client's copy runs no brain (coop/creatures/walker_brain), so what the walker's own
// Blueprint decides arrives as a WalkerState {key, looking, target} (coop/creatures/walker_state_host). This module keeps one
// row per walker key, a fixed table of eight: the copy's `anim.stand` is written from `looking` (the game's own pose variable,
// the one the walker's tick writes from `looking` on the host), and `target` is kept for the copy for the gaze lane (W5).
//
// A state for a key whose copy is not here yet is held, and applied at the copy's birth: the prop lane binds every wire-born copy
// and every adopted twin through RegisterPropMirror, which calls OnBound, so no poll looks for it. A row ends with its copy (the
// death seam, by class) and with the session. When the table is full, the oldest row with no copy is replaced and the loss said
// once; a walker's key is up to five at a time, so a full table is a defect. Coop layer; game thread.

#pragma once

#include <string>

namespace coop::net {
struct WalkerStatePayload;
}  // namespace coop::net

namespace coop::walker_state_client {

// The host's word for one walker: applied to its copy when there is one, held when there is not. Game thread.
void OnState(const coop::net::WalkerStatePayload& p);

// remote_prop::RegisterPropMirror bound `actor` under wire key `key`: a held state for that key is applied to it. With nothing
// held this is one comparison. Game thread.
void OnBound(void* actor, const std::wstring& key);

// Per gameplay tick on a client: ask again for the copy's end of play while the class has not resolved (only when the class set
// moved), then one comparison. Game thread.
void Tick();

// The session ended: every row is dropped. Game thread.
void OnDisconnect();

}  // namespace coop::walker_state_client

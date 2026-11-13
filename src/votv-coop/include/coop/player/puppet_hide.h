// coop/player/puppet_hide.h -- the one owner of a remote player's copy being hidden (AActor::bHidden).
//
// Several things hide a copy for their own reason: its player is in a nightmare (coop/player/nightmare_roll), its
// player is in a map visited below the world (coop/player/level_visit). Two writers of one flag with a latch each undo
// one another when one reason ends while the other holds. Here every reason is a bit of the slot's set: the copy is hidden
// while any bit is set, and the flag is written only when that answer, or the body it is written on, changes.
// A reason is set and cleared LEVEL-wise (the same answer again is a no-op), so a respawned body, a lost packet and a
// late join all heal on the next call. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::puppet_hide {

enum class Reason : uint8_t {
    Dream = 1,  // the player is in a nightmare
    Visit = 2,  // the player is in a map visited far below the world
};

// Sets or clears one reason for the slot's copy. A slot with no body yet only holds the reason: the body is hidden at
// the next call for it (the puppet drive's birth call, or any Set).
void Set(uint8_t slot, Reason reason, bool on);

// The slot's body was just spawned: it starts shown, so a held reason hides it now.
void OnPuppetSpawned(uint8_t slot);

// The slot's occupant left: its reasons are dropped.
void Forget(uint8_t slot);

// The session ended: every reason is dropped and every copy this hid is shown.
void OnDisconnect();

}  // namespace coop::puppet_hide

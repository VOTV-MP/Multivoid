// coop/dev/drill_hold.h -- a drill's hold on one of the game's bool switches: set it true for the span of an arm and
// put back the value it found. The explosion drill holds the local player's startInvinc (so a blast leaves a live
// player) and the gamemode's unlockFungunsInStory; the Octa drill holds startInvinc on both players. Dev only.

#pragma once

namespace coop::dev::drill_hold {

// Sets the bool `name` on `obj` to true and keeps in `held` the value to put back: `restoreTo` (0 or 1), or what the
// member held when it is -1; a `held` that is already set (0 or 1) is kept. Says so, prefixed by `tag`, when it
// changed the value. False when the object is not live or the member is unreadable. Game thread.
bool Hold(void* obj, const wchar_t* name, int& held, int restoreTo, const char* tag);

// Puts `name` on `obj` back to `held` and sets `held` to -1; does nothing for a `held` of -1. Says so, prefixed by
// `tag`, and when the object is gone. Game thread.
void Put(void* obj, const wchar_t* name, int& held, const char* tag);

}  // namespace coop::dev::drill_hold

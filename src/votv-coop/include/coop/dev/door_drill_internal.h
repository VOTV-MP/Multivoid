// coop/dev/door_drill_internal.h -- what the door drill's two files share: this copy's reading of a
// door's open state, and the AIMED legs (coop/dev/door_drill_aim.cpp). Dev only; the walker thread,
// which marshals every engine read and call to the game thread.

#pragma once

#include <string>

namespace coop::dev::door_drill::detail {

// "host" or "client".
const char* Side();

// This copy's open state: the swing's intent, or with `settled` the flag the swing's end sets. -1 when
// the read failed.
int ReadOpen(void* door, bool settled);
inline int ReadOpenIntent(void* door) { return ReadOpen(door, false); }

// Waits until this copy reads `want`, up to `boundMs`; the milliseconds it took, or -1 at the bound.
int WaitForOpen(void* door, int want, int boundMs, bool settled = false);

// Whether `door` is still the door the drill listed in this world, its slot still holding it: a walker
// holds its door for minutes, so every game-thread read and call on it asks first. Game thread.
bool DoorLive(void* door);

// AIMED (a client, the door shut): a leaf, then the frame with the door the leaf's press opened, each
// pressed through useSelectedAction once the game's interaction trace strikes it
// (coop/dev/door_drill_aim.cpp).
void AimedLegs(void* door, const std::wstring& name);

}  // namespace coop::dev::door_drill::detail

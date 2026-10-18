// coop/dev/drill_hand.h -- a drill's way of putting an item into the local player's hand: the hold slot written,
// then the game's own updateHold, the pair a player's pick of the item runs. A drill that thread-hops (door_drill)
// wraps the call in its own game-thread task; a drill that ticks on the game thread (creature_hit_drill) calls it
// directly.
#pragma once

namespace coop::dev::drill_hand {

// The local hand takes `item` of class `cls` (list_props' row name and its class leaf), or empties with the item
// `L"None"` and no class. False when there is no local player or either write fails. Game thread only: a drill on
// another thread runs it through game_thread::RunAndWait, which itself refuses to be called from the game thread.
bool TakeIntoHandNow(const wchar_t* item, const wchar_t* cls);

}  // namespace coop::dev::drill_hand

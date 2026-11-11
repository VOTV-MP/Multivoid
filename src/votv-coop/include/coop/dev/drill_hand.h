// coop/dev/drill_hand.h -- a drill's way of putting an item into the local player's hand. The game equips through the
// hotbar from a world actor (mainPlayer_C's Add Object to Free Hotbar Slot), so the item is spawned from its class at
// the player and taken the way a pickup takes it. A drill that thread-hops (door_drill) wraps the call in its own
// game-thread task; a drill that ticks on the game thread (creature_hit_drill) calls it directly.
#pragma once

namespace coop::dev::drill_hand {

// The local hand takes a fresh `item` of class `cls` (list_props' row name and its class leaf), or empties with the
// item `L"None"` and no class. Whatever the hand held first is destroyed (its hotbar slot freed), so the hotbar does
// not fill across a drill's takes. True once the hand actor is live and of class `cls` (or the hand is empty, for "None");
// false, with the reason logged, when the class does not load, the spawn or the take is refused, or the hand does not
// settle. Game thread only: a drill on another thread runs it through game_thread::RunAndWait, which itself refuses to
// be called from the game thread.
// `fuel`, when given, is the amount a gas can in the hand carries (the can's invRead_amount, written before the take).
bool TakeIntoHandNow(const wchar_t* item, const wchar_t* cls, const float* fuel = nullptr);

}  // namespace coop::dev::drill_hand

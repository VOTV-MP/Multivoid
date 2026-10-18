// ue_wrap/world/bad_sun.h -- the Bad Sun: the world-event actor badSun_C, read per copy. 0.2 s after its BeginPlay
// a copy registers itself, then tests the hour every 5 s, first at 5.2 s; at the first test inside 7 <= hour <= 18
// it calls lib_C::setEvent(true, this), creates a ui_badSun widget on the local viewport, stores it in its `widget`
// member and stops polling (the decompiled badSun Blueprint, its ubergraph's hour test). So a copy is ACTIVE iff
// `widget` is set, and an active copy sits in the gamemode's activeEvents_senders. The join design that rests on
// this reads it as every peer's own copy. The gamemode holds one in `badSun`, written by its own `Spawn Bad Sun`
// (which returns at once while one is held) and cleared by the copy's remove(). Engine access only; no network or
// co-op state. Game thread.
#pragma once

namespace ue_wrap::bad_sun {

// Resolve the `widget` member on badSun_C, the class from the object index. False while the class is not loaded
// (no copy can exist: not a failure). Once it has loaded the member resolves on that pass or never: a failed pass
// latches OFF with one warning. True once the member is in hand.
bool EnsureResolved();

// The most copies CountLive counts and hands out: a second copy is a finding, and more than a few cannot exist.
inline constexpr int kMaxLive = 8;

// The number of badSun_C instances of the running world (the class default object and a departed world's copies
// do not count), after the index has applied every announced birth and death, capped at kMaxLive; `copies`, when
// given, receives the first min(count, cap) of them and null in every other slot below cap. A class that is not
// loaded counts 0. -1 when the index could not drain (off the game thread, or inside a drain): the caller asks
// again.
int CountLive(void** copies, int cap = 1);

// Read whether `copy`'s widget is set, the design's definition of an active copy. Returns whether it could read:
// false, with `active` false, for null or while the member has not resolved (unknown, not "inactive").
bool ReadActive(void* copy, bool& active);

// Read the alpha `a` of `copy`'s widget: the ramp up to 1 over 60 s from its creation, and, once the hour is 18 or
// later, the fade down to 0 over 60 s of real time, at which the widget resets the sky and calls the copy's remove().
// Returns whether it could read: false, with `alpha` 0, for null, for a copy with no live widget (inactive) and while
// the member did not resolve.
bool ReadWidgetAlpha(void* copy, float& alpha);

// Read whether `copy` is among the gamemode's activeEvents_senders, the list setEvent(true) appends its caller to.
// Returns whether it could read: false, with `sender` false, for null or while that list does not read.
bool ReadIsSender(void* copy, bool& sender);

// Read whether the running world's gamemode (or `gamemode`, when given) holds a live copy in its `badSun` member,
// the field `Spawn Bad Sun` tests and writes. Returns whether it could read: false, with `present` false, when
// there is no gamemode or the member did not resolve.
bool ReadGamemodeCopy(void* gamemode, bool& present);

// Whether the running gamemode holds a live day cycle in its own `daynightCycle` member: the object a copy's poll
// reads the hour from and its widget the sun from. A world without one cannot show a Bad Sun. Returns whether it
// could read: false, with `has` false, when the gamemode's `daynightCycle` member did not resolve. No gamemode reads
// true with `has` false.
bool ReadHasCycle(bool& has);

// The running gamemode's own `Spawn Bad Sun`, reached through ProcessEvent: it spawns a copy and stores it in
// `badSun` unless one is held. False when there is no gamemode or the verb did not resolve; true when it was
// called, whatever its body then did.
bool CallSpawn();

// Destroy `copy` (K2_DestroyActor) when it is inactive, that is its widget is unset: it never ran setEvent(true),
// so no refcount and no achievement is owed. False, and nothing done, for null, for an active copy and for one
// whose state could not be read.
bool DestroyInactive(void* copy);

}  // namespace ue_wrap::bad_sun

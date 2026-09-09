// ue_wrap/devices/door_box.h -- standalone engine access for the hinged-door storage boxes: the
// base and map LOCKERS (Alocker_C and its pure subclasses locker_personal_C, locker_death_C) and
// the DRONE-CALL CONSOLE box (AdroneConsole_C). Principle-7 engine-wrapper layer -- class
// resolve, the `opened` state, the native apply verbs and the far-peer frozen-timeline
// force-snap. No network logic: coop::interactable_sync's door-box channel drives the mirror
// through here.
//
// Both classes are plain AActor with no save Key, so identity is the level-export actor
// FName, deterministic cross-peer. State is the `opened` bool; the swing is a 0.5 s Timeline
// that FREEZES outside the local player's tick range, which is why an apply is verified and
// force-snapped through the timeline's alpha and direction fields plus a__UpdateFunc and
// a__FinishedFunc. The locker has a full native verb Open(bool) -- sound, swing, collision,
// trigger side-effects -- while the console has none, so there it is a write to `opened` plus
// setButtonsCollision() and driving the Timeline. The radiotower doors on the mast are
// prop_swinger_C child actors, already synced by the container channel, and need nothing here.

#pragma once

#include <string>

namespace ue_wrap::door_box {

// Resolve locker_C + droneConsole_C UClasses (operational when EITHER is up),
// the per-class `opened` offsets + timeline fields, and the verbs. Idempotent;
// retried until at least one class is loaded. Game thread.
bool EnsureResolved();

// True iff `obj` is a locker_C/droneConsole_C descendant.
bool IsDoorBox(void* obj);

// The cross-peer identity: the level-export actor FName ("locker22",
// "droneConsole_2", ...). Deterministic for placed actors. Empty on failure.
std::wstring GetNameKey(void* actor);

// Read `actor`'s opened bool into `out`. False if unresolved / wrong class.
bool TryReadOpened(void* actor, bool& out);

// Apply `want` natively: locker -> the BP verb Open(want); console -> write
// opened + setButtonsCollision() + Timeline Play/Reverse. Registers the actor
// in the verify queue (force-snap if the swing froze out of tick range).
// Game thread.
bool ApplyOpened(void* actor, bool want);

// Drain the verify queue: swings that completed are dropped; swings frozen past
// their deadline are force-snapped (alpha + direction write + a__UpdateFunc +
// a__FinishedFunc). Cheap no-op when idle. Call once per net-pump tick.
void TickVerify();

// Session teardown: drop any mid-swing verify entries (a stale entry must not
// force-snap into the next session's / SP world state).
void OnDisconnect();

}  // namespace ue_wrap::door_box

// ue_wrap/door_box.h -- standalone engine access for the hinged-door storage
// boxes: the base/map LOCKERS (Alocker_C + its pure subclasses locker_personal_C
// / locker_death_C) and the DRONE-CALL CONSOLE box (AdroneConsole_C). Principle-7
// engine-wrapper layer: class resolve, the `opened` state, the native apply
// verbs, and the far-peer frozen-timeline force-snap. NO network logic --
// coop::interactable_sync's door-box Channel drives the mirror through here.
//
// RE (votv-lockers-boxes-door-RE-2026-06-11.md): both classes are plain AActor
// (no save Key -- identity is the level-export actor FName, deterministic
// cross-peer), state = `opened` (locker @0x0270, console @0x0298), swing = a
// 0.5 s Timeline that FREEZES outside the local player's tick range (the
// door.cpp lesson) -> verify + force-snap via the timeline alpha/direction
// fields + a__UpdateFunc/a__FinishedFunc. The locker has a full native verb
// `Open(bool opened)` (sound + swing + collision + trigger side-effects); the
// console has NO public verb -> write `opened` + setButtonsCollision() + drive
// the Timeline (the garage write+refresh precedent). The radiotower "box on the
// mast" doors are prop_swinger_C child actors -- already synced by the
// container channel, no code here.

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

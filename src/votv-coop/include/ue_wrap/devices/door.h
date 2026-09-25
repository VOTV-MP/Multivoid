// ue_wrap/devices/door.h -- engine access for the base doors (door_C and its two subclasses, the
// pryable and the scaled door). Engine-wrapper layer: the reflection, struct-offset and UFunction
// details of a door actor; no network or coop state, which the interactable sync owns and drives
// through here. A door is a trigger-base descendant: its open state is its own isOpened bool, its
// cross-peer-stable identity the Key name it inherits from the trigger base (assigned by the
// gamemode's key pass and save-persistent), and its canonical state verbs doorOpen and doorClose,
// each taking a bypass flag. A player reaches those through the door's entry verbs: the press, the
// hit and, on the pryable door, the crowbar's pry. Both subclasses inherit the rest unchanged, so
// resolving against door_C covers all three.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::door {

// Resolve the door class, its key and open-state offsets and its open and close UFunctions.
// Idempotent; true once they resolved; false while the class is not loaded, and for a class whose
// required fields or verbs did not resolve by name (said once). Game thread.
bool EnsureResolved();

// True if `obj`'s class is door_C or a subclass (the pryable or the scaled door). Cheap, a bounded
// super walk with no allocation. False if not yet resolved.
bool IsDoor(void* obj);

// Read the door's inherited key name as a wide string. Empty on failure (null, or not
// resolved); an unkeyed door returns None.
std::wstring GetKeyString(void* door);

// Read the door's open state into `open`. False if the read could not be made (null, or not
// resolved), leaving `open` untouched. This is the animation-completed flag, which flips only
// when the swing reaches the end, about half a second after the press; diagnostics and
// is-it-actually-open callers want this, and the door lane wants the intent reader below.
bool TryReadOpen(void* door, bool& open);

// Like TryReadOpen but the swing intent rather than its completion: while the door's move timeline
// plays, the destination is the door's own dir, which doorOpen and doorClose write in their own
// body, so an open or close is reported the instant the verb returns instead of half a second later
// when the open state settles; any other motion (the jam shake) and a settled door read the open
// state. A byte read while the door moves, none at rest. The door lane's reader at a verb, in its
// apply, its snapshot and its shadow poll.
bool TryReadOpenIntent(void* door, bool& open);

// The door's own entry verbs, the ones a player's press, a melee hit and a crowbar's pry run.
// The host runs them on its copy for a client (coop/interactables/door_verb_intent), so the
// door's own body decides the whole press or hit once, on the authority. Each dispatches the
// instance's own override. False on a null door, an unresolved UFunction or a failed dispatch.
// Game thread.
//
// actionOptionIndex(player, hit, action, lookAtComponent), the press: the door's body reads none
// of its parameters (it goes to the power gate and its blackout clause, the moving check, the
// alienated branch and the toggle), so `player` is passed as the presser and the rest stay empty.
bool CallPress(void* door, void* player, uint8_t action);
// The one action a door offers a player (door_C::getActionOptions answers [4]): a press's `action`.
constexpr uint8_t kUseAction = 4;
// addDamage(actor, damage, hit, impact, skipSetting), a hit: the body reads only `damage`, which
// moves both panels toward open, and past the pry threshold the door opens.
bool CallHit(void* door, void* instigator, float damage);
// door_pryable_C::crowbarOpen(pryingCrowbar), a crowbar's pry: a hit of 100 on itself. False on a
// door that is not pryable.
bool CallCrowbarOpen(void* door);

// The canonical open and close. `bypass` is the blueprint's bypass-check parameter (skip the
// keycard, password and jam guards), always true on the receiver, since the sender already
// validated. Both dispatch a UFunction and must run on the game thread. False on a null door
// or an unresolved UFunction.
bool CallDoorOpen(void* door, bool bypass);
bool CallDoorClose(void* door, bool bypass);

// Read the door's power flag, the field its open gate reads: a keypad's setActive writes it, as
// do power triggers and the save. False on a null door or an unresolved field. Game thread.
bool TryReadActive(void* door, bool& on);

// The actors the door's own sensor holds now: sensorOverlaps, which the sensor's begin- and
// end-overlap handlers fill with every Pawn and prop that enters and leaves it; the autoclose
// reads its length every five seconds and closes the door at the first reading that finds it
// empty. Copies up to `maxOut` pointers into
// `out` and returns the array's length, or -1 when the field did not resolve by name, the door is
// null, or the array's header does not read as one. A field read, no dispatch. Game thread.
int ReadSensorOverlaps(void* door, void** out, int maxOut);

// Where that sensor is: the box component's world centre and its scaled half-extent, through the
// box's own GetScaledBoxExtent. The autoclose counts the list above, not the box, so a reader that
// needs what the door counts reads the list. False when the component or the call does not
// resolve. Game thread.
bool ReadSensorBox(void* door, FVector& centre, FVector& halfExtent);

// A part of the door by the name its graph gives the component: its `frame`, or a leaf, `door_L` or
// `door_R`. Null on null, an unresolved name or a dead component. For a dev drill's aim only: each call
// walks the class's properties by name. Game thread.
void* PartOf(void* door, const wchar_t* name);

// Force-snap to a state, independent of proximity. A door's open and close is a timeline
// animation that advances only while the door actor ticks, so a door this peer stops ticking
// freezes mid-animation and its open state is never set; how far that takes is not measured (a
// host copy 694 m from its own player swung and autoclosed in the door drill). The force variants
// complete the state without the animation: write the timeline alpha to the end and the direction,
// then call the door's animation-finished handler, which sets the open state and snaps the mesh to
// the final pose. A receiver's apply falls back on them when its swing has not ended in time. Game
// thread.
void ForceOpen(void* door);
void ForceClose(void* door);

// Apply a door state with the right visual for this peer: near the local camera (visible and
// within tick range) the native animated swing; far, the force-snap, invisible anyway, where
// the native animation would freeze out of tick range. Near peers animate and far peers snap,
// so doors are smooth where seen and correct everywhere. Skips re-triggering a door already
// animating toward the same target. Game thread.
void SmartApply(void* door, bool open);

// Drain the smart apply's verify list: doors whose native swing completed are dropped, and
// doors whose swing froze beyond tick range past their deadline are force-snapped, so their
// state is still correct. A cheap no-op with nothing mid-apply. Once per pump tick. Game
// thread.
void TickSmartApply();

}  // namespace ue_wrap::door

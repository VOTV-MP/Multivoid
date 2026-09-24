// ue_wrap/devices/door.h -- engine access for the base doors (door_C and the pryable door).
// Engine-wrapper layer: the reflection, struct-offset and UFunction details of a door actor;
// no network or coop state, which the interactable sync owns and drives through here. A door
// is a trigger-base descendant: its open state is the inherited isOpened bool, its
// cross-peer-stable identity the inherited Key name (assigned by the gamemode's key pass and
// save-persistent), and its canonical state verbs doorOpen and doorClose, each taking a bypass
// flag. A player reaches those through the door's entry verbs: the press, the hit and, on the
// pryable door, the crowbar's pry. The pryable door inherits the rest unchanged, so resolving
// against door_C covers both classes.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::door {

// Resolve the door class, the inherited key and open-state offsets and the open, close and
// settime UFunctions. Idempotent; true once everything resolved (false while the blueprint
// class is not loaded yet; the caller retries on a later tick). Game thread.
bool EnsureResolved();

// True if `obj`'s class is door_C or a subclass (the pryable door). Cheap, a bounded super
// walk with no allocation. False if not yet resolved.
bool IsDoor(void* obj);

// Read the door's inherited key name as a wide string. Empty on failure (null, or not
// resolved); an unkeyed door returns None.
std::wstring GetKeyString(void* door);

// Read the door's open state into `open`. False if the read could not be made (null, or not
// resolved), leaving `open` untouched. This is the animation-completed flag, which flips only
// when the swing reaches the end, about half a second after the press; diagnostics and
// is-it-actually-open callers want this, and the host poll wants the intent reader below.
bool TryReadOpen(void* door, bool& open);

// Like TryReadOpen but the swing intent rather than its completion: while the door is moving
// the destination is the move direction, set at swing start, so an open or close is reported
// the instant it begins instead of half a second later when the open state settles; a settled
// door reads the open state (the direction holds the last swing's value, which agrees). The
// door channel's poll reader: it makes the host broadcast a door it opens at swing start,
// matching the client's input-edge request, since a client's opens mirrored frame-perfect on
// the host while the host's own lagged behind the poll waiting for the swing to complete.
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

// Write the door's power flag, the field the open gate reads. The keypad's accept unlocks its
// door by setting it (the password lock's open sets the door active); the host-authoritative
// keypad accept uses it so an unlocked door stays openable after the code, whether or not the
// native keypad chain completed. A plain field write, no UFunction. Game thread.
void SetActive(void* door, bool on);

// The actors the door's own sensor holds now: sensorOverlaps, which the sensor's begin- and
// end-overlap handlers fill with every Pawn and prop that enters and leaves it; the autoclose
// reads its length every five seconds and closes the door at the first reading that finds it
// empty. Copies up to `maxOut` pointers into
// `out` and returns the array's length, or -1 when the field did not resolve by name, the door is
// null, or the array's header does not read as one. A field read, no dispatch. Game thread.
int ReadSensorOverlaps(void* door, void** out, int maxOut);

// Where that sensor is: the box component's world centre and its scaled half-extent, through the
// box's own GetScaledBoxExtent. The sensor is not the doorway: a player can stand in an open
// doorway outside it, and the autoclose then closes the door on them as it does in single player.
// False when the component or the call does not resolve. Game thread.
bool ReadSensorBox(void* door, FVector& centre, FVector& halfExtent);

// The host-authoritative client suppression. A door re-drives its own state: when a swing ends
// open it checks its sensor every five seconds and closes at the first check that finds the
// sensor empty. On a client that is a second authority beside the host's, closing on its own
// clock a door the host keeps open, so the MTA single-syncer shape applies: the non-authority
// disables its local simulation and its doors are render-only. The suppression writes autoclose
// off, the flag the check reads before it arms, and caches the original so the restore puts it
// back at disconnect. Idempotent per door. Game thread.
void SuppressClientAutonomy(void* door);
void RestoreClientAutonomy(void* door);

// Force-snap to a state, independent of proximity. A door's open and close is a timeline
// animation that advances only while the door actor ticks, and the engine throttles ticks for
// actors far from a player, so an open on a door whose local player is far freezes
// mid-animation and the open state is never set. The force variants complete the state
// without the animation: write the timeline alpha to the end and the direction, then call the
// door's animation-finished handler, which sets the open state and snaps the mesh to the final
// pose; measured reliable on far, frozen doors. This is how a renderer or host sets a door's
// state regardless of where its own player is. Game thread.
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

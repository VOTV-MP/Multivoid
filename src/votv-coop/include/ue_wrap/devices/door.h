// ue_wrap/devices/door.h -- engine access for the base doors (door_C and the pryable door).
// Engine-wrapper layer: the reflection, struct-offset and UFunction details of a door actor;
// no network or coop state, which the interactable sync owns and drives through here. A door
// is a trigger-base descendant: its open state is the inherited isOpened bool, its
// cross-peer-stable identity the inherited Key name (assigned by the gamemode's key pass and
// save-persistent), and its canonical entry points doorOpen and doorClose, each taking a
// bypass flag. The pryable door inherits all of these unchanged, so resolving against door_C
// covers both classes.

#pragma once

#include <string>

namespace ue_wrap::door {

// Resolve the door class, the inherited key and open-state offsets and the open, close and
// settime UFunctions. Idempotent; true once everything resolved (false while the blueprint
// class is not loaded yet; the caller retries on a later tick). Game thread.
bool EnsureResolved();

// The door class pointer, null until resolved; exposed for the descendant check.
void* DoorClass();

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

// True if a player's manual press would open or toggle this door right now, per the door's
// own logic: the press path gates on the door's power flag before toggling, and the toggle
// itself needs the door neither jammed nor super-closed, so the answer is the power flag and
// not jammed and not super-closed. This replaced a check that read the keypad's accept flag,
// which is a crosshair-hover flag rather than accept state, so a powered door read as locked
// and the host denied the client's open. Reads struct fields only, no dispatch; cheap, once
// per open request. Game thread. True (fail open) if the door is null or the offsets are
// unresolved, so a resolution failure never silently locks every door.
bool CanOpen(void* door);

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

// Read the door's power flag. True on null or unresolved, failing open like the gate. Callers
// save the flag before a temporary clear so the restore puts back the real value: a locked
// door's false must survive the press dispatch, since restoring a hard-coded true silently
// unlocked locked doors on the client.
bool GetActive(void* door);

// The open and close UFunction pointers, for the sync's POST observer registration; null
// until resolved.
void* DoorOpenFn();
void* DoorCloseFn();

// The host-authoritative client suppression. A door's open state is re-driven every tick by
// its local sensor and autoclose logic (an empty sensor with autoclose closes the door). On a
// client this fights the host's authoritative state: the host's real player holds a door open,
// the client's door, whose sensor the host's puppet does not trigger, autocloses, and the two
// oscillate forever. The MTA single-syncer fix, the non-authority disabling its local
// simulation, makes client doors render-only: the suppression writes autoclose off so the
// client door cannot auto-revert an applied host state, and the original value is cached so
// the restore can put it back at disconnect. Idempotent per door. Game thread.
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

// The host-side held-door suppression, the cycle's deeper half: when a client opens a door
// the host opens its copy too, but the host has no local player at that door, only the
// client's puppet, which does not hold the host's sensor. The host's native sensor check then
// finds an empty sensor with autoclose and closes the door it just opened, broadcasts the
// close, the client re-requests the open, and the door cycles about once a second; the close
// arrives through the sensor check, so disabling the sensor is the load-bearing lever and
// autoclose alone is not enough. The fix, again the single-syncer shape, makes the host treat
// a door a remote client holds open as render-only too, the same recipe as the client:
// autoclose off and the sensor's overlap events off. The suppression runs once per door when
// the host applies a remote open, lazily, never per tick or in bulk; the release restores the
// authored autoclose, re-enables the sensor and closes the door (the client released its
// hold), so native autonomy resumes. Cached in a host-side map distinct from the client one.
// Game thread.
void SuppressHostHeldDoor(void* door);
void ReleaseHostHeldDoor(void* door);

}  // namespace ue_wrap::door

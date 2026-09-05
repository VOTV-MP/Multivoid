// ue_wrap/devices/passwordlock.h -- engine access for the password keypads: the reflection,
// struct-offset and UFunction details of a keypad actor, no network or coop state (the
// keypad sync owns those and drives the mirror through here). A keypad is a trigger-base
// descendant that gates a door; its cross-peer identity is the inherited trigger key, which
// the save persists. How the mirror works: every keypad verb dispatches
// blueprint-internally, past our ProcessEvent detour, so no observer fires on them and the
// sync polls state. The digit-input verb is callable and appends to the typed buffer
// natively, driving the keypad's own validator, so the receiver mirrors typing by replaying
// digits: on the host, the client's replayed digits make the host's native keypad accept
// the code itself. The accept and deny flags are not mirrored: they are crosshair-hover
// flags, not state, and writing both onto a mirror lit a non-native purple LED; the door
// lock keys on the door's own active flag. The keypad's own active flag is mirrored (a
// direct write plus the update repaint, never the set-active verb, which cascades a power
// change into a separate light actor): the update verb picks red when neither reset nor
// active, the cancel's open(false) clears it, and it equals the door's power.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::passwordlock {

// Resolve the keypad class, the field offsets (the key, the typed buffer) and the input,
// reset and update UFunctions. Idempotent; true once everything resolved (false while the
// class is not loaded, and the caller retries on a later tick). Game thread.
bool EnsureResolved();

// True iff `obj`'s class is the keypad class or a subclass. Cheap (a bounded superclass
// walk, no allocation). False if not yet resolved.
bool IsPasswordLock(void* obj);

// The keypad's trigger key as a wide string. Empty on failure (null or not resolved); None
// if unkeyed.
std::wstring GetKeyString(void* lock);

// The mirror-relevant state of one keypad: the typed buffer and the active LED and power
// flag. The sender polls this each tick and broadcasts on change. The accept and deny flags
// are hover flags, not state (see the header).
struct State {
    std::wstring buffer;          // inPassword -- the digits typed so far (display)
    bool         active = false;  // the LED selector (red when false) and the door power
};

// Read `lock`'s mirror state into `out`. False if the read could not be made (null or not
// resolved); `out` is untouched on failure. Game thread.
bool ReadState(void* lock, State& out);

// The accept-chain context primitives, game thread. The accept truth: typing digits grows the
// buffer, and at five characters the blueprint auto-submits open(password equals buffer), so
// long codes validate natively on every peer from the digit replay alone; short codes
// submit only on the accept press or the cancel, mirrored cross-peer as a keypad event and
// CallOpen below. A host-side re-derivation from buffer equality accepted without the press,
// and is gone.

// The door this keypad gates, or null (none, dead, or not resolved). Game thread.
void* GatedDoor(void* lock);

// True iff the keypad is in set-a-new-code mode: a correct entry writes the password instead
// of opening the door, so the accept-open must be skipped for it. Game thread.
bool IsResetMode(void* lock);

// True iff the local crosshair is hovering the accept or deny button of this keypad (the
// look-at-driven hover flags, set per frame from what the crosshair points at). Not state
// and never mirrored; read locally as the press discriminator: an active flip observed by
// the poll while the crosshair sits on a submit button is a deliberate press, not an ambient
// power or apply transition. Game thread.
bool IsPressHover(void* lock);

// The receiver apply primitives, game thread. Replay one typed digit: dispatches the input
// verb, which appends to the buffer the native way (display and beep). This is how the
// receiver mirrors typing; not a submit. False on null, an unresolved UFunction or an
// out-of-range digit.
bool CallInputNumber(void* lock, int32_t digit);

// Clear the typed buffer with no side effects: a direct length-zero write (the canonical
// empty array; data and capacity retained as slack and freed by the engine on the next
// append or reassign, so no leak). The same direct-write philosophy as WriteActive, and
// deliberately not the blueprint's reset verb, which is the set-a-new-code mode (it sets
// the reset flag, a blue LED): mirroring a client's cancel through it turned the host blue.
// The caller repaints the now-empty panel via CallUpd. False on null or unresolved. Game
// thread.
bool ClearBuffer(void* lock);

// A best-effort visual refresh after the buffer change: dispatches the keypad's own update
// verb, so a tick or event-driven LED or material repaints from the freshly written state.
// An update verb, not a submit; a no-op if absent.
void CallUpd(void* lock);

// Dispatch the keypad's native submit handler, open(active), exactly what the typist's
// accept or cancel press runs internally: it sets active (the LED green or red), plays the
// success or deny sound, clears the buffer, and propagates the lock state to the pair keypad
// and the gated door. It unlocks or locks; it never moves the door (the door-open chain is a
// scripted trigger entry, not the player accept). The receiver-side replication of a
// short-code submit (long codes never need it, since the digit replay runs the auto-submit
// on every peer). May run latent sub-chains; never assume synchronous state. False on null
// or unresolved. Game thread.
bool CallOpen(void* lock, bool accept);

// A direct write of the keypad's active flag (the LED selector; false is red). Deliberately
// not the set-active verb, which cascades a power change (driving a separate light actor,
// the purple bug) and re-propagates to the pair and the door. The receiver writes this, then
// repaints via CallUpd, and separately drives the gated door's power through the door
// wrapper (keeping the keypad and door active flags equal, as in single player), so no
// cascade is needed. False on null or unresolved. Game thread.
bool WriteActive(void* lock, bool active);

}  // namespace ue_wrap::passwordlock

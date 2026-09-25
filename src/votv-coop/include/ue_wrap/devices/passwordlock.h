// ue_wrap/devices/passwordlock.h -- engine access for the password keypads (passwordLock_C), no
// network or coop state: the keypad lane in coop/interactables drives a keypad through here. Its
// cross-peer identity is the inherited trigger key, which the save persists. From its bytecode:
//   - inputNumber(num): a digit appends to the typed buffer and at five digits submits, open(password
//     == buffer); a negative num submits on the accept key, cancels (open(false)) on the cancel
//     key and submits elsewhere, the keys being the hover flags the aiming player's look-at wrote.
//   - open(active): after 0.2 s takes the buffer as the new password (set-new-code mode) or sets
//     `active` to the verdict with its sound; then empties the buffer and runs setActive(false).
//   - setActive(isPairCall): repaints, and with isPairCall false hands `active` on to the paired
//     keypad and to the gated door's own `active`, which locks or unlocks that door.
//   - open2(): the scripted guesser (locks, beeps, unlocks, opens the door); reset(): set-new-code
//     mode unless protected; falseEnterEvent(): a run of beeps and a deny.
// Every field and function resolves by name; a class whose one does not is left out whole, said
// once, never read at a remembered offset.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::passwordlock {

// Resolve the keypad class, its fields and its verbs. Idempotent; false while the class is not
// loaded (the caller retries on a later tick) and for good once a name failed to resolve. Game
// thread.
bool EnsureResolved();

// True iff `obj`'s class is the keypad class or a subclass. A bounded superclass walk, no
// allocation. False if not resolved.
bool IsPasswordLock(void* obj);

// The keypad's trigger key as a wide string. Empty on failure; "None" if unkeyed.
std::wstring GetKeyString(void* lock);

// A keypad's state: the typed buffer, the verdict it last took (also the power it hands on), the
// set-new-code mode and the password a submit is judged against.
struct State {
    std::wstring buffer;          // inPassword
    bool         active = false;
    bool         isReset = false;
    std::wstring password;
};

// Read `lock`'s state into `out`. False (and `out` untouched) on null or unresolved. Game thread.
bool ReadState(void* lock, State& out);

// The accept and cancel hover flags this copy's look-at last wrote: what the local player's press
// with no digit means. Game thread.
bool ReadHover(void* lock, bool& onAccept, bool& onCancel);

// Whether the typed buffer equals the password: the verdict a submit takes, as the keypad's own
// submit computes it. False on null or unresolved. Game thread.
bool BufferMatchesPassword(void* lock);

// Whether an open's chain is in flight on this copy: `entering`, set by open until its 0.2 s tail has
// run. Game thread.
bool IsEntering(void* lock);

// The paired keypad this one hands its state to, or null (none, dead, or unresolved). Game thread.
void* PairOf(void* lock);

// Whether this keypad or its pair is protected, the case in which reset() changes nothing. Game thread.
bool IsProtected(void* lock);

// The keypad's own verbs, dispatched as its graph calls them. Each returns false on null, an
// unresolved function or a failed dispatch. open and open2 continue on latent delays, so their
// state lands later. Game thread.
bool CallInputNumber(void* lock, int32_t digit);  // 0..9
bool CallOpen(void* lock, bool accept);
bool CallOpen2(void* lock);
bool CallReset(void* lock);
bool CallFalseEnter(void* lock);
bool CallSetActive(void* lock, bool isPairCall);

// The raw writes of a state reconcile, each false on null or unresolved; the two strings are written
// through the engine's allocator. The caller then runs setActive(false), which repaints and hands
// the power on. Game thread.
bool WriteActive(void* lock, bool active);
bool WriteResetMode(void* lock, bool on);
bool WriteBuffer(void* lock, const std::wstring& digits);
bool WritePassword(void* lock, const std::wstring& password);

// The door this keypad gates, or null (none, dead, or unresolved). Game thread.
void* GatedDoor(void* lock);

// A drill's stand-ins for a player at the keypad: a press off the digit keys (inputNumber(-1), whose
// meaning the hover flags decide), the hover flags an aim would write, and a key of the keyboard as
// the focused keypad's playerAnykey receives it, by the engine's key name ("NumPadOne", "Add").
// Game thread.
bool CallPressOffDigits(void* lock);
bool WriteHover(void* lock, bool onAccept, bool onCancel);
bool CallPlayerAnykey(void* lock, const wchar_t* keyName, bool pressed);

}  // namespace ue_wrap::passwordlock

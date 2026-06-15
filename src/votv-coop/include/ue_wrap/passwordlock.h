// ue_wrap/passwordlock.h -- standalone engine access for VOTV password keypads
// (ApasswordLock_C). Principle-7 engine-wrapper layer: it wraps the reflection /
// struct-offset / UFunction-thunk details of a keypad actor. NO network logic, NO
// gameplay/coop state -- coop::keypad_sync owns those and drives the keypad mirror
// through here.
//
// A keypad is an AtriggerBase_C descendant that gates a door. Cross-peer identity is
// the inherited AtriggerBase_C::Key FName (deterministic placed-trigger key, save-
// persistent; v31 proved it stable, keysHash 0xF75D.../14).
//
// HOW THE MIRROR WORKS (RE-and-probe-proven 2026-06-04; refined by the BP disassembly
// 2026-06-06 -- research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md):
//   * Every keypad verb (Open / inputNumber / focusOn / SetActive) dispatches BP-internally
//     (CallFunction -> ProcessInternal, bypassing our ProcessEvent detour), so a POST observer
//     NEVER fires on them -> the sync POLLS state, it cannot observe verbs.
//   * inputNumber(digit) IS callable and APPENDS to inPassword natively (display + beep come
//     for free), and it drives the keypad's OWN native validator (Len>=5 && inPassword==
//     password -> open). So the receiver mirrors TYPING by replaying inputNumber -- pure MTA
//     input-replication: on the HOST, replaying the client's digits makes the host's native
//     keypad accept the code itself (no guessed output mirror, no unreachable submit verb).
//   * We DO NOT mirror isAcc/isDeny: the disassembly proved they are crosshair-HOVER flags
//     (lookAt: isAcc = crosshair-over-key_acc), NOT accept/deny state -- writing both onto a
//     mirror was the non-native green+red "PURPLE" the user reported. The door lock keys on the
//     DOOR's own `Active` (ue_wrap::door::CanOpen), never the keypad's isAcc. upd() repaints the
//     digit DISPLAY after a buffer change (so a native clear wipes the panel).
//   * We DO mirror the keypad's own `active` (bool @0x0330): the BP disassembly
//     (votv-keypad-door-BP-disassembly-2026-06-06.md, cancel trace) proved this is the LED
//     SELECTOR -- `upd` picks eff_glow_red when `!isReset && !active`, green when active. The
//     cancel button calls `open(false)` which sets `active=false` (CFG 848: self.active =
//     SwitchValue(self.false){case0:arg}) -> red. `active` also == the door's power/lock (setActive
//     propagates `door.active = self.active`). It is a real persistent state, NOT a hover flag, so
//     mirroring it (direct field-write + upd repaint, never the `setActive` verb -> never fires
//     powerChanged, which is the LIGHT-actor "purple", a different bug) reproduces the cancel-red on
//     every peer + keeps coop=SP (keypad.active==door.active). Wrong-CODE red already mirrors via the
//     inPassword digit replay (the host's native open(false) fires); this closes the EXPLICIT cancel
//     button gap (a cancel types no digit, so the buffer mirror alone misses it).
//
// RE: research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::passwordlock {

// Resolve the passwordLock_C UClass + the field offsets (Key / inPassword) + the
// inputNumber / Reset / upd UFunctions. Idempotent; true once everything resolved (false
// while the BP class is not yet loaded -- the caller retries on a later tick). Game thread.
bool EnsureResolved();

// True iff `obj`'s class is passwordLock_C or a subclass. Cheap (bounded super walk;
// no allocation). False if not yet resolved.
bool IsPasswordLock(void* obj);

// The keypad's AtriggerBase_C::Key FName as a wide string. Empty on failure
// (null / not resolved); L"None" if unkeyed.
std::wstring GetKeyString(void* lock);

// The mirror-relevant state of one keypad: the typed buffer (inPassword) + the `active` LED/
// power flag. The sender polls this each tick and broadcasts on change. (isAcc/isDeny were
// dropped 2026-06-06 -- crosshair-hover flags, not state; `active` is the real persistent
// LED-selector + door power, see the header note.)
struct State {
    std::wstring buffer;          // inPassword -- the digits typed so far (display)
    bool         active = false;  // ApasswordLock_C::active @0x0330 -- LED selector (red when false) + door power
};

// Read `lock`'s mirror state into `out`. False if the read could not be made
// (null / not resolved); leaves `out` untouched on failure. Game thread.
bool ReadState(void* lock, State& out);

// --- Accept-chain context primitives (game thread) --------------------------------
// The accept truth (BP disassembly 2026-06-06, corrected 2026-06-11): typing digits grows
// `inPassword`; at Len>=5 the BP AUTO-submits open(password==inPassword) (uber @2398) -- so
// long codes validate natively on every peer from the digit replay alone. SHORT codes submit
// only on the accept press (open(eq)) / cancel (open(false)) -- mirrored cross-peer as v59
// KeypadEvent + CallOpen below. (The old host-side buffer==password re-derivation accepted
// WITHOUT the press and is deleted -- the 2026-06-11 stuck-green bug.)

// The door this keypad gates (Adoor_C* @0x0338), or nullptr (none / dead / not resolved).
// Game thread.
void* GatedDoor(void* lock);

// True iff the keypad is in "set a new code" mode (isReset @0x0360): a correct entry WRITES the
// password instead of opening the door, so the accept-open must be skipped for it. Game thread.
bool IsResetMode(void* lock);

// --- Receiver apply primitives (game thread) -------------------------------------
// Replay one typed digit (0..9): dispatches inputNumber(digit), which APPENDS to
// inPassword the native way (display + beep). This is how the receiver mirrors typing
// -- NOT a submit. False on null / unresolved UFunction / out-of-range digit.
bool CallInputNumber(void* lock, int32_t digit);

// Clear the typed buffer (inPassword -> empty) with NO side effects: a direct FString
// length-zero (Num=0, the canonical empty TArray<TCHAR>; Data/Max retained as slack and
// freed by the engine on the next append/reassign, so no leak). This is the SAME direct-
// field-write philosophy as WriteActive -- deliberately NOT the BP `Reset()` verb, which
// is the keypad's "set a new code" mode (it sets isReset=true -> BLUE LED). Mirroring a
// client's CANCEL (which shrinks the buffer) through Reset() turned the HOST blue -- the
// 2026-06-08 bug (BP disassembly: reset()->ubergraph 3091->isReset=TRUE). The caller
// repaints the now-empty panel via CallUpd. False on null / unresolved. Game thread.
bool ClearBuffer(void* lock);

// Best-effort visual refresh after the buffer change: dispatches the keypad's own upd()
// verb (if present) so a tick/event-driven LED/material can repaint from the freshly-
// written state. Low-risk (an "update" verb, not a gameplay/submit verb); a no-op if
// upd() is absent. Whether it actually repaints the LED is a hands-on check.
void CallUpd(void* lock);

// Dispatch the keypad's NATIVE submit handler `Open(bool Active)` -- exactly what the
// typist's accept/cancel press runs internally (uber @4507/@4734/@2479/@3705). Sets
// active:=Active (LED green/red), plays the success/deny sound, clears inPassword, and
// setActive-propagates the LOCK state to the PAIR keypad + the gated door (door.active).
// It UNLOCKS/locks -- it never MOVES the door (the 4s-doorOpen chain is a scripted
// trigger entry, not the player accept; 2026-06-06 doc, re-confirmed by the 2026-06-11
// audit). The v59 receiver-side replication of a SHORT-code submit (len>=5 codes never
// need it -- the digit replay runs the BP's own auto-submit on every peer). May run
// latent sub-chains; never assume synchronous state. False on null / unresolved.
// Game thread.
bool CallOpen(void* lock, bool accept);

// Direct field-write of the keypad's `active` bool @0x0330 (the LED selector: false -> red).
// DELIBERATELY a raw field-write, NOT the `setActive` verb: setActive cascades powerChanged
// (which drives a separate LIGHT actor -> the old "purple" bug) + re-propagates to pair/door.
// The receiver writes this then calls CallUpd to repaint, and separately drives the gated
// door's power via ue_wrap::door::SetActive (keeping keypad.active == door.active like SP),
// so no cascade is needed. False on null / unresolved. Game thread.
bool WriteActive(void* lock, bool active);

}  // namespace ue_wrap::passwordlock

// coop/dev/keypad_probe.h -- dev-only RE probe for the password keypad
// (ApasswordLock_C) digit-entry state machine, gated on ini `keypad_probe=1`.
//
// A live digit-by-digit display with per-key sounds hinges on the
// semantics of inputNumber(int32): does it append the digit to inPassword, increment
// Num, play the beep, and match-check (firing Open/falseEnterEvent)? Those verbs are
// BP-internal so a POST observer never sees a real press -- this probe instead (a)
// registers observers to CONFIRM they're BP-internal, (b) runs a one-shot SYNTHETIC
// inputNumber sequence (typing the keypad's own `password`) and logs how inPassword /
// Num / isAcc / isDeny transition, proving the receiver can replay inputNumber to
// mirror typing, and (c) passively logs state changes so a hands-on real-code entry
// shows the machine. No ship behaviour (ini-gated, self-latched).

#pragma once

namespace coop::dev::keypad_probe {

// Idempotent; resolves passwordLock_C + offsets + registers the BP-internal-confirm
// observers. Retried every net-pump tick until the BP class loads. Game thread.
void Install();

// Per-tick: the one-shot synthetic inputNumber sequence (after the world loads) +
// the passive state-change poll. Game thread.
void Tick();

}  // namespace coop::dev::keypad_probe

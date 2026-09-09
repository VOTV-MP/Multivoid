// coop/creatures/kerfur_menu_input.h -- CLIENT-side detection of a kerfur radial-menu verb
// selection, relayed host-authoritatively through the existing kerfur_command path.
//
// WHY a new seam: the radial dispatch (AmainPlayer_C::useSelectedAction ->
// kerfurOmega_C::actionName) is EX_LocalVirtualFunction, INVISIBLE to our single ProcessEvent
// detour, so it cannot be hooked -- the old actionName interceptor never fired. A State-delta poll
// on the mirror loses the race too: the host streams kerfState every pose tick at 60 Hz and the
// client re-applies it every frame before a 5 Hz poll could read the local change, which measured
// about 0% detection. The ONE ProcessEvent-dispatched seam is AmainPlayer_C::InpActEvt_use, the E
// input action interactable_sync already hooks for doors. A PRE observer on it, gated on the radial
// confirm (releaseEToUse) while the local player is aiming at a kerfur (lookAtActor), reads the
// selected verb BEFORE the local dispatch runs and relays it -- race-free. CLIENT-only: the host's
// own menu use already mirrors through the pose stream and the native follow.
//
// Principle 7: gameplay/network module; engine access through ue_wrap only.

#pragma once

namespace coop::net { class Session; }

namespace coop::kerfur_menu_input {

// Idempotent: caches the session (EVERY call -- reconnect re-caches) and registers the
// InpActEvt_use PRE observer once mainPlayer_C is loaded. Call from subsystems::Install
// (world-gated: the observer needs mainPlayer_C, and the radial is only used in-world).
void Install(coop::net::Session* session);

// Net disconnect: drop the cached session so a stray late observer fire no-ops. The observer
// registration itself stays for process life (the cb is idempotently re-armed on reconnect).
void OnDisconnect();

}  // namespace coop::kerfur_menu_input

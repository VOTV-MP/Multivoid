// coop/props/trash_use_intercept.h -- the CLIENT-grab bridge on the "use" (E) input.
//
// Owns the
// InpActEvt_use PRE-interceptor family (the _41 grab intercept, the _38/_42 use_deny
// suppressors, and the _58/_59 LMB hard-throw bridge). On a CLIENT it cancels the native
// pile grab / carry-throw press and routes GrabIntent/ThrowIntent to the host; on the HOST
// it always runs native. The re-pile / drop observers + the held-item broadcast stay in
// trash_collect_sync. docs/piles/08 is the mechanism authority.
//
// Wiring: trash_collect_sync::Install/OnDisconnect delegate to these (the top-level harness
// Install call reaches both). Game thread only.

#pragma once

namespace coop::net { class Session; }

namespace coop::trash_use_intercept {

// Register the use/fire input interceptors on mainPlayer_C and cache the session. Idempotent
// (latched after the first successful register); retried by the caller until mainPlayer_C is
// loaded. Call from trash_collect_sync::Install. Game thread only.
void Install(coop::net::Session* session);

// Clear the cached session + reset the gesture-pairing latch on session teardown. Call from
// trash_collect_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::trash_use_intercept

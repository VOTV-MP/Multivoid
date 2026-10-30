// coop/props/prop_can_state.h -- a canned food's lid and contents reach every copy. Overview: docs/props.md.
//
// prop_cannedFood_C holds `opened` and `foods` in its save record, and the game changes them in place: the can
// opener and enough impact damage run Open, eating runs eat, and both end in Init. So a can opened or eaten from
// on one machine stayed full and shut on the others until the next join. Init also runs for reasons that change
// nothing (a flag verb), so it is not the signal by itself: every class's init is watched at the script gate, a
// can's state is read after it, and only a change from what this peer last knew publishes the record. A can never
// seen before is compared with its class's defaults, so a new one stays quiet and the first Open on an old one
// goes out. A record landing here updates what is known and does not go back. Temperature and ripeness belong to
// prop_food_C, a separate family (coop/props/prop_food_state). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::prop_can_state {

// Cache the session and register the watch, once for the process. Idempotent.
void Install(coop::net::Session* session);

// Per gameplay tick: resolve the watch's names until it is live, and say once whether it is.
void Tick();

// Session end: what is known of every can, and the counters.
void OnDisconnect();

}  // namespace coop::props::prop_can_state

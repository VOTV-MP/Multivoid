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

#include <string>

namespace coop::net { class Session; }

namespace coop::props::prop_can_state {

// Cache the session and register the watch, once for the process. Idempotent.
void Install(coop::net::Session* session);

// Per gameplay tick: resolve the watch's names until it is live, and say once whether it is.
void Tick();

// How many records this peer has published for a can's change, how many landed here, and how many changes of a can
// whose author's record is owed were not sent, since the session began. A rebirth is an init of a new actor under a Key
// this peer already knew (counted whether or not the adoption is on), and `rebirthsCarried` those whose state already
// equalled what was known: the two say whether a rebirth is ever born with anything but its state.
// For the two-peer drill, which asks that one change costs one record. Game thread.
struct Counts {
    unsigned published = 0;
    unsigned landed = 0;
    unsigned withheld = 0;
    unsigned rebirths = 0;
    unsigned rebirthsCarried = 0;
};
Counts CountsNow();

// How many records this peer has published for the can under `key`; 0 for a Key it does not know. For the drill.
unsigned PublishedFor(const std::wstring& key);

// Session end: the counters. What is known of every can leaves with the world, not with the session: a host whose
// last client left is still in the same world.
void OnDisconnect();

}  // namespace coop::props::prop_can_state

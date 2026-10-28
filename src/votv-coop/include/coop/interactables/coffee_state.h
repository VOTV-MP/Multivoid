// coop/interactables/coffee_state.h -- the coffee maker's state is the host's.
//
// The maker's state is its save record: hasCup, hasCoffee and coffeeReady (its loadData restores them and
// repaints). Two bodies change them: its brew (actionOptionIndex, action 4) and its hit handler, which is the
// fill (a cup that touches it is destroyed and sets hasCup; a coffee bag that touches it loses one of its amounts
// and sets hasCoffee). The host runs both: a client's press goes to the host (coop/interactables/coffee_intent),
// and a client's own run of the hit handler is cancelled here, since it would destroy its mirror of the cup and
// set flags no record carries. After each run the host compares the flags with what it noted before the body and
// publishes the maker's record (coop/props/prop_save_data) when they differ, and the bag's when its amounts moved;
// every client applies it through loadData. A client's record for a maker or a bag is refused, except the one
// behind its own drop (coop/props/prop_record_gates). The cup's destroy crosses by the native destroy seam, the
// debit by balance_sync. A maker's power follows the cord lane on each peer and is not in the record.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::coffee_state {

// Registers the two bodies' name watches and, once, the record gates. The per-tick retry pump
// (coop/interactables/verb_lanes), so a watch the gate refused is registered again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when both are live. Game thread, once per pump tick.
void Tick();

// The session ended: the counters, with one summary line.
void OnDisconnect();

// What the coffee drill reads of a client: how many of its own runs of the hit handler were cancelled this
// session, and the key of the other actor of the newest one ("" when it is no keyed prop or no longer here); and,
// for the hostile drill, the maker records the host published and the client records its gate refused.
// With the drill on (coffee_drill), every run of the hit handler on either role is also tallied: `hits` in all and
// `byClass` by the class of the actor that touched the maker. Both stay empty with it off.
struct ClassCount {
    std::wstring cls;
    uint64_t     n = 0;
};
struct DrillView {
    uint64_t     cancelled = 0;
    std::wstring lastOtherKey;
    uint64_t     hits = 0;
    uint64_t     madeMaker = 0, recordsRefused = 0;
    std::vector<ClassCount> byClass;
};
DrillView ReadForDrill();

}  // namespace coop::coffee_state

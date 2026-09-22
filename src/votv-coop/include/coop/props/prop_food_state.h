// coop/props/prop_food_state.h -- the food family's live state, and who is allowed to author it.
//
// A food does not hold still: its own tick lerps `temperature` toward a value it re-rolls every
// frame, rolls `ripeness` down against a random spoilage rate, and at the ends of that range
// decides on its own to burn or freeze the actor. Every peer running that tick is a separate
// author of the same food.
//
// The live-prop save record cannot be that state's carrier, and this is where the family says so.
// A record is captured at one birth and applied at another, while these three fields move
// continuously; worse, the record is stamped with the CAPTURING peer's world clock and the
// receiver's loadData reads the difference as elapsed time, which it is not. The lane that gives
// the state one author is not built yet.
//
// The claim binds the live-prop record lane only. A food inside a container or a pocket is a
// stored record on its own lane and still crosses whole, stamp included -- docs/scope.md.

#pragma once

namespace coop::props::prop_food_state {

// Claim Aprop_food_C and every class below it from the live-prop save-record lane. Runs from the
// session install, which is a retry pump re-entered every net_pump tick, so it must stay cheap
// after the first call -- the claim set is a set, and re-declaring a name it already holds does
// nothing.
void Install();

}  // namespace coop::props::prop_food_state

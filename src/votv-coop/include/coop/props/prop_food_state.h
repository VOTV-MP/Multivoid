// coop/props/prop_food_state.h -- the food family's live state, and who is allowed to author it.
//
// A food does not hold still. Its own tick lerps `temperature` toward a value it re-rolls every
// frame, rolls `ripeness` down against a random spoilage rate, and at the ends of that range
// decides on its own to burn the actor or freeze it -- so every peer running that tick is a
// separate author of the same food, with its own random numbers.
//
// The prop save record cannot be that state's carrier, and this is where the family says so. A
// record is captured at a birth and applied at another, while these three fields move continuously;
// worse, Aprop_food_C::getData stamps the record with the CAPTURING peer's
// UGameplayStatics::GetTimeSeconds, and its loadData catches the food up against that stamp using
// the RECEIVER's clock. Two peers' worlds start at different moments, so the difference those two
// readings produce is not an elapsed time at all: measured at a join, the receiver's clock was 20 s
// behind the stamp, every arriving food was credited 4 seconds of freshness back, and a chilled one
// came out of the catch-up 29 K warmer than it was sent. The same subtraction fires in single
// player, because TimeSeconds restarts with the world.
//
// So the class leaves the record lane by ownership rather than by accident, and the state it owns
// travels on a lane of its own -- host-authored, with the client-side producers silenced at the
// food's own verbs. That lane is not built yet; until it is, a food's temperature, ripeness and use
// count are each peer's own, exactly as they were before the record lane could carry them.

#pragma once

namespace coop::props::prop_food_state {

// Claim Aprop_food_C and every class below it from the save-record lane. Idempotent; called once
// with the other lane installs.
void Install();

}  // namespace coop::props::prop_food_state

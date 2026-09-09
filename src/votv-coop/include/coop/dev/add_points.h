// coop/dev/add_points.h -- dev cheat: add credits to the LOCAL balance.
//
// Calls AmainGamemode_C::AddPoints(int32) -- the single credit-writer
// (mainGamemode.hpp:447; the same path the drone-sell economy uses to
// pay out, so the UI/email/achievement side-effects fire identically). Intended
// for testing the delivery drone: the HOST needs a balance to afford laptop-shop
// orders. Local-only -- each peer's saveSlot.Points is its own (clients don't
// save), so press on the machine whose balance you want to top up; no wire
// broadcast. Render-thread safe (hops to the game thread for the UObject call).

#pragma once

namespace coop::dev::add_points {

// Add `amount` credits to the local player's balance (game-thread-posted). No-op +
// logged if the gamemode isn't live yet (booting / at the menu). Safe from the menu
// (render thread).
void GivePoints(int amount);

}  // namespace coop::dev::add_points

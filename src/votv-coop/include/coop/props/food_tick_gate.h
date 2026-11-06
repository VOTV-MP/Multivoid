// coop/props/food_tick_gate.h -- on a client, a world food's own tick does not run: the host's does.
//
// A food's tick rolls its temperature and spoilage and, at the ends of the range, spawns a burnt or frozen copy and
// destroys itself, so every peer running it is a separate author (coop/props/prop_food_sync sends the host's numbers).
// The refusal is at the script-body gate, by a watch on the class and the function name (the shape of
// coop/world/plant_gate), not by switching the actor's tick off: the food's `init` and the coffee cup's BeginPlay
// (Delay(0), then SetActorTickEnabled(true)) turn a switched-off tick back on.
//
// A Declared row covers the class and every subclass that inherits its ReceiveTick, not one that overrides it, so each
// prop_food_C subclass that declares its own ReceiveTick has its own row: prop_cookingFood_C (the egg inherits it),
// prop_fish_C, prop_dingus_C and prop_food_coffeeCup_C.
//
// Refused: on an active client session, every food except the one in the LOCAL player's hand, which is its holder's
// and keeps its tick; on any session, the copy shown in another slot's hand (hand_item's mirror), whose own tick could
// burn it. A single-player game and the host's world foods run. Game thread.

#pragma once

#include <cstdint>

namespace coop::food_tick_gate {

// Register the watches, once for the process, ahead of a joining client's world load: its foods tick from that world's
// first frame. A watch that did not register is said in one ERROR line. The refusal reads the session
// spawn_authority holds, so spawn_authority::Install runs first, at the same call site. Each watch is live once the
// game thread resolves its names (script_gate::ResolvePendingNames, driven by the lanes' ticks). Any thread.
void Install();

// Session end: say what the gate did this session and start the counts again from zero (the watches stay registered).
void OnDisconnect();

// What the gate did this session: the bodies it refused, and the bodies that RAN though the rule says refuse (a body
// the gate let through, so it counts a refusal that did not hold). Read-only, for drills. Game thread.
struct Counts {
    std::uint64_t refused = 0;
    std::uint64_t ran = 0;
};
Counts GetCounts();

}  // namespace coop::food_tick_gate

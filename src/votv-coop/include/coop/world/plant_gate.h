// coop/world/plant_gate.h -- a client's plants do not grow, rain or sprinkle by themselves: the host's do.
//
// A growingPlant_C (and every class under it, ue_wrap/actors/growing_plant.h) is a saved actor that its own tick grows on
// water, sun and random rain, and that dies, fruits and spawns fruit props by itself. Every peer loaded the same plants,
// so every peer ran that simulation on its own copy. The host runs it and plant_sync sends the result (`PlantState`); a
// client refuses the bodies that change a plant on their own, at the script-body gate, by a watch on the class and the
// function name (the shape of coop/creatures/kerfus_brain). The rows, each refused only on an active client session
// (spawn_authority::IsActiveClientSession), else run:
//   - growingPlant_C.ReceiveTick: growth, rain, drying, death and fruit;
//   - growingPlant_stolasHerb_C.grown: the herb's lightning end (the base's `grown` is empty);
//   - prop_sprinkler_C.dowater: a sprinkler's 0.1 s timer, which adds water to the plants in its radius;
//   - growthArea_C.ReceiveTick: the area that picks one plant at random and fertilises it.
// No subclass overrides `ReceiveTick`, so the base row matches every plant. What a plant's damage does is the damage
// gate's (coop/props/prop_damage_gate), and the winter bush's and the mushrooms' spawners are rows of spawn_authority.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::plant_gate {

// Register the watches, once for the process, ahead of a joining client's world load: its plants tick from that
// world's first frame. A watch that did not register is said in one ERROR line. The refusal reads the session spawn_authority
// holds, so spawn_authority::Install runs first, at the same call site. Each watch is live once the game thread resolves
// its names (script_gate::ResolvePendingNames, driven by the lanes' ticks). Any thread.
void Install();

// Session end: say what the gate did this session and start the counts again from zero (the watches stay registered).
void OnDisconnect();

// What the gate did this session: each row's refusals, and how many plant ticks RAN while a client session was active
// (a body the gate let through, so it counts a refusal that did not hold). Read-only, for drills. Game thread.
struct Counts {
    std::uint64_t plantTick = 0;
    std::uint64_t herbGrown = 0;
    std::uint64_t sprinklerWater = 0;
    std::uint64_t growthAreaTick = 0;
    std::uint64_t plantTickRan = 0;
};
Counts GetCounts();

}  // namespace coop::plant_gate

// coop/world/copy_graph_gate.h -- a client's copy of a host-run event actor runs a named few of its graph's entries.
//
// The boar (grayBoar_C, a prop with a brain), its overcharged kind, the ariral fire tank (firetank_C), the kavotia
// (kavotia_C) and the igetis (igetis_C) each think inside one event graph: timers, senses, moves, shots and mints.
// The host's is the one that thinks; a client's copy is moved by the host's pose. So on a client the graph of each
// runs only the entries reached straight from the events listed for it (its begin play, its tick where the tick is
// presentation, its destroy) and every other entry is refused: a timer the begin play armed fires into a refusal,
// and so does a function the begin play calls that re-enters the graph.
//
// An entry is told by the event that calls it, not by its cooked offset: the gate notes the object when a listed
// event's own body starts, and the graph call that body makes next on that object passes. A game update that moves
// the entries moves nothing here. The walker (coop/creatures/walker_brain) is the precedent, keyed by offset.

#pragma once

#include <cstdint>

namespace coop::copy_graph_gate {

// Registers the watches, once per process; refused only on an active client session (coop/world/spawn_authority).
// Called pre-world, with the other client refusals. Game thread.
void Install();

// `cls` is, or descends from, a class whose graph a client's copy does not run. Such an actor's health and death
// are the host's, so a client's hit on its copy is an intent (coop/creatures/hit_target_creature).
bool IsGatedClass(void* cls);

// Graph calls refused since the session began, over every class.
uint64_t RefusedCalls();

// Says what was refused over the session, per class, and starts the counts again.
void OnDisconnect();

}  // namespace coop::copy_graph_gate

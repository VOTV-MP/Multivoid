// coop/dev/function_lookup_parity.h -- [dev] reflection::FindFunction, which reads a class's own function
// list, checked against the object-array walk it replaced. Once per world, when the object index has
// drained the world's load: one walk of the array lists every object of exactly the Function class that
// a class owns, and each is looked up by that class and its name; a lookup that answers another object
// or none is named, and a verdict line closes the pass. The pass renders every function's name, so it
// costs a visible hitch; it is armed per run. Armed by function_lookup_parity=1. Game thread.
#pragma once

namespace coop::dev::function_lookup_parity {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::function_lookup_parity

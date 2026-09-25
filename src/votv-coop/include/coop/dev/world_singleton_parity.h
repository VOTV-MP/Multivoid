// coop/dev/world_singleton_parity.h -- [dev] the object index's loaded-class lookup and the world
// singleton, checked against the walks they replace. Once per world, the menu's too, when the index has
// drained the world's load (and a gameplay world's gamemode is up), each name below is looked up both
// ways, and the verdict says how long after the probe first saw the world: the class by
// object_index::ClassByName and by reflection::FindClass, and, for the one-of-a-kind classes, the
// instance by world_singleton::Find and by a walk that applies the singleton's own rule (live,
// readable, of the running world); the Gamemode() and GameInstance() accessors, which hold their own
// references, are compared the same way. Every row is logged, a mismatch named, a row where both are
// null said as such, and a verdict line closes the pass. Armed by world_singleton_parity=1. Game
// thread.
#pragma once

namespace coop::dev::world_singleton_parity {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::world_singleton_parity

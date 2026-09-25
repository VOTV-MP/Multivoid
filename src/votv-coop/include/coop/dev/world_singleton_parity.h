// coop/dev/world_singleton_parity.h -- [dev] the object index's loaded-class lookup and the world
// singleton, checked against the walks they replace. Once per gameplay world, when its gamemode is up,
// each name below is looked up both ways: the class by object_index::ClassByName and by
// reflection::FindClass, and, for the one-of-a-kind classes, the instance by world_singleton::Find
// and by FindObjectByClass. Every row is logged, a mismatch named, and a verdict line closes the
// pass. Armed by world_singleton_parity=1. Game thread.
#pragma once

namespace coop::dev::world_singleton_parity {

// The session tick's entry: one latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::world_singleton_parity

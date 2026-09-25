// ue_wrap/world/world_instances.h -- the running world's instances of a class, found through the object
// index rather than by walking the object array. world_singleton takes one of them for a class the game
// keeps one of; Find here hands out every one, for a class a level places several of (the garage's
// consoles). Both judge an instance by the one rule below. Game thread.
#pragma once

#include <cstdint>

namespace ue_wrap::world_instances {

// Whether an index member may be handed out: live and readable (not marked for death, not still being
// loaded or constructed), not its class's default object, and, when it has a world, of the running one.
// A departed world's actor stays unmarked until the purge, and one whose level has already lost its
// world stamps no world at all, so an instance of an actor class (`actorClass`) must stamp the running
// one. With no running world to judge against (`runningWorld` null: boot, mid-travel), the stamp only
// has to exist, the rule a held reference keeps.
bool IsTakeable(void* obj, int32_t index, bool actorClass, void* runningWorld);

// Whether `cls` is an actor's class, whose instances belong to a world.
bool IsActorClass(void* cls);

// Up to `cap` takeable instances of exactly the loaded class named `className` (not its subclasses), in
// no particular order; returns the count. Zero while the class is not loaded, and an instance born in a
// level load can be missing for a few batches after the world appears, as for world_singleton::Find.
int32_t Find(const wchar_t* className, void** out, int32_t cap);

}  // namespace ue_wrap::world_instances

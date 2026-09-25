// ue_wrap/world/world_singleton.h -- the world's one-of-a-kind objects (the gamemode, the game
// instance, the delivery drone and the like), found without walking the object array.
//
// A resolver used to cache a pointer and walk the whole array on a miss, on every call while the class
// was absent (a menu, another level). Here a miss asks the object index, which lists the loaded
// classes by name and each class's instances. An instance is handed out only when it is live and
// readable (not marked for death, not still being loaded or constructed), is not the class default
// object and, when it has a world, belongs to the running one: a departed world's actors stay
// unmarked until the purge. It is then held in a CachedObjRef, revalidated by slot, serial and world
// without touching the object, and looked up again only after that fails. Exact class, never a
// subclass. The index is drained before each batch of game-thread tasks, so a reader outside one (an
// observer inside a blocking load) sees it as the last batch left it, and an instance born in a level
// load can be missing for a few batches after the world appears, until the index has applied that
// load (object_index::Backlog at zero): the answer is null then, as for an absent class. The running
// world is world_identity's. Game thread.
#pragma once

namespace ue_wrap::world_singleton {

// The live instance of the class named `className`, or null when none is loaded or the object index
// is not seeded yet. Where several instances exist, one of them: callers ask this only of classes
// the game keeps one of.
void* Find(const wchar_t* className);

// The common ones, by the names the profile gives them.
void* Gamemode();       // mainGamemode_C
void* GameInstance();   // the GameInstance class the profile names

}  // namespace ue_wrap::world_singleton

// ue_wrap/world/world_singleton.h -- the world's one-of-a-kind objects (the gamemode, the game
// instance, the delivery drone and the like), found without walking the object array.
//
// A resolver used to cache a pointer and, on a miss, walk the whole array for the class by name;
// with the class absent (a menu, another level, a world without it) that walk ran on every call,
// per tick for some. Here a miss is one lookup in the object index: the class is found among the
// loaded classes by name, and its live instances are the index's own list of it. The found instance
// is held in a CachedObjRef, revalidated by slot, serial and world without touching the object, and
// looked up again only after it is gone. Exact class, never a subclass, and never the class default
// object, as reflection::FindObjectByClass. The running world is not one of these:
// engine/world_identity's CurrentWorld names it. Game thread.
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

// ue_wrap/actors/spawner_stamp.h -- what a class's native spawner writes into the actor between the two halves of
// its spawn (BeginDeferredActorSpawnFromClass, then FinishSpawningActor), because the class's BeginPlay reads it:
// the value is a constant of the spawner's own bytecode, so a spawn made any other way writes it too. One table
// row per class; today the black orb (npc_orborb_C), whose BeginPlay ends the game unless NewVar_0 holds the string
// blackballSpawner_C writes, and the birch (birch_C), the same shape and inert while no birch is mirrored. A row
// is known only while the loaded class still declares the function its guarded path runs in.
// Precedent: Source creates an entity, sets the key values its creator would, then spawns it
// (reference/source-sdk-2013/src/game/server/ai_concommands.cpp:404-415, the map loader the same,
// mapentities.cpp:574-581); MTA builds an entity from its add packet with the creation-time properties set
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:3871, the ped). A constant of the
// spawner's bytecode is written from this table and not carried (a per-spawn value would be carried, MTA's
// way). Engine-wrapper layer (principle 7): no network and no coop state; the caller decides what a failed stamp
// means.

#pragma once

namespace ue_wrap::spawner_stamp {

// Write every row of the table for `actorClass` into `deferredActor` (spawned, its Finish not yet made). True when
// the class has no row or every write landed. False when the loaded class no longer declares the row's guard
// function, or its property is missing, is not an FString, or the write failed: the game changed and the stamp is
// no longer known. The guard, the property's offset and its type are resolved once per listing of the class
// (object_index::ClassListingId), the cost of a call after that being the listing compare; a class the index does
// not list (id 0) is resolved on every call. The first stamp says `[NQ] stamped <class> <property>`, a failed one
// `[NQ] stamp failed <class> <what>` (`guard`, or the property's name), each once per listing, and once per class object while the index does
// not list it. Game thread.
bool Stamp(void* actorClass, void* deferredActor);

// How many Stamp calls returned false in this process (a drill's read). Game thread.
int FailureCount();

}  // namespace ue_wrap::spawner_stamp

// ue_wrap/actors/item_spawn.h -- a loose item the host makes by the name of its class, with the class loaded first.
// Principle-7 engine-wrapper layer: no network or coop state.
//
// The game loads a Blueprint class when something first needs it, so which item classes a process holds depends on
// what its save and its play touched. A refund, a hand-over or a pulled part names an item class from a fixed table of
// the caller's; a host that never held that item has not loaded the class, and a lookup in the object index alone
// answers null and the item is lost. Both verbs here ask the class resolver, which loads the class on the spot when this
// process has not loaded it. The name must come from the caller's own table and never from a wire: the resolver's family
// is open (class_resolve::AnyFamily), so the table is the bound.
//
// Game thread.

#pragma once

#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

namespace ue_wrap::item_spawn {

// The loaded UClass named `className` (a Blueprint class name with its `_C`), loading it when this process has not.
// Null when the class does not load. For a caller that stages the spawn itself (a deferred begin with writes before the
// finish) and needs only the class.
void* LoadedClass(const wchar_t* className);

// A new actor of the class named `className` at `at`, the class loaded first. `beforeFinish` and `ctx` are
// engine::SpawnActor's: the callback runs on the begun actor before its init. The actor, or null (the class did not
// load, or the world refused the spawn).
void* Spawn(const wchar_t* className, const FVector& at, engine::BeforeFinishFn beforeFinish = nullptr,
            void* ctx = nullptr);

}  // namespace ue_wrap::item_spawn

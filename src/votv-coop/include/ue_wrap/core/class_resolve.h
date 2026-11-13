// ue_wrap/core/class_resolve.h -- the one resolver for "the UClass named X", loading it when this process has not.
//
// The game loads Blueprint classes lazily, so which classes a peer holds depends on what its save and its play
// touched. A receive site that looked a wire class name up in the object index dropped the spawn of any class this
// peer had not loaded. ResolveClass answers from the object index; for a class it does not hold, it finds the
// class's Blueprint in the engine's asset registry (the game's index of every cooked asset, reached by reflection:
// AssetRegistryHelpers::GetAssetRegistry, then GetAssetsByPath over /Game) and loads `<package>.<asset>_C` through
// asset_load, which returns the generated class itself. The name -> asset map is ours: RE-UE4SS's LoadAsset
// (LuaMod.cpp, 3214-3259) verifies a caller-given object path in the same registry and loads it; it does not look
// a name up. The load goes through the game's RyRuntime plugin (asset_load); without it the resolver says so once
// and answers Absent for every class that needed a load.
//
// Game thread only. The queue a Pending resolve waits in is coop's (coop/element/class_load_wait).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::class_resolve {

// What may be loaded is bounded, as the models an MTA client builds are: the server's model id is checked against the
// client's own table before the entity is built (mtasa-blue CPacketHandler.cpp:3278-3282, IsValidModel), and Source
// precaches a bounded, declared table (qlimits.h MAX_MODELS; baseentity.cpp:5421). Here the bound is the family,
// judged BEFORE the load from the registry's own tags (a Blueprint's ParentClass, read when the map is built) and again
// on the class the load yields, and only a Blueprint under /Game/objects/ is ever loaded. MTA streams a model through
// a queue and the entity waits, never dropped (CClientModelRequestManager.cpp, Request and DoPulse, RequestBlocking
// only for an instant stream-in): loads here run in a per-tick time budget and a resolve past it answers Pending, so
// the caller waits and asks again. MTA ships a resource's files with checksums before an entity cites them; this mod
// needs no manifest, because two peers join only on byte-equal builds (the game version and the build number).
enum class ClassState {
    Found,    // `cls` is loaded and listed
    Pending,  // the load budget of this tick is spent: wait (class_load_wait) and ask again next tick
    Refused,  // the asset is ambiguous, or it or its class is not of the family
    Absent,   // no such class here: not loaded, and the registry does not hold it (or cannot load it)
};

struct Resolved {
    ClassState state = ClassState::Absent;
    void* cls = nullptr;  // set only for Found
};

// A host never loads for a CLIENT's class name: that site asks with LookupOnly and gets Found or Absent from the
// object index alone. Only a client loads, and only for a name the host sent.
enum class Load {
    Budgeted,    // load on demand inside the per-tick budget
    Immediate,   // load on demand outside the budget: a caller that must return its result in place (it says why)
    LookupOnly,  // the object index alone: a host resolving a class a CLIENT named
};

// What a caller accepts. `bases` are class names (with their `_C`) of which the class, or a Blueprint ancestor of it,
// must be one: the registry's ParentClass tags answer this before any load (when the tags are readable), and
// `acceptsClass`, when set, judges the class itself as the second line, on a class the resolver loads and on the
// first time it hands out a class already in the index.
struct Family {
    const wchar_t* label;
    const wchar_t* const* bases;       // null: no ancestry bound
    size_t nBases;
    bool (*acceptsClass)(void* cls);   // null accepts every class
};

// A family that accepts what the caller's own fixed table names (an id -> class table needs no further bound).
const Family& AnyFamily();

// The loaded UClass named `className` (a Blueprint class name carries its `_C`). The object index is the lookup:
// one hash probe, so a flood of unknown names walks nothing. A name the registry cannot place (a native class, a
// class a pak brought) gets one whole-array lookup per name per world before it is Absent. A class this call loads
// is listed in the index before it returns, so a second resolve in the same tick loads nothing.
Resolved ResolveClass(const wchar_t* className, const Family& family, Load load);

// Whether a loading ask (Budgeted or Immediate) for `className` was answered Refused or Absent in this world: the
// refusal event a drill decides a red on. Kept for the newest 256 names, dropped when the world changes.
bool WasDeclined(const wchar_t* className);

// Once per frame: the load budget starts anew. Game thread.
void Tick();

// Whether this tick's load budget is spent: a Budgeted resolve of a class that needs a load now answers Pending, so a
// waiter does not ask again before the next tick.
bool LoadBudgetSpent();

// How many on-demand loads ran, and how many of them while the probe below said the game's save replay was running.
uint64_t LoadsRun();
uint64_t LoadsDuringReplay();

// The caller's answer to "is the game replaying its save now": a load that runs while it is true is counted and said.
void SetReplayProbe(bool (*inReplay)());

// The class names (asset name + `_C`) of every Blueprint under /Game/objects/ whose asset name starts with
// `prefix`, loaded or not; empty when the registry is unreachable. For a drill that needs a class this process has
// not loaded.
std::vector<std::wstring> RegistryClassNames(const wchar_t* prefix);

// A drill's red arm: false turns the registry step off, so ResolveClass is the object index alone. Never set by
// shipping code.
void SetRegistryStep(bool on);

}  // namespace ue_wrap::class_resolve

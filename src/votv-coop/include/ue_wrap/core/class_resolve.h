// ue_wrap/core/class_resolve.h -- the one resolver for "the UClass named X", loading it when this process has not.
//
// The game loads Blueprint classes lazily, so which classes a peer holds depends on what its save and its play
// touched. A receive site that looked a wire class name up in the object index dropped the spawn of any class this
// peer had not loaded. ResolveClass answers from the object index; for a class it does not hold, it finds the
// class's Blueprint in the engine's asset registry (the game's index of every cooked asset, reached by reflection:
// AssetRegistryHelpers::GetAssetRegistry, then GetAssetsByPath over /Game) and loads `<package>.<asset>_C` through
// asset_load, which returns the generated class itself. The name -> asset map is ours: RE-UE4SS's LoadAsset
// (UE4SS/src/Mod/LuaMod.cpp:3214-3259) verifies a caller-given object path in the same registry and loads it, it
// does not look a name up. The load goes through the game's RyRuntime plugin (asset_load); without it the resolver
// says so once and answers Absent for every class that needed a load.
//
// What may be loaded is bounded, as the models an MTA client builds are: the server's model id is checked against the
// client's own table before the entity is built (reference/mtasa-blue/Client/mods/deathmatch/logic/
// CPacketHandler.cpp:3278-3282, CClientVehicleManager::IsValidModel), and Source precaches a bounded, declared table
// (src/common/qlimits.h MAX_MODELS, CBaseEntity::PrecacheModel, baseentity.cpp:5421). Here the bound is the family:
// only a Blueprint under /Game/objects/ is ever loaded, and the class it yields must be of the caller's family or it
// is Refused. MTA streams a model through a queue and the entity waits (CClientModelRequestManager.cpp, Request and
// DoPulse, with RequestBlocking only for an instant stream-in): loads here run in a per-tick time budget, and a
// resolve past it answers Pending, so the caller parks its work and retries it next tick through Park. MTA ships a
// resource's files with checksums before the entity cites them; this mod needs no such manifest, because two peers
// join only on byte-equal builds (the game version and the build number).
//
// A host never loads for a CLIENT's class name: that site asks with Load::LookupOnly and gets Found or Absent from
// the index alone. Only a client loads, and only for a name the host sent.
//
// Game thread only.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ue_wrap::class_resolve {

enum class ClassState {
    Found,    // `cls` is loaded and listed
    Pending,  // the load budget of this tick is spent: Park the work and ask again next tick
    Refused,  // the asset is not a Blueprint under /Game/objects/, is ambiguous, or its class is not of the family
    Absent,   // no such class here: not loaded, and the registry does not hold it (or cannot load it)
};

struct Resolved {
    ClassState state = ClassState::Absent;
    void* cls = nullptr;  // set only for Found
};

enum class Load {
    Budgeted,    // load on demand inside the per-tick budget
    Immediate,   // load on demand outside the budget: a caller that cannot park (it returns the result at once)
    LookupOnly,  // the object index alone: a host resolving a class a CLIENT named
};

// What a caller accepts of a class this resolver loads. A class already in the object index is Found without a
// test (the caller's own checks, where it has them, stand); a class the resolver loads is judged here, so the
// load of an unwanted one is refused and remembered.
struct Family {
    const wchar_t* label;
    bool (*acceptsName)(const wchar_t* className);  // before any load; null accepts every name
    bool (*acceptsClass)(void* cls);                 // on the class the load yielded; null accepts every class
};

// A family that accepts what the caller's own fixed table names (an id -> class table needs no further bound).
const Family& AnyFamily();

// The loaded UClass named `className` (a Blueprint class name carries its `_C`). A name without `_C` is a native
// class: the object index, then one whole-array lookup per name per world.
Resolved ResolveClass(const wchar_t* className, const Family& family, Load load);

// Parks `retry` to run on the game thread from a later tick's Tick(), for a resolve that answered Pending. `site`
// names the caller (a string literal): each site holds at most 64 parked retries, and one that overflows says so
// once and returns false, so the caller drops the work. Parked work is dropped when the world changes.
bool Park(const char* site, std::function<void()> retry);

// Once per frame: the load budget starts anew and the retries parked by earlier ticks run. Game thread.
void Tick();

// The class names (asset name + `_C`) of every Blueprint under /Game/objects/ whose asset name starts with
// `prefix`, loaded or not; empty when the registry is unreachable. For a drill that needs a class this process has
// not loaded.
std::vector<std::wstring> RegistryClassNames(const wchar_t* prefix);

// A drill's red arm: false turns the registry step off, so ResolveClass is the object index alone. Never set by
// shipping code.
void SetRegistryStep(bool on);

}  // namespace ue_wrap::class_resolve

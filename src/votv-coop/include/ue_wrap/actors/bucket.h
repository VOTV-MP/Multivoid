// ue_wrap/actors/bucket.h -- engine access for the bucket family: the water bucket (prop_bucket_C and the base's mop bucket
// under it), the concrete bucket, the cement bag, the trowel (prop_wallfixer_C) and the sponge. Principle-7 engine-wrapper
// layer: no network or coop state.
//
// A water bucket holds `height` of water (up to `maxHeight`), `soap` units, a brewing `alcoholP` that turns `alcohol` on at 1,
// `alcReady` while it brews and `empty`; its tick eases `height_interp` toward `height` and redraws the water. A concrete bucket
// holds `units` of concrete (20 when mixed), drying `dryTimer` seconds toward `dry`, with a mesh `stage` derived from `units`.
// A cement bag holds `units` up to `maxUnits`; a trowel carries `concrete` (0 or 5). Each member is found by reflection on the
// instance's class and kept per class, so a layout shift follows the name; a member that does not resolve is said once per
// class and the accessor answers false. Every accessor is game thread only.
//
// A class is known by its name: a Blueprint class dies with its world, and a subclass with no body of its own counts as its
// parent (prop_bucket_mop_C is a prop_bucket_C).

#pragma once

#include "ue_wrap/core/reflection.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::save_record { struct SaveRecord; }

namespace ue_wrap::bucket {

enum class Kind : uint8_t { None, Plain, Concrete, Bag, Trowel, Sponge };

// What `actor` (or a class) is to this family, by its class's parent chain; kept per class. None for null and for any other.
Kind KindOf(void* actor);
Kind KindOfClass(void* cls);

// A dropped item's save record as these classes' loadData reads it: a water bucket's ints group 0 [soap], floats group 1 [height]
// and bools group 1 [alcohol]; a concrete bucket's ints 0 [units] and bools 1 [dry]; a bag's ints 0 [units]; a trowel's ints 0
// [concrete]. False when the groups for `kind` are missing or too short, with `out` untouched. Pure; any thread.
struct Record {
    float height = 0.f;
    int32_t soap = 0;
    int32_t units = 0;
    int32_t concrete = 0;
    bool alcohol = false;
    bool dry = false;
};
bool ReadRecord(Kind kind, const ue_wrap::save_record::SaveRecord& rec, Record& out);

// A water bucket's members.
struct Plain {
    float height = 0.f;
    float heightInterp = 0.f;
    float maxHeight = 0.f;
    float alcoholP = 0.f;
    int32_t soap = 0;
    bool alcohol = false;
    bool alcReady = false;
    bool empty = false;
    bool canConcrete = false;
};
bool ReadPlain(void* bucket, Plain& out);

// Puts the host's row on a water bucket: height, soap, alcoholP, alcohol and alcReady as given, and `empty` as the BP's own rule
// derives it (empty exactly when height <= 0). No call and none of the game's side effects. False for a bucket that does not
// resolve, with nothing written.
bool WritePlain(void* bucket, float height, int32_t soap, float alcoholP, bool alcohol, bool alcReady);

// A water bucket's brewing contents, read only: the actors its overlap box holds (`itemsInside`, up to `max` of them copied to
// `out`; `total` is the array's length). The bread and sugar are locals of the game's `deleteAlc`, not members, so they are not
// read here. False for a bucket that does not resolve.
bool ReadItemsInside(void* bucket, void** out, size_t max, size_t& total);

// A concrete bucket's members. `stage` is derived by the game's updStage from `units`.
struct Concrete {
    int32_t units = 0;
    int32_t stage = 0;
    float dryTimer = 0.f;
    bool dry = false;
};
bool ReadConcrete(void* bucket, Concrete& out);
bool WriteConcrete(void* bucket, int32_t units, float dryTimer, bool dry);

// A cement bag's units and cap.
bool ReadBag(void* bag, int32_t& units, int32_t& maxUnits);
bool WriteBagUnits(void* bag, int32_t units);

// A trowel's carried concrete.
bool ReadTrowel(void* trowel, int32_t& concrete);
bool WriteTrowel(void* trowel, int32_t concrete);

// The game's own repaints and verbs on these actors. False when the function does not resolve or the call does not run.
bool CallUpd(void* bucket);            // a water bucket: the water mesh and bubbles from the members
bool CallDipEffect(void* bucket);      // a water bucket: the splash a dip makes
bool CallUpdStage(void* bucket);       // a concrete bucket: stage from units, the mesh, then updDry
bool CallUpdDry(void* bucket);         // a concrete bucket: the material of `dry`
bool CallUpdConcrete(void* trowel);    // the trowel's carried-concrete mesh
bool CallDeleteAlc(void* bucket);      // a water bucket: destroys the bread and sugar inside
// takeConcrete(dry&): takes one unit, and replaces the bucket with a water bucket at its last. `dryOut` is the out-param: true
// when the concrete was dry and nothing was taken.
bool CallTakeConcrete(void* bucket, bool& dryOut);
// dipped(sponge, isDipped&): the bucket's side of a sponge's dip. `isDipped` is the out-param the sponge reads.
bool CallDipped(void* bucket, void* sponge, bool& isDipped);
// The held item's own `playerHandUse_LMB(player)`: a sponge's stroke, a cement bag's or trowel's use.
bool CallHandUseLmb(void* item, void* player);

// lib.replaceProp(prop, newName, delete=false, playerHolding=null, worldContext): the prop spawned in place of `prop`, named by a
// prop row (the concrete bucket is "concreteBucket"); null when the call does not run. `prop` is not destroyed.
void* ReplaceProp(void* prop, const wchar_t* newName, void* worldContext);

}  // namespace ue_wrap::bucket

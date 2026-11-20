// ue_wrap/actors/dingus.h -- engine access for the dingus (prop_dingus_C and the three classes below it that share its
// graph). Principle-7 engine-wrapper layer: no network or coop state.
//
// A dingus plays its music on `ws1`, an audio component active from its spawn, until it is found. `touch` (a grab and every
// action call it) sets `isUsed` and deactivates `ws1`; `loadData` with `isUsed` set lowers `ws1`'s volume to 0.05, gives it the
// small attenuation asset and deactivates it. The three classes below prop_dingus_C override neither function.
// prop_dingus3_C descends from another line (a food) and is not one of the four.
//
// A Blueprint class dies with the world that loaded it, so the four class pointers are asked again when the class set or the
// unlisted classes move, and while one is null. With the pointers the table holds what each class's reads and writes need
// (`isUsed`'s byte and bit, `ws1`'s offset) and, once all four are known, the attenuation asset: a mark loads no object.

#pragma once

#include <cstddef>

namespace ue_wrap::dingus {

// Ask the object index for the four classes again when its class set or its unlisted classes moved since the last ask, or when
// any of the four is still null (a class answers null while it loads, and its finishing moves no number). One ClassByName
// per class on such a tick, nothing otherwise. While all four are known it also loads the attenuation asset when none is held
// live and the class set moved since the last try. True on the call that found the four classes after a call that lacked one: a
// copy of a class that loaded in the same tick may have been born and bound while IsDingus was false. Game thread.
bool RefreshClasses();

// True iff `cls` is one of the four classes. A pointer compare, safe on any thread; false for null.
bool IsDingusClass(void* cls);

// True iff `actor` is an instance of one of the four classes: its class pointer, then IsDingusClass. No reflection call, so it is
// as cheap as a compare for every prop a join binds. False for null, and before RefreshClasses found the classes. Any thread
// that may read `actor`'s class.
bool IsDingus(void* actor);

// `isUsed` as 1 or 0, -1 when `actor` is no dingus or the member does not resolve. Game thread.
int ReadFound(void* actor);

// Whether `ws1`, the music component, is active, as 1 or 0; -1 when `actor` is no dingus or the component cannot be read. Game
// thread.
int ReadMusicOn(void* actor);

// What `loadData` writes for a found dingus, and only that: `isUsed` set, then on `ws1` the volume multiplier 0.05, the
// attenuation asset /Game/audio/misc/att_small, and Deactivate. It is not a record apply (a prop's loadData ends in `init`,
// which turns physics back on) and never calls `touch`, so no achievement is granted. False when `actor` is no dingus or any
// step fails (the asset not held included); the steps before the failing one stay written. Loads no object. Game thread.
bool MarkFound(void* actor);

// Every live dingus, in no particular order, class defaults excluded. Game thread; the callback must not create or destroy
// objects.
using DingusFn = void (*)(void* ctx, void* actor);
size_t ForEachDingus(DingusFn fn, void* ctx);

}  // namespace ue_wrap::dingus

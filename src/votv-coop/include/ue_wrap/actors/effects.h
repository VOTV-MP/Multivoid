// ue_wrap/actors/effects.h -- the local player's status effects (engine substrate): the game's own
// list of them, add, remove and list through the gamemode's verbs and arrays, and the reset of a
// save object's effect records.
//
// An effect is an `effect_C` actor the gamemode spawns from a row of its `list_effects` DataTable
// and tracks in two parallel arrays, `effects_names` and `effects`. The names this module accepts
// are that table's row names, read live and never copied. Game thread only (UFunction calls and
// object reads). No network, coop or policy logic (principle 7).
//
// The game's own effect lifecycle shows through: a non-stacking effect already active is merged by
// an add and spawns nothing, and lsd and foodPoison end by destroying themselves without leaving
// the arrays, so List reports such an entry as not live.
//
// A joiner's save object arrives holding the host's effects as records among its world's, and the
// game's own reset verb takes them out of that object before the world is built from it.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ue_wrap::effects {

// The game's effect names: the row names of its list_effects DataTable. Read once and kept (the
// table does not change within a run); false while the table is not loaded or its rows do not
// read, each asked again at the next call.
bool Names(std::vector<std::wstring>* out);

// live false: the actor is gone (strength and time are then 0).
struct Entry { std::wstring name; bool live; float strength; float time; };

// The gamemode's effects, effects_names[i] beside effects[i]. False when the gamemode or a
// property does not resolve, the two arrays differ in length, or effect_C's strength and time do
// not resolve while the arrays hold an entry; an empty pair, or a pair whose headers save_record
// rejects, lists as empty.
bool List(std::vector<Entry>* out);

// `fn(effect, ctx)` for each live actor of the gamemode's `effects` array, in array order; the number
// visited. 0, calling nothing, when the gamemode or the array does not resolve or the array's header does
// not read. `fn` may write the actor's fields and may not add or remove an effect. Game thread.
size_t ForEachWorldEffect(void (*fn)(void* effect, void* ctx), void* ctx);

// gamemode addEffect(effect, strength, time, incrementStrength=false, incrementTime=false). False,
// calling nothing, for a name not in Names() (matched without case; the table's spelling is what
// the game receives), for a non-finite strength or seconds, or when the gamemode or the function
// does not resolve, a parameter does not set, or the name does not convert to an FName. True means
// the game's verb ran, not that an entry was added: on a non-stacking effect already active the game
// merges (strength and time each become the larger of old and new), and a stale entry makes that
// merge a no-op.
bool Add(const std::wstring& name, float strength, float seconds);

// gamemode removeEffect(InputPin): the first entry of `name`. False, calling nothing, when List
// has no entry of that name or its first one is not live (the game would call removeRanout on a
// destroyed actor), or a parameter does not set or the name does not convert to an FName. One call
// takes one instance.
bool Remove(const std::wstring& name);

// The game's own saveSlot_C::reset_player_effects on `saveSlot`, which may be a save object whose world
// does not exist yet: every effect record of its objectsData removed (and any empty record). Returns how
// many records went (the array's length before less after); -1 when `saveSlot` is not live, the function
// or objectsData does not resolve, an array header does not read, or the call fails. `gamemodeSet` (null
// allowed) reports whether the save's gamemode field held an object when the call ran, the target of
// the hint the game's verb ends with.
int ResetOnSaveObject(void* saveSlot, bool* gamemodeSet);

}  // namespace ue_wrap::effects

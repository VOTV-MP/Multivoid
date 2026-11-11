// ue_wrap/actors/vitals.h -- the local player's stat table (engine substrate), and the profile
// snapshot of a player's own state.
//
// Twenty rows, each a stat of the local player on the object that owns it: the pawn (the gamemode's
// own `mainPlayer`, the game's reference to the local player), the save object
// (UmainGameInstance_C::save_gameInst, a UsaveSlot_C and exactly ONE per machine) and the gamemode.
// Every row names its property once, in this module's table, and is resolved by NAME per owner
// class: Blueprint-cooked offsets shift across recooks and are never hardcoded. Game thread only
// (UObject lookups and blueprint-state writes). No network, coop or quantization logic
// (principle 7): the callers in coop/ own the wire encoding.
//
// CAUTION: there is exactly ONE saveSlot per machine, and every remote-player puppet AND the
// local player resolve to it. This accessor only ever touches the LOCAL player's stats. NEVER
// use it to store a remote puppet's display health. Puppet display health lives on
// coop::RemotePlayer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::vitals {

// The rows of the stat table. The values are permanent (a wire id): a new
// row takes the next number. Rows 0-6 are the profile snapshot's seven floats, in its order.
enum class Field : uint8_t {
    Health = 0, MaxHealth = 1, Food = 2, Sleep = 3, CoffeePower = 4, GasolinePilled = 5, Strength = 6,
    Irradiation = 7, Air = 8, BurningTime = 9, Pooped = 10, FoodDrain = 11, SleepDrain = 12, Burning = 13,
    Dead = 14, Sleeping = 15, Dreaming = 16, Exhausted = 17, Nearsighted = 18, Glasses = 19, Count = 20,
};

enum class Owner : uint8_t { SaveSlot, Pawn, Gamemode };
enum class Type : uint8_t { Float, Bool };
enum class WriteRule : uint8_t { Raw, ReadOnly };

// `name` is the row's stable identifier (what the probe prints and a command token maps to);
// `property` the reflected property on the owner's class.
struct Row { Field field; const char* name; Owner owner; const wchar_t* property; Type type; WriteRule write; };

// The row of `f`; f < Field::Count.
const Row& RowOf(Field f);

// A number off the wire as a Field: false for id >= Count.
bool FieldFromId(uint8_t id, Field* out);

// Read `f` into *out (a Bool reads 0 or 1). False, *out untouched, for f >= Count, or when the
// row's owner or its property does not resolve (still booting, no world, no pawn). Game thread
// only. O(1) only while the owner (the gamemode, the pawn or the save object) resolves: while it
// is absent every call repeats the owner lookup, so a caller on a repeating path gates on its own
// readiness first.
bool Read(Field f, float* out);

// Write `v` by the row's rule (a Bool row stores v != 0). False, having written nothing, for
// f >= Count, a ReadOnly row, a non-finite `v`, or when the owner or the property does not
// resolve. Raw means raw: the game applies its own bounds on its next tick. Game thread only.
bool Write(Field f, float v);

// ---- The whole per-player vital state, as one value ------------------------------------------
//
// Gameplay reads and writes the vitals IN PLACE on the pawn (the hunger drain is a
// VictoryFloatMinusEquals on mainPlayer.save_food), and the game saves them inside the player's
// own record (ue_wrap/actors/player_record.h). Strength and the food tolerance lists are still the
// save object's. A joiner's world is built from a capture of the HOST's save object, so unless
// they are replaced there the joiner starts at the host's numbers.
struct Snapshot {
    float health = 0, maxHealth = 0, food = 0, sleep = 0;   // the pawn's save_* fields
    float coffeePower = 0, gasolinepilled = 0;
    float strength = 0;                                     // the save object's
    std::vector<std::wstring> foodConsumed;   // parallel arrays: what was eaten, and the
    std::vector<float>        foodTolerance;  // tolerance built up against it
};

// The snapshot's member for `f`: rows 0-6; nullptr for a row the profile does not store. The one
// place a row meets a Snapshot member.
float* SnapshotField(Snapshot& s, Field f);
const float* SnapshotField(const Snapshot& s, Field f);

// Read the LOCAL player's state off the live pawn and save object. False if either is not
// resolvable yet.
bool ReadSnapshot(Snapshot& out);

// Are the objects ReadSnapshot reads standing: the gamemode's `mainPlayer` pawn and the live save object. They
// come up a moment after the pawn does, so a ReadSnapshot that fails while this is false is "not yet", and one
// that fails while it is true is a row that does not resolve.
bool SnapshotSourcesUp();

// What a player who has never played here starts from: the pawn class's and the save class's own
// defaults, read off their class default objects rather than restated. `save`, when given, is a
// save object whose player record names the pawn class, loaded from there when it is not loaded
// yet (ue_wrap/actors/player_record.h). False if either class is not loaded.
bool ReadDefaults(Snapshot& out, void* save = nullptr);

// Write `s` onto `saveSlot`, which may be a save object whose world does not exist yet: the pawn's
// rows into the player's record there, through the game's own property codec, and the save
// object's own fields raw. The two food arrays are rebuilt as engine-owned arrays of equal length,
// the previous buffers orphaned. A save object with no player record takes the save object's part
// alone. False if `saveSlot` is dead, a field is missing, or a record did not take the pawn's rows.
bool ApplySnapshot(void* saveSlot, const Snapshot& s);

// saveSlot.playerTransform: where the game believes the player is. On a client nothing writes it
// (saveObjects never runs there), so it stays the HOST's position from the join capture -- and it
// is where the game's anti-noclip check puts a player it finds behind geometry. Written before
// the world exists so that fallback is the player's own spot. Scale 1. False if it did not resolve.
bool WritePlayerTransform(void* saveSlot, float x, float y, float z, float yawDeg);

}  // namespace ue_wrap::vitals

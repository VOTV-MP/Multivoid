// ue_wrap/actors/vitals.h -- the local player's stat table (engine substrate), and the profile
// snapshot of the save object's per-player state.
//
// Twenty-two rows, each a stat of the local player on the object that owns it: the save object
// (UmainGameInstance_C::save_gameInst, a UsaveSlot_C and exactly ONE per machine), the pawn (the
// gamemode's own `mainPlayer`, the game's reference to the local player) and the gamemode. Every
// row names its property once, in this module's table, and is resolved by NAME per owner class:
// Blueprint-cooked offsets shift across recooks and are never hardcoded. Game thread only
// (UObject lookups and blueprint-state writes). No network, coop or quantization logic
// (principle 7): the callers in coop/ own the wire encoding.
//
// CAUTION: there is exactly ONE saveSlot per machine, and every remote-player puppet AND the
// local player resolve to it. This accessor only ever touches the LOCAL player's stats. NEVER
// use it to store a remote puppet's display health -- that would corrupt the local player's
// persisted health. Puppet display health lives on coop::RemotePlayer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::vitals {

// The rows of the stat table. The values are permanent (a wire id): a new
// row takes the next number. Rows 0-8 are the profile snapshot's nine floats, in its order.
enum class Field : uint8_t {
    Health = 0, MaxHealth = 1, Food = 2, Sleep = 3, Battery = 4, CoffeePower = 5, GasolinePilled = 6,
    Strength = 7, Agility = 8, Irradiation = 9, Air = 10, BurningTime = 11, Pooped = 12, FoodDrain = 13,
    SleepDrain = 14, Burning = 15, Dead = 16, Sleeping = 17, Dreaming = 18, Exhausted = 19, Nearsighted = 20,
    Glasses = 21, Count = 22,
};

enum class Owner : uint8_t { SaveSlot, Pawn, Gamemode };
enum class Type : uint8_t { Float, Bool };
enum class WriteRule : uint8_t { Raw, RawThenUpdateStrAgl, ReadOnly };

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

// The object the Pawn rows read and write: the gamemode's own `mainPlayer`, live. Null with no gamemode or
// no live pawn. The pointer is as live as the game keeps it; a caller holding it across ticks wraps it in a
// CachedObjRef. Game thread only.
void* LocalPawn();

// Write `v` by the row's rule (a Bool row stores v != 0). False, having written nothing, for
// f >= Count, a ReadOnly row, a non-finite `v`, or when the owner, the property or (Strength,
// Agility) the pawn and its updateStrAgl do not resolve -- every one resolved before the store.
// For Strength and Agility the store stands and the result is updateStrAgl's call's. Raw means
// raw: the game applies its own bounds on its next tick. Game thread only.
bool Write(Field f, float v);

// ---- The whole per-player vital state, as one value ------------------------------------------
//
// Gameplay reads and writes these IN PLACE on the save object (the hunger drain is a
// VictoryFloatMinusEquals on saveSlot.food), so the save object is their live store. The pawn
// keeps its own copy of strength and agility only (mainPlayer `str` and `agil`, each the save
// value / 100, which updateStrAgl refreshes from the save object). A joiner's world is built
// from a capture of the HOST's save object, so unless they are replaced there the joiner starts
// at the host's numbers.
struct Snapshot {
    float health = 0, maxHealth = 0, food = 0, sleep = 0;
    float battery = 0;                     // flashlight charge
    std::wstring flashlightBattery;        // the inserted battery's class, as its leaf name
    float coffeePower = 0, gasolinepilled = 0, strength = 0, agility = 0;
    std::vector<std::wstring> foodConsumed;   // parallel arrays: what was eaten, and the
    std::vector<float>        foodTolerance;  // tolerance built up against it
};

// The snapshot's member for `f`: rows 0-8; nullptr for a row the profile does not store. The one
// place a row meets a Snapshot member.
float* SnapshotField(Snapshot& s, Field f);
const float* SnapshotField(const Snapshot& s, Field f);

// Read the LOCAL player's state off the live save object. False if it is not resolvable yet.
bool ReadSnapshot(Snapshot& out);

// What a player who has never played here starts from: the save class's own defaults, read off
// its class default object rather than restated. False if the class is not loaded.
bool ReadDefaults(Snapshot& out);

// Write `s` onto `saveSlot`, which may be a save object whose world does not exist yet. The two
// food arrays are rebuilt as engine-owned arrays of equal length, the previous buffers orphaned
// (the contract of inventory::ApplyToSaveObject). An empty battery class is "no battery in the
// flashlight" and is written as one; a class that is not loaded leaves the field as it was.
// False, having written nothing, if `saveSlot` is dead or a field is missing.
bool ApplySnapshot(void* saveSlot, const Snapshot& s);

// saveSlot.playerTransform: where the game believes the player is. On a client nothing writes it
// (saveObjects never runs there), so it stays the HOST's position from the join capture -- and it
// is where the game's anti-noclip check puts a player it finds behind geometry. Written before
// the world exists so that fallback is the player's own spot. Scale 1. False if it did not resolve.
bool WritePlayerTransform(void* saveSlot, float x, float y, float z, float yawDeg);

}  // namespace ue_wrap::vitals

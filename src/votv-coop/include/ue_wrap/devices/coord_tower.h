// ue_wrap/devices/coord_tower.h -- the three coordinate towers (coordRadarDish_C): their state, its raw writes, and
// the tower's own painters and verbs.
//
// The towers triangulate the coordinates panel's cursor. Each can break: eight fuses and a lights-out puzzle,
// both saved with the actor, and a broken tower blocks the ping and the movement keys. A tower that loads broken
// scrambles itself again inside its own loadData, on every machine that loads the save. Its panel opens on a
// montage whose end flips `opened`; its lever runs a timeline whose end judges the puzzle. A player's use reads
// the look-at getActionOptions wrote from that player's trace. Instances come from the object index, never from
// a walk. Principle 7: no network or gameplay logic. Game thread.

#pragma once

#include "ue_wrap/core/types.h"  // FVector

#include <cstdint>

namespace ue_wrap::coord_tower {

inline constexpr int kMaxFuses = 8;    // the class default holds eight
inline constexpr int kMaxLights = 16;  // the difficulty sizes the puzzle at 3 to 10

struct State {
    int32_t id = -1;
    bool    isBroken = false;
    bool    opened = false;        // the fuse panel
    bool    isAnim = false;        // the panel's montage runs; its end flips `opened`
    bool    leverMoving = false;   // the lever's timeline runs; its end judges the puzzle
    bool    leverUp = false;       // the lever stands up or heads up: its timeline plays forward from past its start
    uint8_t fuseCount = 0;
    uint8_t fuses[kMaxFuses] = {};  // 0 empty, 1 good, 2 blown
    uint8_t lightCount = 0;
    bool    lights[kMaxLights] = {};  // puzzleLights; solved when every one is lit
};

// Resolve the class, its members and verbs. A missing class is looked for again at most every 2 s, and so is one
// that has gone with its map; a member missing from the loaded class latches the wrapper off with one warning
// naming it, since it will not appear later. True while resolved.
bool EnsureResolved();

// Every live tower, sorted by id, into `out`. How many were written, or -1 while the wrapper is not resolved, so
// a caller never prints an unresolved read as "no towers".
int32_t ReadAll(void** out, int32_t cap);

// The id of `tower`, or -1 when it is not a tower. An unresolved wrapper resolves from the tower's own class: a
// name compare and that class's property chain, never a walk of the object array, so it is safe inside a
// script-gate callback and names the tower inside the first world load.
int32_t IdOf(void* tower);

bool Read(void* tower, State& out);

// Raw writes: nothing repaints. WriteLights takes the tower's own count of lights and no other.
bool WriteBroken(void* tower, bool broken);
bool WriteLeverMoving(void* tower, bool moving);
bool WriteFuse(void* tower, int32_t slot, uint8_t value);
bool WriteLights(void* tower, const bool* lights, int32_t count);

// The tower's own painters and verbs, called as its graph calls them. MoveLever is its lever event: the lever's
// sound, `leverMoving`, then the timeline forward (up) or back. Scramble is the one roll, for the drills.
bool UpdBroken(void* tower);
bool UpdPuzzle(void* tower);
bool UpdFuses(void* tower);
bool SolvePuzzle(void* tower);
bool MoveLever(void* tower, bool up);
bool Scramble(void* tower);

enum class Sound : uint8_t { Click, Success, Fail, FusePulled, FuseInserted };
bool Play(void* tower, Sound sound);

// What a use acts on. getActionOptions writes it from the user's trace and the use reads it: a puzzle button by
// its index, the lever, the panel's retract buttons, a fuse slot by its index.
enum class Part : uint8_t { None, Button, Lever, Retract, Fuse };
struct LookAt {
    Part    part = Part::None;
    int32_t index = -1;  // Button and Fuse
};

// The look-at the tower holds now: the last getActionOptions' answer, or a use's written one.
bool ReadLookAt(void* tower, LookAt& out);

// The fuse slot an insert fills: the last lookAtFuseIndex, which the insert reads whatever the look-at since.
bool ReadFuseLook(void* tower, int32_t& slot);

// A use as `player` makes it: `at` written as getActionOptions writes it, then actionOptionIndex(player, a hit on
// the part, 4, the part), then the look-at the tower held put back, so another player's look survives it.
bool Use(void* tower, void* player, const LookAt& at);

// The world location of the part `at` names, where a use reaches. False while the part is not there.
bool PartLocation(void* tower, const LookAt& at, FVector& out);

// A good fuse, what the insert takes (a prop_fuse_C, as its cast reads it), and one spawned at `at`. A pulled
// fuse is the plain prop the pull spawns and names fuse_0.
bool IsFuse(void* actor);
void* SpawnFuse(const FVector& at);
bool IsPulledFuse(void* actor);

// [dev] the drills' insert as a player makes it: the fuse slot as the look-at, then playerUsedOn(player, a hit on
// the slot, the slot, `fuse`), which spends the fuse it is handed.
bool InsertFuse(void* tower, void* player, int32_t slot, void* fuse);

}  // namespace ue_wrap::coord_tower

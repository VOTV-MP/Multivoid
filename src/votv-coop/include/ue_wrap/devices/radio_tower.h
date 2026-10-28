// ue_wrap/devices/radio_tower.h -- the radio tower (radiotower_C): its state, its raw writes, and the tower's own
// painters and verbs.
//
// One tower, which the drone's shop run reads (`isBroken`). Like the coordinate towers it has fuses that blow
// before it breaks, a lights-out puzzle on a torus, a confirm lever whose timeline end judges the puzzle, and one
// roll; unlike them it has a second lever, whose timeline re-rolls, and no panel. One `animation` flag gates
// every use. Only `fuses` is saved. A player's use reads the look-at getActionOptions wrote from that player's
// trace. The instance is the gamemode's `radiotower`. Principle 7: no network or gameplay logic. Game thread.

#pragma once

#include "ue_wrap/core/types.h"  // FVector

#include <cstdint>

namespace ue_wrap::radio_tower {

inline constexpr int kMaxFuses = 8;
inline constexpr int kMaxCells = 25;  // the difficulty sizes the puzzle at 2x2 to 5x5

struct State {
    bool    isBroken = false;
    bool    animation = false;      // a lever runs; every use is ignored meanwhile
    bool    leverPlaying = false;   // TL_lever, the confirm lever's timeline; its forward end judges the puzzle
    bool    leverForward = false;   // TL_lever is not reversing
    bool    resetPlaying = false;   // TL_reset, the reset lever's timeline; its event re-rolls the tower
    uint8_t fuseCount = 0;
    uint8_t fuses[kMaxFuses] = {};  // 0 empty, 1 good, 2 blown
    uint8_t width = 0;              // puzzleSize.X
    uint8_t height = 0;             // puzzleSize.Y; cell i sits at row i / height, column i % height
    uint8_t cellCount = 0;          // puzzleData.Num
    bool    cells[kMaxCells] = {};  // puzzleData; the puzzle is solved when no cell is false
    bool    skipPuzzle = false;     // the tower's BeginPlay read the game rule enableLightsOut as off: a scramble sets
                                    // every cell and breaks, and the lever repairs with no press
};

// The gamemode's radiotower, or null while the gamemode or the tower is not there or the wrapper is latched off.
// Resolves the class's members from the tower it finds; a member missing from the loaded class latches the
// wrapper off with one warning naming it, since it will not appear later.
void* Instance();

// Whether `obj` is the radio tower. An unresolved wrapper resolves from the object's own class: a name compare and
// that class's property chain, never a walk of the object array, so it is safe inside a script-gate callback.
bool IsTower(void* obj);

bool Read(void* tower, State& out);

// Raw writes: nothing repaints. WriteCells takes the tower's own count of cells and no other.
bool WriteAnimation(void* tower, bool animation);
bool WriteFuse(void* tower, int32_t slot, uint8_t value);
bool WriteCells(void* tower, const bool* cells, int32_t count);

// The tower's own painters and verbs, called as its graph calls them. SetBroken is setBroken(broken, pullLever);
// CompleteMinigame lights every cell, repaints and judges; Scramble is the one roll, for the drills.
bool UpdPuzzle(void* tower);
bool UpdFuses(void* tower);
bool SetBroken(void* tower, bool broken, bool pullLever);
bool CompleteMinigame(void* tower);
bool Scramble(void* tower);

// A lever's timeline: the confirm lever plays forward (PlayFromStart) or back (ReverseFromEnd); the reset lever
// plays forward only, and a reverse of it is refused.
enum class Lever : uint8_t { Confirm, Reset };
bool PlayLever(void* tower, Lever lever, bool forward);

enum class Sound : uint8_t { Click, Success, Fail, FusePulled, FuseInserted, Lever, Scramble };
bool Play(void* tower, Sound sound);

// The SAT console's alert for this tower, as the tower's radioTowerBroken dispatcher delivers it: the handler
// ui_console_C::radioTowerBroken(tower, damagedHealth) called on the live console (a hint "Radio tower has been
// damaged!" or "is down!", and the console's own wake-up of the player). ue_wrap has no delegate broadcast, and the
// handler is a plain Blueprint event. False while the console is not there.
bool Alert(void* tower, bool damagedHealth);

// What a use acts on. getActionOptions writes it from the user's trace and the use reads it: a puzzle button by
// its index, the confirm lever, the reset lever, a fuse slot by its index.
enum class Part : uint8_t { None, Button, Confirm, Reset, Fuse };
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

// An insert as `player` makes it: `Fuse` slot `slot` written as the look-at, then playerUsedOn(player, a hit on the
// slot's component, that component, `fuse`), then the look-at the tower held put back. False while the slot is not
// there or the call did not run.
bool InsertFuse(void* tower, void* player, int32_t slot, void* fuse);

}  // namespace ue_wrap::radio_tower

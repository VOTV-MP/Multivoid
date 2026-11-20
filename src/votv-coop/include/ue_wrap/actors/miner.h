// ue_wrap/actors/miner.h -- engine access for the GPU mining rig (prop_miner_C) and the three items its use takes.
// Principle-7 engine-wrapper layer: no network or coop state.
//
// A rig keeps what it holds in its own members: `upg_slots` slots, `upg_gpu` GPUs in them and `upg_fans` coolers,
// plus `accumulatedPoints` it has mined and the `overheat` it has built up; `powered` says its cord is live. Its
// save record carries the five numbers: ints group 0 holds [upg_gpu, upg_slots, upg_fans, accumulatedPoints] and
// floats group 1 holds [overheat] (getData and loadData; group 0 of the floats is the prop base's own). The rig's
// `upgrade(name)` takes one of three hold names, adds what it names and says whether it did; `upd` repaints the
// GPUs, slots and coolers from the counts and `plugged` repaints the monitor and the sounds from `powered`;
// `actionOptionIndex` with action 4 pays the points into the player's balance and empties them.
//
// A Blueprint class dies with the world that loaded it, so a class is known here by its name, and a member's place is
// read off the live instance's class and kept.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::save_record { struct SaveRecord; }

namespace ue_wrap::miner {

// What a rig holds.
struct Fields {
    int32_t gpus = 0;
    int32_t slots = 0;
    int32_t fans = 0;
    int32_t points = 0;
    float overheat = 0.f;
    bool powered = false;
};

// The three items a rig's use takes, by number: 0 scrap metal, 1 a GPU, 2 a cooler.
inline constexpr uint8_t kItemSlot = 0, kItemGpu = 1, kItemFan = 2;
inline constexpr uint8_t kItemCount = 3;

// The hold name the player's hand shows an item under (the key `upgrade` takes), and the class the hand lane announces
// it as. Null for a number out of range.
const wchar_t* ItemHoldName(uint8_t item);
const wchar_t* ItemClass(uint8_t item);

// The item a hold name is, compared without case (an FName keeps its first spelling), or -1.
int ItemOfHoldName(const std::wstring& name);

// Resolve the class name. Dispatches the string-to-name conversion, so call it from a top-level game-thread pass, never
// from inside a hook. Idempotent; true once resolved.
bool ResolveNames();

// True iff `obj` is a rig (prop_miner_C or a subclass). False before ResolveNames. Game thread.
bool IsMiner(void* obj);

// The rig's members; false, with `out` untouched, for anything that is not a rig or a member that does not resolve.
// Write puts the five numbers and `powered` in place with no call and none of the game's own side effects. Game thread.
bool Read(void* rig, Fields& out);
bool Write(void* rig, const Fields& f);

// The game's own verbs on a rig. CallUpgrade runs `upgrade(<the item's hold name>)` and returns what the game said:
// whether the rig took it. CallClaim runs the claim (`actionOptionIndex`, action 4) with `player` as the claimant, whose
// balance is paid; true is that the call ran, not that points were paid. CallUpd repaints the GPUs, slots and coolers,
// CallPlugged the monitor and sounds. False when a call does not run. Game thread.
bool CallUpgrade(void* rig, uint8_t item);
bool CallClaim(void* rig, void* player);
bool CallUpd(void* rig);
bool CallPlugged(void* rig);

// The player's hand as the rig's use reads it: `holding_name` and `holding_actor`. False for no player or a player
// without the members. Game thread.
bool ReadHold(void* player, std::wstring& name, void*& actor);

// Every live rig, in no particular order. Game thread; the callback must not create or destroy objects.
using RigFn = void (*)(void* ctx, void* rig);
void ForEachRig(RigFn fn, void* ctx);

// A record's layout and numbers: false when the record is not a rig's layout (the ints or floats groups missing or too
// short); otherwise `out` holds the five numbers, and `powered` is false. Pure. Any thread.
bool ReadRecord(const ue_wrap::save_record::SaveRecord& rec, Fields& out);

// A new item of the kind `item` at `at`, the loose prop a hand holds or a rig takes; the item's class is loaded when this
// process has not loaded it. The actor, or null (the class did not load, or the world refused the spawn). Game thread.
void* SpawnItem(uint8_t item, const FVector& at);

}  // namespace ue_wrap::miner
